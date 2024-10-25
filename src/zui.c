#include "zui.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))

//#define assert(bool, msg) { if(!(bool)) printf(msg); exit(1); }
#define FOR_CHILDREN(ui) for(zcmd_widget* child = __ui_get_child((zcmd_widget*)ui); child != (zcmd_widget*)ui; child = __ui_next(child))
#define FOR_SIBLINGS(ui, sibling) for(zcmd_widget* child = __ui_next((zcmd_widget*)sibling); child != (zcmd_widget*)ui; child = __ui_next(child))


typedef struct zui_type {
    zvec2 (*size)(void*, zvec2);
    void (*pos)(void*, zvec2, i32);
    void (*draw)(void*);
} zui_type;
typedef struct zui_list {
    i32 used;
    i32 cap;
    void *data;
} zui_buf;
typedef struct zui_ctx {
    i32 width, height;
    zvec2 padding;
    zfont *font;
    zvec2 next_size;
    i32 justification;
    i32 prev;
    zui_buf registry;
    zui_buf ui;
    zui_buf draw;
    zui_buf stack;
    i32 ui_reader;
    void *cmd_reader;
    i32 __focused; // used for calculating focused
    i32 focused;
    i32 hovered;
    struct {
        zvec2 mouse_pos;
        i32 mouse_state;
        i32 modifier_state;
        zvec2 prev_mouse_pos;
        i32 prev_mouse_state;
        zui_buf text;
    } input;
    struct {
        float delta;
        u32 ms;
    } time;
} zui_ctx;

static zui_ctx *ctx;

static void __buf_init(zui_buf *l, i32 cap) {
    l->used = 0;
    l->cap = cap;
    l->data = malloc(cap);
}
static void *__buf_alloc(zui_buf *l, i32 size) {
    if(l->used + size > l->cap) {
        l->cap *= 2;
        l->data = realloc(l->data, l->cap);
    }
    void *ret = l->data + l->used;
    l->used += size;
    return ret;
}
static void *__buf_pop(zui_buf *l, i32 size) {
    l->used -= size;
    return l->data + l->used;
}
static void *__ui_alloc(i32 size) {
    ((zcmd_ui*)(ctx->ui.data + ctx->prev))->next = ctx->ui.used;
    ctx->prev = ctx->ui.used;
    return __buf_alloc(&ctx->ui, size);
}
static void *__cont_alloc(i32 size) {
    *(i32*)__buf_alloc(&ctx->stack, 4) = ctx->ui.used;
    return __ui_alloc(size);
}
static void *__buf_peek(zui_buf *l, i32 size) {
    return (l->data + l->used - size);
}
static zrect __rect_add(zrect a, zrect b) {
    return (zrect) { a.x + b.x, a.y + b.y, a.w + b.w, a.h + b.h };
}
static void __push_rect_cmd(zrect rect, zcolor color, i32 zindex) {
    zcmd_draw_rect *r = (zcmd_draw_rect*)__buf_alloc(&ctx->draw, sizeof(zcmd_draw_rect));
    *r = (zcmd_draw_rect) {
        .header = {
            .id = ZCMD_DRAW_RECT,
            .bytes = sizeof(zcmd_draw_rect),
        },
        .zindex = zindex,
        .rect = rect,
        .color = color,
    };
}
static void __push_clip_cmd(zrect rect) {
    zcmd_clip *r = (zcmd_clip*)__buf_alloc(&ctx->draw, sizeof(zcmd_clip));
    *r = (zcmd_clip) {
        .header = {
            .id = ZCMD_CLIP,
            .bytes = sizeof(zcmd_clip)
        },
        .cliprect = rect
    };
}
static void __push_text_cmd(zfont *font, zvec2 coord, zcolor color, char *text, i32 len, i32 zindex) {
    zcmd_draw_text *r = (zcmd_draw_text*)__buf_alloc(&ctx->draw, sizeof(zcmd_draw_text));
    *r = (zcmd_draw_text) {
        .header = {
            .id = ZCMD_DRAW_TEXT,
            .bytes = sizeof(zcmd_draw_text),
        },
        .zindex = zindex,
        .font = font,
        .coord = coord,
        .text = text,
        .color = color,
        .len = len
    };
}
static zvec2 __vec_swap(zvec2 v) {
    return (zvec2) { v.y, v.x };
}
static void __rect_print(zrect r) {
    printf("(%d, %d, %d, %d)\n", r.x, r.y, r.w, r.h);
}
static bool __vec_within(zvec2 v, zrect bounds) {
    return (v.x >= bounds.x) && (v.x <= bounds.x + bounds.w) && (v.y >= bounds.y) && (v.y <= bounds.y + bounds.h);
}
static bool __rect_within(zrect r, zrect bounds) {
    return (r.x >= bounds.x) && (r.x + r.w <= bounds.x + bounds.w) && (r.y >= bounds.y) && (r.y + r.h <= bounds.y + bounds.h);
}
static bool __rect_intersect(zrect a, zrect b, zrect *intersect) {
    i32 x0 = max(a.x, b.x);
    i32 x1 = min(a.x + a.w, b.x + b.w);
    i32 y0 = max(a.y, b.y);
    i32 y1 = min(a.y + a.h, b.y + b.h);
    if(y1 <= y0 || x1 <= x0) return false;
    *intersect = (zrect) {
        .x = x0,
        .y = y0,
        .w = x1 - x0,
        .h = y1 - y0
    };
    return true;
}
static void __rect_justify(zrect *used, zrect bounds, i32 justification) {
    // vertical justification
    if(justification & ZJ_UP)
        used->y = bounds.y;
    else if(justification & ZJ_DOWN)
        used->y = bounds.y + bounds.h - used->h;
    else
        used->y = bounds.y + (bounds.h - used->h) / 2;

    // horizontal justification
    if(justification & ZJ_LEFT)
        used->x = bounds.x;
    else if(justification & ZJ_RIGHT)
        used->x = bounds.x + bounds.w - used->w;
    else
        used->x = bounds.x + (bounds.w - used->w) / 2;
}
static inline zcmd_widget *__ui_widget(i32 index) {
    return (zcmd_widget*)(ctx->ui.data + index);
}
static inline i32 __ui_index(zcmd_widget *ui) {
    return (i32)((u8*)ui - (u8*)ctx->ui.data);
}
// returns next ui element in the container
// if none left, returns parent
zcmd_widget *__ui_next(zcmd_widget *widget) {
    return __ui_widget(widget->next);
}
bool __ui_has_child(zcmd_widget *ui) {
    void *next = (ctx->ui.data + ui->next);
    return (next != (void*)ui + ui->bytes);
}
zcmd_widget *__ui_get_child(zcmd_widget *ui) {
    void *next = (ctx->ui.data + ui->next);
    if(next == (void*)ui + ui->bytes) return 0; // this element has no child
    zcmd_widget *ret = (zcmd_widget*)((void*)ui + ui->bytes);
    if(ret->id < ZW_FIRST)
        ret = __ui_next(ret);
    return ret;
}
bool __ui_pressed(i32 buttons) {
    if(ctx->input.mouse_state & buttons)
        return true;
    return false;
}
bool __ui_clicked(i32 buttons) {
    if(ctx->input.mouse_state & (ctx->input.mouse_state ^ ctx->input.prev_mouse_state) & buttons)
        return true;
    return false;
}

