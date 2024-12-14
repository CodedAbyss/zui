#pragma once

#define WIN32_LEAN_AND_MEAN
#define ZAPP_DECLARE
#include <Windows.h>
#include <stdlib.h>
#include "zui/src/zui.h"

typedef struct zapp_gdi {
	HDC memory_dc;
	HDC window_dc;
	HBITMAP bitmap;
	HWND wnd;
	i32 width;
	i32 height;
	bool running;
} zapp_gdi;

static zapp_gdi app_ctx;


static void zui_recv_clipboard(char *text, i32 len) {
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

static void zui_send_clipboard() {
	if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(0)) return;
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
			char *utf8 = _alloca(utf8size);
			WideCharToMultiByte(CP_UTF8, 0, wstr, wsize, utf8, utf8size, 0, 0);
			while(*utf8)
				zui_input_char(*utf8++);	
		} while (0);
		GlobalUnlock(mem);
	} while (0);
	CloseClipboard();
}

static LRESULT CALLBACK WindowProc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam)
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
	} break;
	case WM_PAINT: {
		PAINTSTRUCT paint;
		HDC dc = BeginPaint(wnd, &paint);
		BitBlt(dc, 0, 0, 600, 400, app_ctx.memory_dc, 0, 0, SRCCOPY);
		EndPaint(wnd, &paint);
		return 0;
	}
	case WM_KEYDOWN: {
		int ctrl = GetKeyState(VK_CONTROL) & (1 << 15);
		switch (wparam) {
		case 'C': if (ctrl) zui_input_copy(zui_recv_clipboard); return 0;
		case 'V': if (ctrl) zui_send_clipboard(); return 0;
		case 'A': if (ctrl) zui_input_select(); return 0;
		case VK_TAB: zui_input_char('\t'); return 0;
		case VK_BACK: zui_input_char('\b'); return 0;
		case VK_DELETE: zui_input_char(127); return 0;
		case VK_LEFT: zui_input_char(17); return 0;
		case VK_RIGHT: zui_input_char(18); return 0;
		case VK_UP: zui_input_char(19); return 0;
		case VK_DOWN: zui_input_char(20); return 0;
		}
	} break;
	case WM_CHAR: if (wparam >= 32 && wparam <= 127) { zui_input_char((char)wparam); return 0; } break;
	case WM_RBUTTONDOWN: zui_input_mousedown(ZM_RIGHT_CLICK); SetCapture(wnd);  return 0;
	case WM_RBUTTONUP:   zui_input_mouseup(ZM_RIGHT_CLICK); ReleaseCapture();   return 0;
	case WM_LBUTTONDOWN: zui_input_mousedown(ZM_LEFT_CLICK); SetCapture(wnd);   return 0;
	case WM_LBUTTONUP:   zui_input_mouseup(ZM_LEFT_CLICK); ReleaseCapture();    return 0;
	case WM_MBUTTONDOWN: zui_input_mousedown(ZM_MIDDLE_CLICK); SetCapture(wnd); return 0;
	case WM_MBUTTONUP:   zui_input_mouseup(ZM_MIDDLE_CLICK); ReleaseCapture();  return 0;
	case WM_MOUSEMOVE:   zui_input_mousemove((zvec2) { LOWORD(lparam), HIWORD(lparam) }); return 0;
	}

	return DefWindowProcW(wnd, msg, wparam, lparam);
}

typedef struct GdiFont {
	zfont header;
	HFONT handle;
	HDC dc;
	i32 height;
} GdiFont;

zvec2 __zapp_font_size(zfont *font, char *text, i32 len) {
	GdiFont *gdifont = (GdiFont*)font;
	if (!gdifont || !text)
		return (zvec2) { 0 };
	SIZE size;
	i32 wsize = MultiByteToWideChar(CP_UTF8, 0, text, len, NULL, 0);
	WCHAR *wstr = (WCHAR*)_alloca(wsize * sizeof(wchar_t));
	MultiByteToWideChar(CP_UTF8, 0, text, len, wstr, wsize);
	if (GetTextExtentPoint32W(gdifont->dc, wstr, wsize, &size))
		return (zvec2) { size.cx, gdifont->height };
	return (zvec2) { 0 };
}

