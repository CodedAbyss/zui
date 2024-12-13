#include <stdbool.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

typedef char i8;
typedef short i16;
typedef int i32;
typedef long long i64;

typedef float f32;
typedef double f64;

#ifndef ZUI_INCLUDED
#define ZUI_INCLUDED

#define Z_NONE -3
#define Z_AUTO -2
#define Z_FILL -1
#define ZV_AUTO (zvec2) { Z_AUTO, Z_AUTO }
#define ZV_FILL (zvec2) { Z_FILL, Z_FILL }

typedef struct zcolor { u8 r, g, b, a; } zcolor;
typedef struct zvec2 { i32 x, y; } zvec2;
typedef struct zrect { i32 x, y, w, h; } zrect;
typedef struct zfont {
	i32 id;
	i32 bytes;
    zvec2 (*text_size)(struct zfont *font, char *str, i32 len);
} zfont;

typedef struct zrenderer {
	void (*render)(void *data);
	void *data;
} zrenderer;

#ifdef ZAPP_DECLARE
typedef struct zapp_desc {
	i32 width;
	i32 height;
	char *name;
	void *user_data;
	void(*init)(void *user_data);
	void(*frame)(float ts, void *user_data);
	void(*close)(void *user_data);
} zapp_desc;
i32 zapp_width();
i32 zapp_height();
void zapp_render();
zfont *zapp_font(char *name, i32 size);
void zapp_launch(zapp_desc *description);
#endif

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
enum ZUI_UI_CMDS {
    ZU_JUSTIFY = 0,
    ZU_SET_SIZE
};
enum ZUI_WIDGETS {
    ZW_FIRST = ZU_SET_SIZE + 1,
    ZW_BLANK = ZW_FIRST,
    ZW_BOX,
    ZW_POPUP,
    ZW_LABEL,
    ZW_ROW,
    ZW_COL,
    ZW_BTN,
    ZW_TEXT,
    ZW_COMBO,
    ZW_GRID
};
enum ZUI_CMDS {
    ZCMD_DRAW_RECT,
    ZCMD_DRAW_TEXT,
    ZCMD_CLIP,
};

enum ZUI_JUSTIFY {
    ZJ_CENTER = 0,
    ZJ_LEFT = 1,
    ZJ_RIGHT = 2,
    ZJ_UP = 4,
    ZJ_DOWN = 8
};

typedef struct zcmd      { i32 id, bytes; } zcmd;
typedef struct zcmd_clip { zcmd header; zrect cliprect; } zcmd_clip;
typedef struct zcmd_draw_rect { zcmd header; i32 zindex; zrect rect; zcolor color; } zcmd_draw_rect;
typedef struct zcmd_draw_text { zcmd header; i32 zindex; zfont *font; zvec2 coord; char *text; i32 len; zcolor color; } zcmd_draw_text;

typedef union zcmd_draw {
    zcmd base;
    zcmd_clip clip;
    zcmd_draw_rect rect;
    zcmd_draw_text text;
} zcmd_draw;

typedef struct zcmd_ui       { i32 id, bytes, next; } zcmd_ui;
typedef struct zcmd_justify  { zcmd_ui _; i32 justification; } zcmd_justify;
typedef struct zcmd_set_size { zcmd_ui _; zvec2 size; } zcmd_set_size;

typedef struct zcmd_widget { i32 id, bytes, next, zindex; zrect bounds; zrect used; } zcmd_widget;

typedef struct zs_text     { i32 flags, index, ofs, selection; } zs_text;
typedef struct zcmd_text   { zcmd_widget _; char *buffer; i32 len; zs_text *state; } zcmd_text;
typedef struct zcmd_btn    { zcmd_widget _; u8 *state; } zcmd_btn;
typedef struct zcmd_combo  { zcmd_widget _; char *tooltip, *csoptions; i32 *state; } zcmd_combo;
typedef struct zcmd_label  { zcmd_widget _; char *text;  i32 len; } zcmd_label;
typedef struct zcmd_box    { zcmd_widget _; } zcmd_box;
typedef struct zcmd_popup  { zcmd_widget _; } zcmd_popup;
typedef struct zcmd_layout { zcmd_widget _; i32 count; float data[1]; } zcmd_layout;
typedef struct zcmd_grid   { zcmd_widget _; u8 rows, cols, padx, pady; float data[1]; } zcmd_grid;

zcmd_draw *zui_draw_next();

u32 zui_ms();

void zui_print_tree();

void zui_input_mousedown(i32 btn);
void zui_input_mouseup(i32 btn);
void zui_input_mousemove(zvec2 pos);
void zui_input_keydown(i32 keycode);
void zui_input_keyup(i32 keycode);
void zui_input_char(char c);
void zui_input_select();
void zui_input_copy(void (*set_clipboard)(char *data, i32 len));

void zui_init();

void zui_blank();
void zui_box();

void zui_popup();

// set justification
void zui_just(i32 justification);

// set next element size
void zui_size(i32 w, i32 h);

// set zui font
void zui_font(zfont *font);

// ends any container (window / grid)
void zui_end();

// returns true if window is displayed
void zui_window(i32 width, i32 height, float ts);

// creates a label
void zui_label(char *text);

// create a slider with a formatted tooltip which accepts *value
void zui_sliderf(char *tooltip, f32 min, f32 max, f32 *value);
void zui_slideri(char *tooltip, i32 min, i32 max, i32 *value);

// create a combo box with comma-seperated options
i32  zui_combo(char *tooltip, char *csoptions, i32 *state);

bool zui_button(char *text, u8 *state);

// sets the text validator for text inputs
void zui_validator(bool (*validator)(char *text));

// creates a single-line text input
void zui_text(char *buffer, i32 len, zs_text *state);

// creates a multi-line text input
void zui_textbox(char *buffer, i32 len, i32 *state);

// creates a column for the next (n) widgets
// A width within 0..1 is a percentage of available area, above is pixel count, -1 is size to content
// all -1 sized content must be resolved first, in top to bottom order
// width can specifiy justification of the elements, such as (48 | ZJ_LEFT)
void zui_col(i32 n, float *heights);

// creates a row for the next (n) widgets
// A width within 0..1 is a percentage of available area, above is pixel count, -1 is size to content
// all -1 sized content must be resolved first, in left to right order
// height can specify justification of the elements, such as (48 | ZJ_UP)
void zui_row(i32 n, float *widths);

#endif