zvec2 __ui_sz(zcmd_widget *ui, zvec2 bounds) {
    zui_type type = ((zui_type*)ctx->registry.data)[ui->id - ZW_FIRST];

    if(ctx->next_size.x >= 0) bounds.x = ctx->next_size.x;
    else if(ctx->next_size.x == Z_AUTO) bounds.x = Z_AUTO;
    if(ctx->next_size.y >= 0) bounds.y = ctx->next_size.y;
    else if(ctx->next_size.y == Z_AUTO) bounds.y = Z_AUTO;

    ctx->next_size = (zvec2) { Z_NONE, Z_NONE };
    zvec2 sz = type.size(ui, bounds);
    ui->used.w = sz.x;
    ui->used.h = sz.y;
    ui->bounds.w = bounds.x == Z_AUTO ? ui->used.w : bounds.x;
    ui->bounds.h = bounds.y == Z_AUTO ? ui->used.h : bounds.y;
    return sz;
}
bool __ui_is_child(zcmd_widget *container, zcmd_widget *other) {
    if(other < container) return false; // children must have a greater index (ptr)
    if(other == container) return true; // we consider a ui to be a child of itself (useful for hovering / focusing)
    i32 index = __ui_index(other);
    if(__ui_index(container) < index && index < container->next) // if index of other is between the container and its sibling, it's a child
        return true;
    // Worst case: when container->next is its parent __ui_is_child checks all children
    // loop through children, if any of the children have a greater index than other, other is a sub-child
    FOR_CHILDREN(container)
        if(child >= other)
            return true;
    return false;
}
bool __ui_hovered(zcmd_widget *ui) {
    return __ui_is_child(ui, __ui_widget(ctx->hovered));
}
bool __ui_focused(zcmd_widget *ui) {
    return __ui_is_child(ui, __ui_widget(ctx->focused));
}