zfont *zapp_font(char *name, i32 size) {
	TEXTMETRICW metric;
	GdiFont *font = (GdiFont*)calloc(1, sizeof(GdiFont));
	if (!font)
		return NULL;
	font->dc = CreateCompatibleDC(0);
	font->handle = CreateFontA(size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, name);
	SelectObject(font->dc, font->handle);
	GetTextMetricsW(font->dc, &metric);
	font->height = metric.tmHeight;
	font->header.id = 0;
	font->header.bytes = sizeof(GdiFont);
	font->header.text_size = __zapp_font_size;
	return &font->header;
}

void zapp_font_free(zfont *font) {
	GdiFont *gdifont = (GdiFont*)font;
	gdifont->dc;
	DeleteObject(gdifont->handle);
	free(gdifont);
}

i32 zapp_width() { return app_ctx.width; }

i32 zapp_height() { return app_ctx.height; }

void zapp_render() {
	SelectObject(app_ctx.memory_dc, GetStockObject(DC_PEN));
	SelectObject(app_ctx.memory_dc, GetStockObject(DC_BRUSH));

	zcmd_draw *cmd;
	while ((cmd = zui_draw_next())) {
		switch (cmd->base.id) {
		case ZCMD_CLIP: {
			zrect clip = cmd->clip.cliprect;
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
			if (!cmd->text.text || !cmd->text.font || !cmd->text.len) break;

			int wsize = MultiByteToWideChar(CP_UTF8, 0, cmd->text.text, cmd->text.len, NULL, 0);
			WCHAR *wstr = (WCHAR*)_alloca(wsize * sizeof(wchar_t));
			MultiByteToWideChar(CP_UTF8, 0, cmd->text.text, cmd->text.len, wstr, wsize);

			zcolor c = cmd->text.color;
			COLORREF color = c.r | (c.g << 8) | (c.b << 16);
			SetTextColor(app_ctx.memory_dc, color);

			SetBkMode(app_ctx.memory_dc, TRANSPARENT);
			SelectObject(app_ctx.memory_dc, ((GdiFont*)cmd->text.font)->handle);
			ExtTextOutW(app_ctx.memory_dc, cmd->text.coord.x, cmd->text.coord.y, 0, NULL, wstr, wsize, NULL);
		} break;
		}
	}
	BitBlt(app_ctx.window_dc, 0, 0, app_ctx.width, app_ctx.height, app_ctx.memory_dc, 0, 0, SRCCOPY);
}

void zapp_launch(zapp_desc *description) {
	GdiFont* font;

	app_ctx.width = description->width;
	app_ctx.height = description->height;
	app_ctx.running = true;
	RECT rect = { 0, 0, app_ctx.width, app_ctx.height };
	DWORD style = WS_OVERLAPPEDWINDOW;
	DWORD exstyle = WS_EX_APPWINDOW;

	WNDCLASSW wc = { 0 };
	wc.style = CS_DBLCLKS;
	wc.lpfnWndProc = WindowProc;
	wc.hInstance = GetModuleHandleW(0);
	wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.lpszClassName = L"ZappWindowClass";
	ATOM atom = RegisterClassW(&wc);

	i32 len = strlen(description->name) + 1;
	i32 wsize = MultiByteToWideChar(CP_UTF8, 0, description->name, len, NULL, 0);
	WCHAR *title = (WCHAR*)_alloca(wsize * sizeof(wchar_t));
	MultiByteToWideChar(CP_UTF8, 0, description->name, len, title, wsize);

	AdjustWindowRectEx(&rect, style, FALSE, exstyle);
	app_ctx.wnd = CreateWindowExW(exstyle, wc.lpszClassName, title, style | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, wc.hInstance, NULL);
	app_ctx.window_dc = GetDC(app_ctx.wnd);

	app_ctx.bitmap = CreateCompatibleBitmap(app_ctx.window_dc, description->width, description->height);
	app_ctx.memory_dc = CreateCompatibleDC(app_ctx.window_dc);
	SelectObject(app_ctx.memory_dc, app_ctx.bitmap);

	i32 needs_refresh = 0;
	zui_init();

	description->init(description->user_data);
	u32 prev_ts = GetTickCount();
	while (app_ctx.running)
	{
		MSG msg;
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

		u32 ts = GetTickCount();
		if (ts - prev_ts >= 16) {
			description->frame((float)ts / 1000.0f, description->user_data);
			prev_ts = ts;
		}
	}
	description->close(description->user_data);

	ReleaseDC(app_ctx.wnd, app_ctx.window_dc);
	UnregisterClassW(wc.lpszClassName, wc.hInstance);
}

void zapp_close() {
	app_ctx.running = false;
}

