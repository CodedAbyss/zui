#ifndef ZUI_INCLUDED
#error Must include zui.h before zui-gdi.h
#else
#ifdef ZUI_IMPL
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#ifndef ZUI_UTF8
#error Must Define ZUI_UTF8. GDI backend relies on utf8 functions.
#endif
#else
void gdi_renderer(zcmd_any *cmd, void *user_data);
#endif
typedef struct zui_gdi_args {
    i32 width;
    i32 height;
    char *title;
    zui_init_fn init;    
    zui_frame_fn frame;
    zui_close_fn close;
    bool tick_manually;
} zui_gdi_args;
#ifdef ZUI_IMPL
typedef struct zui_gdi_ctx {
	HDC memory_dc;
	HDC window_dc;
    HFONT font_list[10];
    HDC   font_dc[10];
	HBITMAP bitmap;
    WNDCLASSW wnd_class;
	HWND wnd;
	i32 width;
	i32 height;    
	bool running;
} zui_gdi_ctx;
static zui_gdi_ctx app_ctx;
static void _win32_set_clipboard(char *text, i32 len) {
	if (!OpenClipboard(0)) return;
	do {
		i32 wsize = MultiByteToWideChar(CP_UTF8, 0, text, len, 0, 0);
		if (!wsize) break;
		HGLOBAL mem = (HGLOBAL)GlobalAlloc(GMEM_MOVEABLE, (wsize + 1) * sizeof(wchar_t));
		if (!mem) break;
		wchar_t* wstr = (wchar_t*)GlobalLock(mem);
		if (!wstr) break;
		MultiByteToWideChar(CP_UTF8, 0, text, len, wstr, wsize);
		wstr[wsize] = 0;
		GlobalUnlock(mem);
		EmptyClipboard();
		SetClipboardData(CF_UNICODETEXT, mem);
	} while (0);
	CloseClipboard();
}

static char *_win32_get_clipboard() {
    static char *clipboard = 0;
    bool success = false;
	if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(0)) return "";
	do {
		HGLOBAL mem = GetClipboardData(CF_UNICODETEXT);
		if (!mem) break;
		i32 wsize = GlobalSize(mem) / sizeof(wchar_t);
		if (!wsize) break;
		wchar_t* wstr = (wchar_t*)GlobalLock(mem);
		if (!wstr) break;
		do {
			i32 utf8size = WideCharToMultiByte(CP_UTF8, 0, wstr, wsize, 0, 0, 0, 0);
			if (!utf8size) break;
            if(clipboard) free(clipboard);
            clipboard = malloc(utf8size + 1);
			WideCharToMultiByte(CP_UTF8, 0, wstr, wsize, clipboard, utf8size, 0, 0);
            clipboard[utf8size] = 0;
            success = true;
		} while (0);
		GlobalUnlock(mem);
	} while (0);
	CloseClipboard();
    return success ? clipboard : "";
}