void __ui_pos(zcmd_widget *ui, zvec2 pos, i32 zindex) {
    zui_type type = ((zui_type*)ctx->registry.data)[ui->id - ZW_FIRST];
    ui->bounds.x = pos.x;
    ui->bounds.y = pos.y;
    ui->zindex = zindex;
    __rect_justify(&ui->used, ui->bounds, ctx->justification);
    if(__vec_within(ctx->input.mouse_pos, ui->used)) {
        if(zindex >= __ui_widget(ctx->hovered)->zindex)
            ctx->hovered = __ui_index(ui);
        if(zindex >= __ui_widget(ctx->__focused)->zindex && __ui_clicked(ZM_LEFT_CLICK))
            ctx->__focused = __ui_index(ui);
    }
    if(type.pos) // pos functions are only needed for containers to position children
        type.pos(ui, pos, zindex);
}

void __ui_draw(zcmd_widget *ui) {
    zui_type type = ((zui_type*)ctx->registry.data)[ui->id - ZW_FIRST];
    __push_clip_cmd(ui->bounds);
    type.draw(ui);
    ctx->next_size = (zvec2) { Z_AUTO, Z_AUTO };
}

static zvec2 __ui_sz_ofs(zvec2 bounds, zvec2 ofs) {
    zvec2 ret = { 0, 0 };
    if(bounds.x == Z_AUTO) ret.x = Z_AUTO;
    else ret.x = max(bounds.x + ofs.x, 0);
    if(bounds.y == Z_AUTO) ret.y = Z_AUTO;
    else ret.y = max(bounds.y + ofs.y, 0);
    return ret;
}

static zvec2 __ui_sz_auto(zvec2 bounds, zvec2 auto_sz) {
    zvec2 ret = { 0, 0 };
    if(bounds.x == Z_AUTO) ret.x = auto_sz.x;
    else ret.x = bounds.x;
    if(bounds.y == Z_AUTO) ret.y = auto_sz.y;
    else ret.y = bounds.y;
    return ret;
}

u32 zui_ms() {
    return ctx->time.ms;
}

void zui_input_mousedown(i32 btn) {
    ctx->input.mouse_state |= btn;
}
void zui_input_mouseup(i32 btn) {
    ctx->input.mouse_state &= ~btn;
}
void zui_input_mousemove(zvec2 pos) {
    ctx->input.mouse_pos = pos;
}
void zui_input_keydown(i32 keycode) {
    ctx->input.modifier_state |= keycode;
}
void zui_input_keyup(i32 keycode) {
    ctx->input.modifier_state &= ~keycode;
}
void zui_input_char(char c) {
    char *ptr = __buf_alloc(&ctx->input.text, 1);
    *ptr = c;
}
void zui_print_tree() {
    printf("printing tree...\n");
    i32 latest = 0, index = 0, indent = 0;
    while(index >= latest) {
        latest = index;
        zcmd_ui *cmd = ctx->ui.data + index;
        printf("%04x | ", index);
        for(i32 i = 0; i < indent; i++)
            putc(' ', stdout);
        i32 prev_indent = indent;
        switch(cmd->id) {
            case ZU_JUSTIFY: {
                zcmd_justify *j = (zcmd_justify*)cmd;
                printf("(ZU_JUSTIFY, next: %04x, just: %d)\n", j->_.next, j->justification);
            } break;
            case ZU_SET_SIZE: {
                zcmd_set_size *j = (zcmd_set_size*)cmd;
                printf("(ZU_SET_SIZE, next: %04x, size: (%d, %d))\n", j->_.next, j->size.x, j->size.y);
            } break;
            case ZW_BOX: {
                zcmd_box *b = (zcmd_box*)cmd;
                printf("(ZW_BOX, next: %04x)\n", b->_.next);
                indent += 4;
            } break;
            case ZW_LABEL: {
                zcmd_label *j = (zcmd_label*)cmd;
                printf("(ZW_LABEL, next: %04x, text: %s)\n", j->_.next, j->text);
            } break;
            case ZW_ROW:
            case ZW_COL: {
                zcmd_layout *l = (zcmd_layout*)cmd;
                printf("(%s, next: %04x, layout: [", l->_.id == ZW_COL ? "ZW_COL" : "ZW_ROW", l->_.next);
                for(i32 i = 0; i < l->count; i++) {
                    if(l->data[i] >= 0)
                        printf("%dpx", (i32)l->data[i]);
                    else if(l->data[i] == Z_AUTO)
                        printf("AUTO");
                    else if(l->data[i] >= -1.0f && l->data[i] < 0.0f)
                        printf("%d%%", (i32)(-l->data[i] * 100 + 0.5f));
                    else
                        printf("ERR");
                    if(i != l->count - 1)
                        printf(", ");
                }
                printf("])\n");
                indent += 4;
            } break;
        }
        if(prev_indent == indent && cmd->next < index) {
            indent -= 4;
            cmd = ctx->ui.data + cmd->next; // parent container
            index = cmd->next;
        }
        else {
            index += cmd->bytes;
        }
    }
}

