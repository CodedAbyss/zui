#include <stdbool.h>

#ifndef TYPES_INCLUDED
#define TYPES_INCLUDED
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned long u32;
typedef unsigned long long u64;

typedef char i8;
typedef short i16;
typedef int i32;
typedef long long i64;

typedef float f32;
typedef double f64;
#endif TYPES_INCLUDED

#ifndef ZUI_INCLUDED
#define ZUI_INCLUDED

#ifdef __cplusplus 
extern "C" {
#endif

#define Z_NONE -3
#define Z_AUTO -2
#define Z_FILL -1
#define ZV_AUTO (zvec2) { Z_AUTO, Z_AUTO }
#define ZV_FILL (zvec2) { Z_FILL, Z_FILL }

typedef struct zcolor { u8 r, g, b, a; } zcolor;
typedef struct zvec2 { union { struct { u16 x, y; }; u16 e[2]; }; } zvec2;
typedef struct zrect { union { struct { u16 x, y, w, h; }; u16 e[4]; }; } zrect;
typedef struct zfont {
    i32 id;
    i32 bytes;
    zvec2 (*text_size)(struct zfont *font, char *str, i32 len);
} zfont;

//typedef struct zapp_desc {
//    i32 width;
//    i32 height;
//    char *name;
//    void *user_data;
//    void *instance;
//    void(*init)(void *user_data);
//    void(*frame)(float ts, void *user_data);
//    void(*close)(void *user_data);
//} zapp_desc;
//i32 zapp_width();
//i32 zapp_height();
//void zapp_render();
//void zapp_close();
//void zapp_log(const char *fmt, ...);
//zfont *zapp_font(const char *name, i32 size);
//void zapp_launch(zapp_desc *description);

enum ZUI_KEYS {
    ZK_L_SHIFT = 1 << 0,
    ZK_R_SHIFT = 1 << 1,
    ZK_SHIFT = ZK_L_SHIFT | ZK_R_SHIFT,
    ZK_L_CTRL = 1 << 2,
    ZK_R_CTRL = 1 << 3,
    ZK_CTRL = ZK_L_CTRL | ZK_R_CTRL,
    ZK_L_ALT = 1 << 4,
    ZK_R_ALT = 1 << 5,
    ZK_ALT = ZK_L_ALT | ZK_R_ALT,
    ZK_SUPER = 1 << 6,
#if defined(_WIN32)
    ZK_SHORTCUT = ZK_CTRL,
#elif defined(__APPLE__) && defined(__MACH__)
    ZK_SHORTCUT = ZK_SUPER,
#endif
};

enum ZUI_MOUSE {
    ZM_LEFT_CLICK = 1,
    ZM_RIGHT_CLICK = 2,
    ZM_MIDDLE_CLICK = 4
};

enum ZUI_WIDGETS {
	ZW_FIRST,
	ZW_BLANK = ZW_FIRST,
	ZW_BOX,
	ZW_POPUP,
	ZW_LABEL,
	ZW_ROW,
	ZW_COL,
	ZW_BTN,
	ZW_CHECK,
	ZW_TEXT,
	ZW_COMBO,
	ZW_GRID
};

enum ZUI_FLAGS {
	ZJ_CENTER = 0, // justification flags
	ZJ_LEFT = 1,
	ZJ_RIGHT = 2,
	ZJ_UP = 4,
	ZJ_DOWN = 8,
	ZF_PARENT = 16, // a container with children
	ZF_TABBABLE = 32, // pressing tab can focus to this element
};

typedef struct zcmd { u16 id, bytes; } zcmd;

// commands generated by the server
enum ZUI_SERVER_CMDS {
	ZSCMD_DRAW,
	ZSCMD_CLIP,
	ZSCMD_RECT,
	ZSCMD_TEXT,
	ZSCMD_COPY,
	ZSCMD_FONT,
	ZSCMD_GLYPH,
};

typedef struct zglyph_data { u16 font_id; u8 width; char c; } zglyph_data;
typedef struct zscmd_clip { zcmd header; zrect rect; } zscmd_clip;                                          // set clip rect
typedef struct zscmd_rect { zcmd header; zrect rect; zcolor color; } zscmd_rect;                            // draw rect
typedef struct zscmd_text { zcmd header; zvec2 pos;  zcolor color; u16 font_id; char text[0]; } zscmd_text; // draw text
typedef struct zscmd_copy { zcmd header; char text[0]; } zscmd_copy;                                        // send copied text to client
typedef struct zscmd_font { zcmd header; u16 font_id; u16 size; char family[0]; } zscmd_font;               // register font
typedef struct zscmd_glyph { zcmd header; zglyph_data c; } zscmd_glyph;
typedef union zscmd {
	zcmd        base;
	zscmd_clip  clip;
	zscmd_rect  rect;
	zscmd_text  text;
	zscmd_copy  copy;
	zscmd_font  font;
	zscmd_glyph glyph;
} zscmd;

// commands generated by the client
enum ZUI_CLIENT_CMDS {
	ZCCMD_MOUSE,
	ZCCMD_KEYS,
	ZCCMD_GLYPH,
	ZCCMD_WIN
};

typedef struct zccmd_mouse { zcmd header; zvec2 pos; u16 state; } zccmd_mouse;      // mouse movement / state
typedef struct zccmd_keys { zcmd header; u16 modifiers; char key; } zccmd_keys;     // key presses
typedef struct zccmd_glyph { zcmd header; zglyph_data c; } zccmd_glyph;             // sent every time a new glyph needs to be displayed
typedef struct zccmd_win { zcmd header; zvec2 sz; } zccmd_win;                      // new window size
typedef union zccmd {
	zcmd        base;
	zccmd_mouse mouse;
	zccmd_keys  keys;
	zccmd_glyph glyph;
	zccmd_win   win;
} zccmd;

typedef struct zcmd_widget { u16 id, bytes; i32 next, zindex, flags; zrect bounds; zrect used; } zcmd_widget;

typedef struct zs_text { i32 flags, index, ofs, selection; } zs_text;
typedef struct zcmd_text { zcmd_widget _; char *buffer; i32 len; zs_text *state; } zcmd_text;
typedef struct zcmd_btn { zcmd_widget _; u8 *state; } zcmd_btn;
typedef struct zcmd_check { zcmd_widget _; u8 *state; } zcmd_check;
typedef struct zcmd_combo { zcmd_widget _; char *tooltip, *csoptions; i32 *state; } zcmd_combo;
typedef struct zcmd_label { zcmd_widget _; char *text;  i32 len; } zcmd_label;
typedef struct zcmd_box { zcmd_widget _; } zcmd_box;
typedef struct zcmd_popup { zcmd_widget _; } zcmd_popup;
typedef struct zcmd_layout { zcmd_widget _; i32 count; float data[1]; } zcmd_layout;
typedef struct zcmd_grid { zcmd_widget _; u8 rows, cols, padx, pady; float data[1]; } zcmd_grid;

typedef void(*zui_server_fn)(zscmd *cmd, void *user_data);
typedef void(*zui_client_fn)(zccmd *cmd, void *user_data);

// CLIENT COMMANDS
void zui_client_init(zui_client_fn send, zui_server_fn recv, void *user_data);
void zui_client_process(char *bytes, i32 len);
void zui_client_render();

void zui_mouse_down(u16 btn);
void zui_mouse_up(u16 btn);
void zui_mouse_move(zvec2 pos);
void zui_key_char(char c);
void zui_resize(u16 width, u16 height);

// SERVER COMMANDS
void zui_init(zui_server_fn renderer, void *user_data);

void zui_server_init(zui_server_fn send, zui_client_fn recv, void *user_data);
void zui_server_render();
void zui_render();
u32 zui_ms();

void zui_print_tree();

void zui_close();
void zui_blank();
void zui_box();
void zui_popup();
void zui_justify(u32 justification);
void zui_size(i32 w, i32 h);
void zui_font(zfont *font);
void zui_end();
void zui_window(i32 width, i32 height, float ts);
void zui_label(const char *text);
void zui_sliderf(char *tooltip, f32 min, f32 max, f32 *value);
void zui_slideri(char *tooltip, i32 min, i32 max, i32 *value);
i32  zui_combo(char *tooltip, char *csoptions, i32 *state);
bool zui_button(const char *text, u8 *state);
bool zui_check(u8 *state);
void zui_validator(bool(*validator)(char *text));
void zui_text(char *buffer, i32 len, zs_text *state);
void zui_textbox(char *buffer, i32 len, i32 *state);
void zui_col(i32 n, float *heights);
void zui_row(i32 n, float *widths);

#ifdef __cplusplus 
}
#endif

#endif