static LRESULT CALLBACK _win32_event(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	switch (msg)
	{
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	case WM_SIZE: {
		u16 width = LOWORD(lparam);
		u16 height = HIWORD(lparam);
		if (width == app_ctx.width && height == app_ctx.height)
			break;
		DeleteObject(app_ctx.bitmap);
		app_ctx.bitmap = CreateCompatibleBitmap(app_ctx.window_dc, width, height);
		app_ctx.width = width;
		app_ctx.height = height;
		SelectObject(app_ctx.memory_dc, app_ctx.bitmap);
        zui_resize(width, height);
	} break;
	case WM_PAINT: {
		PAINTSTRUCT paint;
		HDC dc = BeginPaint(wnd, &paint);
		BitBlt(dc, 0, 0, 600, 400, app_ctx.memory_dc, 0, 0, SRCCOPY);
		EndPaint(wnd, &paint);
		return 0;
	}
	case WM_KEYDOWN: {
		switch (wparam) {
		case VK_TAB: zui_key_char('\t'); break;
		case VK_BACK: zui_key_char('\b'); break;
		case VK_DELETE: zui_key_char(127); break;
		case VK_LEFT: zui_key_char(17); break;
		case VK_RIGHT: zui_key_char(18); break;
		case VK_UP: zui_key_char(19); break;
		case VK_DOWN: zui_key_char(20); break;
		}
	}
	case WM_KEYUP: {
        u16 mod = 0;
        mod |= (GetKeyState(VK_LCONTROL) >> 15) * ZK_L_CTRL;
        mod |= (GetKeyState(VK_RCONTROL) >> 15) * ZK_R_CTRL;
        mod |= (GetKeyState(VK_LSHIFT) >> 15) * ZK_L_SHIFT;
        mod |= (GetKeyState(VK_RSHIFT) >> 15) * ZK_R_SHIFT;
        mod |= (GetKeyState(VK_LMENU) >> 15) * ZK_L_ALT;
        mod |= (GetKeyState(VK_RMENU) >> 15) * ZK_R_ALT;
        mod |= (GetKeyState(VK_CAPITAL) & 1) * ZK_CAPSLOCK;
        zui_key_mods(mod);
    } break;
	case WM_CHAR: if (wparam >= 32 && wparam <= 127) { zui_key_char((i32)wparam); return 0; } break;
	case WM_RBUTTONDOWN: zui_mouse_down(ZM_RIGHT_CLICK); SetCapture(wnd);  return 0;
	case WM_RBUTTONUP:   zui_mouse_up(ZM_RIGHT_CLICK); ReleaseCapture();   return 0;
	case WM_LBUTTONDOWN: zui_mouse_down(ZM_LEFT_CLICK); SetCapture(wnd);   return 0;
	case WM_LBUTTONUP:   zui_mouse_up(ZM_LEFT_CLICK); ReleaseCapture();    return 0;
	case WM_MBUTTONDOWN: zui_mouse_down(ZM_MIDDLE_CLICK); SetCapture(wnd); return 0;
	case WM_MBUTTONUP:   zui_mouse_up(ZM_MIDDLE_CLICK); ReleaseCapture();  return 0;
	case WM_MOUSEMOVE:   zui_mouse_move((zvec2) { LOWORD(lparam), HIWORD(lparam) }); return 0;
	}

	return DefWindowProcW(wnd, msg, wparam, lparam);
}
void _win32_tick(zui_gdi_args *args, bool blocking) {
    if(!app_ctx.running) return;
    MSG msg;
    static u32 needs_refresh = 0;
    if (needs_refresh == 0) {
        if (GetMessageW(&msg, NULL, 0, 0) <= 0)
            app_ctx.running = false;
        else {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        needs_refresh = 1;
    }
    else needs_refresh = 0;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT)
            app_ctx.running = false;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        needs_refresh = 1;
    }    
    args->frame(0);
}
void _win32_close(zui_gdi_args *args) {
    args->close(0);
    ReleaseDC(app_ctx.wnd, app_ctx.window_dc);
    UnregisterClassW(app_ctx.wnd_class.lpszClassName, app_ctx.wnd_class.hInstance);
}
void _win32_setup(zui_gdi_args *args) {
	app_ctx.width = args->width;
	app_ctx.height = args->height;
	app_ctx.running = true;
	RECT rect = { 0, 0, app_ctx.width, app_ctx.height };
	DWORD style = WS_OVERLAPPEDWINDOW;
	DWORD exstyle = WS_EX_APPWINDOW;

    app_ctx.wnd_class = (WNDCLASSW) { 0 };
	app_ctx.wnd_class.style = CS_DBLCLKS;
	app_ctx.wnd_class.lpfnWndProc = _win32_event;
	app_ctx.wnd_class.hInstance = GetModuleHandleW(0);
	app_ctx.wnd_class.hIcon = LoadIcon(NULL, IDI_APPLICATION);
	app_ctx.wnd_class.hCursor = LoadCursor(NULL, IDC_ARROW);
	app_ctx.wnd_class.lpszClassName = L"ZappWindowClass";
	ATOM atom = RegisterClassW(&app_ctx.wnd_class);

	i32 len = strlen(args->title) + 1;
	i32 wsize = MultiByteToWideChar(CP_UTF8, 0, args->title, len, NULL, 0);
	WCHAR *title = (WCHAR*)_alloca(wsize * sizeof(wchar_t));
	MultiByteToWideChar(CP_UTF8, 0, args->title, len, title, wsize);
	AdjustWindowRectEx(&rect, style, FALSE, exstyle);
	app_ctx.wnd = CreateWindowExW(exstyle,
        app_ctx.wnd_class.lpszClassName,
        title,
        style | WS_VISIBLE,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        NULL,
        NULL,
        app_ctx.wnd_class.hInstance,
        NULL);
	app_ctx.window_dc = GetDC(app_ctx.wnd);
	app_ctx.bitmap = CreateCompatibleBitmap(app_ctx.window_dc, args->width, args->height);
	app_ctx.memory_dc = CreateCompatibleDC(app_ctx.window_dc);
	SelectObject(app_ctx.memory_dc, app_ctx.bitmap);
    zui_resize(args->width, args->height);
    args->init(0);
    if(!args->tick_manually) {
        while(app_ctx.running)
            _win32_tick(args, true);
        _win32_close(args);
    }
}
// TODO:
// We can allow delayed responses to the renderer (in cases like network latency)
// By doing a setjmp before the non-blocking check for data
// Then we do a long jump back to the caller of zui_tick() after setting a "pending" context flag
// Subsequent calls to zui_tick() will check for the "pending" flag and jump to the set point
// Upon receiving the info, execution will continue normally
// Question: Is this fool proof, or does it mess up a stack?
// I'd think as long as zui_tick() isn't allowed to change consequential variables it'll work
void gdi_renderer(zcmd_any *cmd, void *user_data) {
    switch(cmd->base.id) {
        case ZCMD_INIT: _win32_setup((zui_gdi_args*)user_data); break;
        case ZCMD_TICK: _win32_tick((zui_gdi_args*)user_data, true); break;
        case ZCMD_CLOSE: _win32_close((zui_gdi_args*)user_data); break;
        case ZCMD_RENDER_BEGIN:
            // set pen and brush for drawing
            SelectObject(app_ctx.memory_dc, GetStockObject(DC_PEN));
            SelectObject(app_ctx.memory_dc, GetStockObject(DC_BRUSH));
            break;
        case ZCMD_RENDER_END:
            // copy data to screen
            BitBlt(app_ctx.window_dc, 0, 0, app_ctx.width, app_ctx.height, app_ctx.memory_dc, 0, 0, SRCCOPY);
            break;
        case ZCMD_GET_CLIPBOARD: {
            cmd->get_clipboard.response = _win32_get_clipboard();
        } break;
        case ZCMD_SET_CLIPBOARD: {
            i32 size = cmd->base.bytes - sizeof(zcmd_set_clipboard);
            _win32_set_clipboard(cmd->set_clipboard.text, size);
        } break;
        case ZCMD_REG_FONT: {
            TEXTMETRICW metric;
            HDC dc = CreateCompatibleDC(0);
            HANDLE handle = CreateFontA(cmd->font.size,
                0,
                0,
                0,
                FW_NORMAL,
                FALSE,
                FALSE,
                FALSE,
                DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE,
                cmd->font.family);
            if(handle) {
                SelectObject(dc, handle);
                GetTextMetricsW(dc, &metric);
                app_ctx.font_list[cmd->font.font_id] = handle;
                app_ctx.font_dc[cmd->font.font_id] = dc;
            }
            cmd->font.response = (handle != 0);
        } break;
        case ZCMD_GLYPH_SZ: {
            u16 font_id = cmd->glyph.c.font_id;
            WCHAR wstr[4]; // should be more than enough for a utf8 codepoint
            char text[4];
            i32 len = utf8_len(cmd->glyph.c.c);
            utf8_print(text, cmd->glyph.c.c, len);
            SIZE size;
            i32 wsize = MultiByteToWideChar(CP_UTF8, 0, text, len, wstr, 4); 
            if (GetTextExtentPoint32W(app_ctx.font_dc[font_id], wstr, wsize, &size))
                cmd->glyph.c.width = size.cx;
        } break;
		case ZCMD_DRAW_CLIP: {
			zrect clip = cmd->clip.rect;
			SelectClipRgn(app_ctx.memory_dc, 0);
			IntersectClipRect(app_ctx.memory_dc, clip.x, clip.y, clip.x + clip.w, clip.y + clip.h);
		} break;
		case ZCMD_DRAW_RECT: {
			zcolor c = cmd->rect.color;
			zrect r = cmd->rect.rect;
			COLORREF color = c.r | (c.g << 8) | (c.b << 16);
			RECT rect = { r.x, r.y, r.x + r.w, r.y + r.h };
			SetBkColor(app_ctx.memory_dc, color);
			ExtTextOutW(app_ctx.memory_dc, 0, 0, ETO_OPAQUE, &rect, NULL, 0, NULL);
		} break;
		case ZCMD_DRAW_TEXT: {
            int len = cmd->base.bytes - sizeof(zcmd_text);
			int wsize = MultiByteToWideChar(CP_UTF8, 0, cmd->text.text, len, NULL, 0);
			WCHAR *wstr = (WCHAR*)_alloca(wsize * sizeof(wchar_t));
			MultiByteToWideChar(CP_UTF8, 0, cmd->text.text, len, wstr, wsize);
			zcolor c = cmd->text.color;
			COLORREF color = c.r | (c.g << 8) | (c.b << 16);
			SetTextColor(app_ctx.memory_dc, color);
			SetBkMode(app_ctx.memory_dc, TRANSPARENT);
			SelectObject(app_ctx.memory_dc, app_ctx.font_list[cmd->text.font_id]);
			ExtTextOutW(app_ctx.memory_dc, cmd->text.pos.x, cmd->text.pos.y, 0, NULL, wstr, wsize, NULL);
		} break;    
        case ZCMD_DRAW_BEZIER: {
            int cnt = (cmd->base.bytes - sizeof(zcmd_bezier)) / sizeof(zvec2);
            POINT *points = _alloca(sizeof(POINT) * cnt);
            for(i32 i = 0; i < cnt; i++) {
                zvec2 v = cmd->bezier.points[i];
                points[i] = (POINT) { v.x, v.y };
            }
            PolyBezier(app_ctx.memory_dc, points, cnt);
        } break;
    }
}
#endif
#endif