zcmd_draw *zui_draw_next() {
    //printf("size: %d\n", ctx->draw.used);
    if(ctx->cmd_reader >= ctx->draw.data + ctx->draw.used) {
        ctx->draw.used = 0;
        return 0;
    }
    zcmd_draw *next = ctx->cmd_reader;
    ctx->cmd_reader += next->base.bytes;
    return next;
}

void zui_register(i32 widget_id, void *size_cb, void *pos_cb, void *draw_cb) {
    ctx->registry.used = (widget_id - ZW_FIRST) * sizeof(zui_type);
    zui_type *t = __buf_alloc(&ctx->registry, sizeof(zui_type));
    t->size = (zvec2(*)(void*, zvec2))size_cb;
    t->pos = (void(*)(void*, zvec2, i32))pos_cb;
    t->draw = (void(*)(void*))draw_cb;
}

void zui_size(i32 w, i32 h) {
    zcmd_set_size *j = __ui_alloc(sizeof(zcmd_set_size));
    *j = (zcmd_set_size) { ._ = { ZU_SET_SIZE, sizeof(zcmd_set_size) }, .size = (zvec2) { w, h } };
}

void zui_just(i32 justification) {
    zcmd_justify *j = __ui_alloc(sizeof(zcmd_justify));
    *j = (zcmd_justify) { ._ = { ZU_JUSTIFY, sizeof(zcmd_justify) }, .justification = justification };
}

// set zui font
void zui_font(zfont *font) {
    ctx->font = font;
}

// ends any container (window / grid)
void zui_end() {
    zcmd_ui* prev = (ctx->ui.data + ctx->prev);
    ctx->prev = *(i32*)__buf_pop(&ctx->stack, 4);
    prev->next = ctx->prev;
    // window was ended, calculate sizes and generate draw commands
    if(ctx->stack.used == 0) {
        zui_size(Z_NONE, Z_NONE);
        ((zcmd_ui*)ctx->ui.data)->next = 0;

        ctx->next_size = (zvec2) { Z_FILL, Z_FILL };
        zcmd_widget *w = ctx->ui.data;
        __ui_sz(w, (zvec2) { ctx->width, ctx->height });
        w->bounds.x = 0;
        w->bounds.y = 0;

        ctx->__focused = 0;
        ctx->hovered = 0;
        __ui_pos(w, (zvec2) { 0, 0 }, 0);
        if(ctx->__focused)
            ctx->focused = ctx->__focused;
        __ui_draw(w);
        ctx->cmd_reader = ctx->draw.data;
        ctx->input.prev_mouse_pos = ctx->input.mouse_pos;
        ctx->input.prev_mouse_state = ctx->input.mouse_state;
        ctx->input.text.used = 0;
    }
}

void zui_blank() {
    zcmd_widget *w = __ui_alloc(sizeof(zcmd_widget));
    *w = (zcmd_widget) { .id = ZW_BLANK, .bytes = sizeof(zcmd_widget) };
}
static zvec2 __zui_blank_size(zcmd_widget *w, zvec2 bounds) { return bounds; }
static void __zui_blank_draw(zcmd_widget *w) {}

// returns true if window is displayed
void zui_box() {
    zcmd_box *l = __cont_alloc(sizeof(zcmd_box));
    *l = (zcmd_box) { ._ = { ZW_BOX, sizeof(zcmd_box) } };
}
static zvec2 __zui_box_size(zcmd_box *box, zvec2 bounds) {
    zvec2 auto_sz = { 0, 0 };
    zvec2 child_bounds = __ui_sz_ofs(bounds, (zvec2) { -ctx->padding.x * 2, -ctx->padding.y * 2 });
    int i = 0;
    FOR_CHILDREN(box) {
        zvec2 sz = __ui_sz(child, child_bounds);
        auto_sz.x = max(auto_sz.x, sz.x);
        auto_sz.y = max(auto_sz.y, sz.y);
        i++;
    }
    auto_sz.x += ctx->padding.x * 2;
    auto_sz.y += ctx->padding.y * 2;
    return __ui_sz_auto(bounds, auto_sz);
}
static void __zui_box_pos(zcmd_box *box, zvec2 pos, i32 zindex) {
    zvec2 child_pos = { pos.x + ctx->padding.x, pos.y + ctx->padding.y };
    FOR_CHILDREN(box) __ui_pos(child, child_pos, zindex);
}
static void __zui_box_draw(zcmd_box *box) {
    __push_rect_cmd(box->_.bounds, (zcolor) { 50, 50, 50, 255 }, box->_.zindex);
    FOR_CHILDREN(box) __ui_draw(child);
}

void zui_popup() {
    zcmd_box *l = __cont_alloc(sizeof(zcmd_box));
    *l = (zcmd_box) { ._ = { ZW_BOX, sizeof(zcmd_box) } };
}
static zvec2 __zui_popup_size(zcmd_box *box, zvec2 bounds) {
    FOR_CHILDREN(box)
        __ui_sz(child, bounds);
    return (zvec2) { 0, 0 };
}
static void __zui_popup_pos(zcmd_box *box, zvec2 pos, i32 zindex) {
    zvec2 child_pos = { pos.x + ctx->padding.x, pos.y + ctx->padding.y };
    FOR_CHILDREN(box) __ui_pos(child, child_pos, zindex + 1);
}
static void __zui_popup_draw(zcmd_box *box) {
    FOR_CHILDREN(box) __ui_draw(child);
}

void zui_window(i32 width, i32 height, double dt) {
    ctx->ui.used = 0;
    ctx->width = width;
    ctx->height = height;
    float ms = dt * 1000.0f + ctx->time.delta;
    u32 new_ms = (u32)ms;
    ctx->time.delta = ms - new_ms;
    ctx->time.ms += new_ms;
    zui_box();
}

// LABEL
void zui_label_n(char *text, i32 len) {
    zcmd_label *l = __ui_alloc(sizeof(zcmd_label));
    *l = (zcmd_label) {
        ._ = { ZW_LABEL, sizeof(zcmd_label) },
        .text = text,
        .len = len
    };
}

void zui_label(char *text) {
    zui_label_n(text, strlen(text));
}

static zvec2 __zui_label_size(zcmd_label *data, zvec2 bounds) {
    zfont *font = ctx->font;
    return font->text_size(font, data->text, data->len);
}

static void __zui_label_draw(zcmd_label *data) {
    zvec2 coords = { data->_.used.x, data->_.used.y };
    __push_text_cmd(ctx->font, coords, (zcolor) { 250, 250, 250, 255 }, data->text, data->len, data->_.zindex);
}

// BUTTON
bool zui_button(char *text, u8 *state) {
    zcmd_btn *l = __cont_alloc(sizeof(zcmd_btn));
    *l = (zcmd_btn) { ._ = { ZW_BTN, sizeof(zcmd_btn) }, .state = state };
    zui_label(text);
    zui_end();
    return *state;
}

// A button is just a clickable box
// This allows trivial addition of various button kinds: with images, multiple labels, etc.
// Reuse __zui_box_size for sizing

static void __zui_button_draw(zcmd_btn *btn) {
    zcolor c = (zcolor) { 80, 80, 80, 255 };
    if(__ui_hovered(&btn->_)) {// hovered
        *btn->state = __ui_clicked(ZM_LEFT_CLICK);
        if(__ui_pressed(ZM_LEFT_CLICK))
            c = (zcolor) { 120, 120, 120, 255 };
        else
            c = (zcolor) { 100, 100, 100, 255 };
    }
    __push_rect_cmd(btn->_.bounds, c, btn->_.zindex);
    FOR_CHILDREN(btn)
        __ui_draw(child);
}

// create a slider with a formatted tooltip which accepts *value
void zui_sliderf(char *tooltip, f32 min, f32 max, f32 *value);
void zui_slideri(char *tooltip, i32 min, i32 max, i32 *value);

char *__zui_combo_get_option(zcmd_combo *c, i32 n, i32 *len) {
    if(n < 0) {
        *len = strlen(c->tooltip);
        return c->tooltip;
    }
    char *option = c->csoptions, *s = c->csoptions;
    i32 cnt = 0;
    for(; *s; s++) {
        if(*s != ',') continue;
        if(cnt == n) {
            *len = (i32)(s - option);
            return option;
        }
        cnt++;
        option = s + 1;
    }
    *len = (i32)(s - option);
    return option;
}
// create a combo box with comma-seperated options
i32 zui_combo(char *tooltip, char *csoptions, i32 *state) {
    zcmd_combo *c = __cont_alloc(sizeof(zcmd_combo));
    *c = (zcmd_combo) {
        ._ = { ZW_COMBO, sizeof(zcmd_combo) },
        .tooltip = tooltip,
        .csoptions = csoptions,
        .state = state
    };
    if(true) { // combo box is open
        zui_blank();
        char *option = csoptions, *s = csoptions;
        for(; *s; s++) {
            if(*s != ',') continue;
            zui_box();
                zui_label_n(option, (i32)(s - option));
            zui_end();
            option = s + 1;
        }
        zui_box();
            zui_label_n(option, (i32)(s - option));
        zui_end();
    }
    zui_end();
    return (*state >> 1) - 1;
}
static zvec2 __zui_combo_size(zcmd_combo *data, zvec2 bounds) {
    zfont *font = ctx->font;
    zvec2 auto_sz = font->text_size(font, data->tooltip, strlen(data->tooltip));
    zvec2 back_sz = (zvec2) { 0, 0 };
    bounds.y = Z_AUTO;
    // if(*data->state & 1) {
        zcmd_widget *background = __ui_get_child(&data->_);
        zcmd_widget *child = __ui_next(background);
        while(child != &data->_) {
            zvec2 sz = __ui_sz(child, bounds);
            back_sz.x = auto_sz.x = max(auto_sz.x, sz.x);
            back_sz.y += sz.y;
            child = __ui_next(child);
        }

    auto_sz.x += 10;
    auto_sz.y += 10;
    if(bounds.x != Z_AUTO)
        auto_sz.x = bounds.x;
    back_sz.x = auto_sz.x;
    __ui_sz(background, back_sz);
    return auto_sz;
}
static void __zui_combo_pos(zcmd_combo *data, zvec2 pos) {
    pos.y += data->_.used.h;
    zcmd_widget *background = __ui_get_child(&data->_);
    i32 prev = ctx->justification;
    ctx->justification = ZJ_LEFT;
    __ui_pos(background, pos, data->_.zindex + 1);
    FOR_SIBLINGS(data, background) {
        __ui_pos(child, pos, data->_.zindex + 1);
        pos.y += child->used.h;
    }
    ctx->justification = prev;
}
static void __zui_combo_draw(zcmd_combo *box) {
    bool is_focused = ((ctx->focused + ctx->ui.data) == box);
    i32 selected_index = (*box->state >> 1) - 1;
    __push_rect_cmd(box->_.used, (zcolor) { 70, 70, 70, 255 }, box->_.zindex);

    i32 len;
    char *text = __zui_combo_get_option(box, selected_index, &len);
    zvec2 pos = { box->_.used.x + ctx->padding.x, box->_.used.y + ctx->padding.y };
    __push_text_cmd(ctx->font, pos, (zcolor) { 230, 230, 230, 255 }, text, len, box->_.zindex);

    // this detects whether our current widget (not any children) are focused
    if(&box->_ == __ui_widget(ctx->focused) && __ui_clicked(ZM_LEFT_CLICK)) {
        *box->state ^= 1;
        return;
    }
    if(~*box->state & 1) return;
    zcmd_widget *background = __ui_get_child(&box->_);
    __push_clip_cmd(background->used);
    __push_rect_cmd(background->used, (zcolor) { 80, 80, 80, 255 }, box->_.zindex);
    i32 i = 0;
    FOR_SIBLINGS(box, background) {
        __push_clip_cmd(background->used);
        if(__ui_focused(child) && __ui_clicked(ZM_LEFT_CLICK)) {
            *box->state = ((i + 1) << 1); // close popup and set selected
        } else if(__ui_hovered(child)) {
            __push_rect_cmd(child->used, (zcolor) { 120, 120, 120, 255 }, child->zindex);
        } else if(i == selected_index) {
            __push_rect_cmd(child->used, (zcolor) { 100, 100, 100, 255 }, child->zindex);
        }
        __ui_draw(__ui_get_child(child));
        i++;
    }
}

// sets the text validator for text inputs
void zui_validator(bool (*validator)(char *text));

// creates a single-line text input
void zui_text(char *buffer, i32 len, i32 *state) {
    zcmd_text *l = __ui_alloc(sizeof(zcmd_text));
    *l = (zcmd_text) { ._ = { ZW_TEXT, sizeof(zcmd_text) }, .buffer = buffer, .len = len, .state = state };;
}

static zvec2 __zui_text_size(zcmd_text *data, zvec2 bounds) {
    zfont *font = ctx->font;
    zvec2 sz = font->text_size(font, data->buffer, strlen(data->buffer));
    sz.x += 10;
    sz.y += 10;
    if(bounds.x != Z_AUTO) sz.x = bounds.x;
    return sz;
}

static void __zui_text_draw(zcmd_text *data) {
    typedef struct text_ctx { i16 index, ofs; } text_ctx;
    text_ctx tctx = *(text_ctx*)data->state;
    zfont *font = ctx->font;
    i32 len = strlen(data->buffer);
    if(ctx->ui.data + ctx->focused == data) {
        for(i32 i = 0; i < ctx->input.text.used; i++) {
            char c = ((char*)ctx->input.text.data)[i];
            if(c == 27 || c == 9) { // escape on tab or esc
                ctx->focused = 0;
                continue;
            }
            if(c >= 17 && c <= 20) { // arrow keys
                if(c == 17 && tctx.index > 0)
                    tctx.index--;
                else if(c == 18 && tctx.index < len)
                    tctx.index++;
                continue;
            }
            if(c != '\b') { // write character
                if(len < data->len - 1) {
                    len++;
                    for(i32 j = data->len - 1; j > tctx.index; j--)
                        data->buffer[j] = data->buffer[j - 1];
                    data->buffer[tctx.index++] = c;
                }
            } else if(tctx.index > 0) { // backspace
                len--;
                tctx.index--;
                if(tctx.ofs > 0) {
                    char tmp[2] = { data->buffer[tctx.index], 0 };
                    tctx.ofs -= font->text_size(font, tmp, 1).x;
                    tctx.ofs = max(tctx.ofs, 0);
                }
                for(i32 j = tctx.index; j < data->len - 1; j++)
                    data->buffer[j] = data->buffer[j + 1];
            }
        }
        data->buffer[data->len - 1] = 0;
    }
    zvec2 sz = font->text_size(font, data->buffer, tctx.index);
    zvec2 textpos = { data->_.used.x + 5 - tctx.ofs, data->_.used.y + 5 };
    if(textpos.x + sz.x + 1 > data->_.used.x + data->_.used.w - 5) {
        i32 diff = textpos.x + sz.x + 6 - data->_.used.x - data->_.used.w;
        tctx.ofs += diff;
        textpos.x -= diff;
    }
    else if(textpos.x + sz.x < data->_.used.x + 5) {
        i32 diff = textpos.x + sz.x - data->_.used.x - 5;
        tctx.ofs += diff;
        textpos.x -= diff;
    }
    zrect cursor = { textpos.x + sz.x, textpos.y, 1, sz.y };

    __push_rect_cmd(data->_.used, (zcolor) { 30, 30, 30, 255 }, data->_.zindex);
    __push_text_cmd(ctx->font, textpos, (zcolor) { 250, 250, 250, 255 }, data->buffer, len, data->_.zindex);

    if(ctx->ui.data + ctx->focused == data && (ctx->time.ms >> 9) & 1) // blink cursor
        __push_rect_cmd(cursor, (zcolor) { 200, 200, 200, 255 }, data->_.zindex);

    *(text_ctx*)data->state = tctx;
}

// creates a multi-line text input
void zui_textbox(char *buffer, i32 len, i32 *state);

static void zui_layout(i32 id, i32 n, float *sizes) {
    i32 bytes = sizeof(zcmd_layout) + (n - 1) * sizeof(float);
    zcmd_layout *l = __cont_alloc(bytes);
    *l = (zcmd_layout) { ._ = { id, bytes }, .count = n };
    if(sizes)
        memcpy(l->data, sizes, n * sizeof(float));
    else {
        for(i32 i = 0; i < n; i++)
            l->data[i] = Z_AUTO;
    }
}

void zui_col(i32 n, float *heights) {
    zui_layout(ZW_COL, n, heights);
}

void zui_row(i32 n, float *heights) {
    zui_layout(ZW_ROW, n, heights);
}

static void __zui_layout_draw(zcmd_layout *data) {
    FOR_CHILDREN(data)
        __ui_draw(child);
}

static zvec2 __ui_sz_swap(zcmd_widget *ui, zvec2 bounds) {
    zvec2 sz = __ui_sz(ui, (zvec2) { bounds.y, bounds.x });
    return (zvec2) { sz.y, sz.x };
}

static zvec2 __zui_layout_size(zcmd_layout *data, zvec2 bounds) {
    zcmd_widget *child = __ui_get_child(&data->_);

    // minimum size of empty container is 0, 0
    if(!child)
        return (zvec2) { 0, 0 };

    zvec2 (*ui_sz)(zcmd_widget *, zvec2);
    i32 major = (data->count - 1), minor = 0;
    i32 major_bound = 0, minor_bound = 0;

    // We share logic between rows and columns by swapping vectors
    if(data->_.id == ZW_COL) {
        ui_sz = &__ui_sz;
        minor_bound = bounds.x;
        major_bound = bounds.y;
        major *= ctx->padding.y;
    } else if(data->_.id == ZW_ROW) {
        ui_sz = &__ui_sz_swap;
        minor_bound = bounds.y;
        major_bound = bounds.x;
        major *= ctx->padding.x;
    } else {
        printf("Layout type %d unrecognized\n", data->_.id);
        return (zvec2) { 0, 0 };
    }

    // calculate autosized
    zcmd_widget *iter = child;
    for(i32 i = 0; i < data->count; i++, iter = __ui_next(iter)) {
        if(data->data[i] != Z_AUTO) continue;
        zvec2 child_sz = ui_sz(iter, (zvec2) { minor_bound, Z_AUTO });
        minor = max(minor, child_sz.x);
        major += child_sz.y;
    }

    // calculate pixels
    iter = child;
    float percent_total = 0;
    for(i32 i = 0; i < data->count; i++, iter = __ui_next(iter)) {
        float f = data->data[i];
        if(f >= -1.0f && f < 0.0f)
            percent_total += f;
        if(f < 0.0f) continue;
        i32 pixels = (i32)f;
        zvec2 child_sz = ui_sz(iter, (zvec2) { minor_bound, pixels });
        minor = max(minor, child_sz.x);
        major += pixels;
    }

    i32 pixels_so_far = major;

    // calculate percentage
    iter = child;
    for(i32 i = 0; i < data->count; i++, iter = __ui_next(iter)) {
        float f = data->data[i];
        if(f < -1.0f || f > -0.0f) continue;
        i32 height = (i32)((major_bound - pixels_so_far) * f / percent_total + 0.5f);
        zvec2 child_sz = ui_sz(iter, (zvec2) { minor_bound, height });
        minor = max(minor, child_sz.x);
        major += height;
    }
    if(data->_.id == ZW_ROW) {
        FOR_CHILDREN(data)
            child->bounds.h = minor;
        return __ui_sz_auto(bounds, (zvec2) { major, minor });
    } else {
        FOR_CHILDREN(data)
            child->bounds.w = minor;
        return __ui_sz_auto(bounds, (zvec2) { minor, major });
    }
}

static void __zui_layout_pos(zcmd_layout *data, zvec2 pos, i32 zindex) {
    FOR_CHILDREN(data) {
        __ui_pos(child, pos, zindex);
        if(data->_.id == ZW_COL)
            pos.y += ctx->padding.y + child->bounds.h;
        else
            pos.x += ctx->padding.x + child->bounds.w;
    }
}
void zui_grid(i32 rows, i32 cols, float *row_col_settings) {
    i32 bytes = sizeof(zcmd_grid) + (rows + cols - 1) * sizeof(float);
    zcmd_grid *l = __cont_alloc(bytes);
    *l = (zcmd_grid) { ._ = { ZW_GRID, bytes }, .rows = rows, .cols = cols };
    if(row_col_settings)
        memcpy(l->data, row_col_settings, (rows + cols) * sizeof(float));
    else {
        for(i32 i = 0; i < (rows + cols); i++)
            l->data[i] = Z_AUTO;
    }
}
// static zvec2 __zui_grid_size(zcmd_grid *grid, zvec2 bounds) {
//     return (zvec2) { 0, 0 };
// }
// static void __zui_grid_pos(zcmd_grid *grid, zvec2 pos, i32 zindex) {
//
// }
// static void __zui_grid_draw(zcmd_grid *grid) {
//
// }

void zui_init() {
    static zui_ctx global_ctx = { 0 };
    __buf_init(&global_ctx.draw, 256);
    __buf_init(&global_ctx.ui, 256);
    __buf_init(&global_ctx.registry, 256);
    __buf_init(&global_ctx.stack, 256);
    __buf_init(&global_ctx.input.text, 256);
    global_ctx.padding = (zvec2) { 5, 5 };
    global_ctx.next_size = (zvec2) { Z_NONE, Z_NONE };
    global_ctx.prev = 0;
    ctx = &global_ctx;
    zui_register(ZW_BLANK, __zui_blank_size, 0, __zui_blank_draw);
    zui_register(ZW_BOX, __zui_box_size, __zui_box_pos, __zui_box_draw);
    zui_register(ZW_POPUP, __zui_popup_size, __zui_popup_pos, __zui_popup_draw);
    zui_register(ZW_LABEL, __zui_label_size, 0, __zui_label_draw);
    zui_register(ZW_COL, __zui_layout_size, __zui_layout_pos, __zui_layout_draw);
    zui_register(ZW_ROW, __zui_layout_size, __zui_layout_pos, __zui_layout_draw);
    zui_register(ZW_BTN, __zui_box_size, __zui_box_pos, __zui_button_draw);
    zui_register(ZW_TEXT, __zui_text_size, 0, __zui_text_draw);
    zui_register(ZW_COMBO, __zui_combo_size, __zui_combo_pos, __zui_combo_draw);
    //zui_register(ZW_GRID, __zui_grid_size, __zui_grid_pos, __zui_grid_draw);
}
