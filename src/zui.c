#include "zui.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))

//#define assert(bool, msg) { if(!(bool)) printf(msg); exit(1); }
#define FOR_CHILDREN(ui) for(zcmd_widget* child = __ui_get_child((zcmd_widget*)ui); child != (zcmd_widget*)ui; child = __ui_next(child))
#define FOR_SIBLINGS(ui, sibling) for(zcmd_widget* child = __ui_next((zcmd_widget*)sibling); child != (zcmd_widget*)ui; child = __ui_next(child))
#define SWAP(type, a, b) { type tmp = a; a = b; b = tmp; }

typedef struct zui_type {
    zvec2 (*size)(void*, zvec2);
    void (*pos)(void*, zvec2, i32);
    void (*draw)(void*);
} zui_type;

typedef struct zui_buf {
    i32 used;
    i32 cap;
    u8 *data;
} zui_buf;

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

typedef struct zui_client {
	zvec2 window_sz;
	zvec2 mouse_pos;
	u16 mouse_state;
	u16 modifiers;
	u16 font_cnt;
	zui_client_fn send;
	zui_server_fn recv;
	void *user_data;
	zui_buf commands;
	zui_buf knownGlyphs;
} zui_client;

static zui_client *client;

void zui_client_init(zui_client_fn send, zui_server_fn recv, void *user_data) {
	static zui_client global_client = { 0 };
	client = &global_client;
	client->send = send;
	client->recv = recv;
	client->user_data = user_data;
	__buf_init(&client->knownGlyphs, 256);
}

void zui_client_process(char *bytes, i32 len) {
	if (len < sizeof(zcmd)) return;
	zcmd *cmd = (zcmd*)bytes;
	if (cmd->bytes > len) return;	
	// draw commands are pushed to the command stack
	if (cmd->id >= ZSCMD_CLIP && cmd->id <= ZSCMD_TEXT) {
		void *ptr = __buf_alloc(&client->commands, cmd->bytes);
		memcpy(ptr, cmd, cmd->bytes);
	}
	// non-draw commands are executed immediately
	else {
		client->recv(cmd, client->user_data);
	}
	zui_client_process(bytes + cmd->bytes, len - cmd->bytes);
}

void zui_client_render() {
	char *ptr = client->commands.data;
	char *end = client->commands.data + client->commands.used;
	while (ptr < end) {
		zscmd *cmd = (zscmd*)ptr;
		client->recv(cmd, client->user_data);
		ptr += cmd->base.bytes;
	}	
}

void __zui_send_mouse_data() {
	zccmd_mouse data = {
		.header = { ZCCMD_MOUSE, sizeof(zccmd_mouse) },
		.pos = client->mouse_pos,
		.state = client->mouse_state
	};
	client->send(&data, client->user_data);
}

void zui_mouse_down(u16 btn) {
	client->mouse_state |= btn;
	__zui_send_mouse_data();
}
void zui_mouse_up(u16 btn) { client->mouse_state &= ~btn; __zui_send_mouse_data(); }
void zui_mouse_move(zvec2 pos) { client->mouse_pos = pos; __zui_send_mouse_data(); }
void zui_key_char(char c) {
	c &= 0x7f;
	u8 mask = (1 << (c & 7));
	// send glyph info for all fonts
	for (u16 id = 0; id < client->font_cnt; id++) {
		u8 *byte = &client->knownGlyphs.data[c / 8 + id * 16];
		if (*byte & mask) continue;
		*byte |= mask;
		zscmd_glyph glyph = {
			.header = { ZSCMD_GLYPH, sizeof(zscmd_glyph) },
			.c = (zglyph_data) {
				.font_id = 0,
				.c = c,
				.width = 0
			}
		};
		client->recv((zscmd*)&glyph, client->user_data);
		glyph.header.id = ZCCMD_GLYPH;
		client->send((zccmd*)&glyph, client->user_data);
	}
	zccmd_keys key = {
		.header = { ZCCMD_KEYS, sizeof(zccmd_keys) },
		.modifiers = client->modifiers,
		.key = c
	};
	client->send((zccmd*)&key, client->user_data);
}

void zui_resize(u16 width, u16 height) {
	if (width == client->window_sz.x && height == client->window_sz.y) return;
	client->window_sz = (zvec2) { width, height };
	zccmd_win win = {
		.header = { ZCCMD_WIN, sizeof(zccmd_win) },
		.sz = client->window_sz
	};
	client->send(&win, client->user_data);
}

typedef struct zui_ctx {
    i32 width, height;
    zui_server_fn send;
    zui_client_fn recv;
    void *user_data;
    zvec2 padding;
    zfont *font;
    zvec2 next_size;
    u32 flags;
    i32 container;
    zui_buf registry;
    zui_buf ui;
    zui_buf draw;
    zui_buf stack;
    zui_buf zdeque;
    union {
        u8 *cmd_reader;
        u64 *deque_reader;
    };
    i32 __focused; // used for calculating focused
    i32 focused;
    i32 hovered;
    struct {
        zvec2 mouse_pos;
        zvec2 prev_mouse_pos;
        i32 mouse_state;
        i32 prev_mouse_state;
        zui_buf text;
        bool ctrl_a;
    } input;
    struct {
        float delta;
        u32 ms;
    } time;
} zui_ctx;

static zui_ctx *ctx;

static void *__ui_alloc(i32 id, i32 size) {
    ((zcmd_widget*)(ctx->ui.data + ctx->container))->next = ctx->ui.used;
    ctx->container = ctx->ui.used;
    zcmd_widget *widget = __buf_alloc(&ctx->ui, size);
    memset(widget, 0, size);
    widget->id = id;
    widget->bytes = size;
    widget->flags = ctx->flags;
    return widget;
}
static void *__cont_alloc(i32 id, i32 size) {
    *(i32*)__buf_alloc(&ctx->stack, 4) = ctx->ui.used;
    return __ui_alloc(id, size);
}
static void *__draw_alloc(u16 id, u16 size, i32 zindex) {
    u64 *index = __buf_alloc(&ctx->zdeque, sizeof(u64));
    zscmd *ret = (zscmd*)__buf_alloc(&ctx->draw, size);
	ret->base.id = id;
	ret->base.bytes = size;
    // high bytes represent z-index, low bits are index into the pointer
    // we can sort this deque as 64 bit integers, while will sort zindex first, and then by insertion order
    *index = ((u64)zindex << 32) | ((u8*)ret - ctx->draw.data);
    return ret;
}
static void *__buf_peek(zui_buf *l, i32 size) {
    return (l->data + l->used - size);
}
static zrect __rect_add(zrect a, zrect b) {
    return (zrect) { a.x + b.x, a.y + b.y, a.w + b.w, a.h + b.h };
}
static void __push_rect_cmd(zrect rect, zcolor color, i32 zindex) {
    zscmd_rect *r = __draw_alloc(ZSCMD_RECT, sizeof(zscmd_rect), zindex);
	r->rect = rect;
	r->color = color;
}
static void __push_clip_cmd(zrect rect, i32 zindex) {
    zscmd_clip *r = __draw_alloc(ZSCMD_CLIP, sizeof(zscmd_clip), zindex);
	r->rect = rect;
}
static void __push_text_cmd(zfont *font, zvec2 coord, zcolor color, char *text, i32 len, i32 zindex) {
    zscmd_text *r = __draw_alloc(ZSCMD_TEXT, sizeof(zscmd_text) + len, zindex);
	r->font_id = 0;
	r->pos = coord;
	r->color = color;
	memcpy(r->text, text, len);
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
void __ui_schedule_focus(zcmd_widget *widget) {
    if (!widget) return;
    ctx->focused = 0;
    ctx->__focused = __ui_index(widget);
}
zcmd_widget *__ui_find_with_flag(zcmd_widget *start, u32 flags) {
    i32 si = __ui_index(start);
    for(i32 index = si + start->bytes; index < ctx->ui.used; index += __ui_widget(index)->bytes) {
        zcmd_widget *w = __ui_widget(index);
        if (w->flags & flags)
            return w;
    }
    for (i32 index = 0; index < si; index += __ui_widget(index)->bytes) {
        zcmd_widget *w = __ui_widget(index);
        if (w->flags & flags)
            return w;
    }
    return 0;
}
bool __ui_has_child(zcmd_widget *ui) {
	return (ui->flags & ZF_PARENT) > 0;
}
zcmd_widget *__ui_get_child(zcmd_widget *ui) {
	if (~ui->flags & ZF_PARENT) return 0;
    void *next = (ctx->ui.data + ui->next);
    zcmd_widget *ret = (zcmd_widget*)((u8*)ui + ui->bytes);
    return ret;
}
bool __ui_pressed(i32 buttons) {
    if(ctx->input.mouse_state & buttons)
        return true;
    return false;
}
bool __ui_dragged(i32 buttons) {
    if(ctx->input.mouse_state & ctx->input.prev_mouse_state & buttons)
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
static inline bool __ui_hovered(zcmd_widget *ui) { return ui == __ui_widget(ctx->hovered); }
static inline bool __ui_cont_hovered(zcmd_widget *ui) { return __ui_is_child(ui, __ui_widget(ctx->hovered)); }
static inline bool __ui_focused(zcmd_widget *ui) { return ui == __ui_widget(ctx->focused); }
static inline bool __ui_cont_focused(zcmd_widget *ui) { return __ui_is_child(ui, __ui_widget(ctx->focused)); }

void __ui_pos(zcmd_widget *ui, zvec2 pos, i32 zindex) {
    zui_type type = ((zui_type*)ctx->registry.data)[ui->id - ZW_FIRST];
    ui->bounds.x = pos.x;
    ui->bounds.y = pos.y;
    ui->zindex = zindex;
    __rect_justify(&ui->used, ui->bounds, ui->flags);
    if(__vec_within(ctx->input.mouse_pos, ui->used)) {
        if(zindex >= __ui_widget(ctx->hovered)->zindex)
            ctx->hovered = __ui_index(ui);
        if(zindex >= __ui_widget(ctx->__focused)->zindex && __ui_clicked(ZM_LEFT_CLICK))
            ctx->__focused = __ui_index(ui);
    }
    if(type.pos) // pos functions are only needed for containers to position children
        type.pos(ui, *(zvec2*)&ui->used, zindex);
}

void __ui_draw(zcmd_widget *ui) {
    zui_type type = ((zui_type*)ctx->registry.data)[ui->id - ZW_FIRST];
    __push_clip_cmd(ui->bounds, ui->zindex);
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
void zui_input_char(char c) {
    char *ptr = __buf_alloc(&ctx->input.text, 1);
    *ptr = c;
}

void __ui_print(zcmd_widget *cmd, int indent) {
	zapp_log("%04x | ", __ui_index(cmd));
	for (i32 i = 0; i < indent; i++)
		zapp_log("    ");
	switch (cmd->id) {
	//case ZW_BOX: {
	//	zcmd_box *b = (zcmd_box*)cmd;
	//	zapp_log("(ZW_BOX, next: %04x)\n", b->_.next);
	//} break;
	//case ZW_LABEL: {
	//	zcmd_label *j = (zcmd_label*)cmd;
	//	zapp_log("(ZW_LABEL, next: %04x, text: %s)\n", j->_.next, j->text);
	//} break;
	//case ZW_ROW:
	//case ZW_COL: {
	//	zcmd_layout *l = (zcmd_layout*)cmd;
	//	zapp_log("(%s, next: %04x, layout: [", l->_.id == ZW_COL ? "ZW_COL" : "ZW_ROW", l->_.next);
	//	for (i32 i = 0; i < l->count; i++) {
	//		if (l->data[i] >= 0)
	//			zapp_log("%dpx", (i32)l->data[i]);
	//		else if (l->data[i] == Z_AUTO)
	//			zapp_log("AUTO");
	//		else if (l->data[i] >= -1.0f && l->data[i] < 0.0f)
	//			zapp_log("%d%%", (i32)(-l->data[i] * 100 + 0.5f));
	//		else
	//			zapp_log("ERR");
	//		if (i != l->count - 1)
	//			zapp_log(", ");
	//	}
	//	zapp_log("])\n");
	//} break;
	default: {
		zrect b = cmd->bounds;
		zrect u = cmd->used;
		zapp_log("(id: %d, next: %04x, bounds: {%d,%d,%d,%d}, used: {%d,%d,%d,%d})\n", cmd->id, cmd->next, b.x, b.y, b.w, b.h, u.x, u.y, u.w, u.h);
	}
	}
	if (__ui_has_child(cmd)) {
		FOR_CHILDREN(cmd) {
			__ui_print(child, indent + 1);
		}
	}
}


void zui_server_init(zui_server_fn send, zui_client_fn recv) {

}

static void __zui_default_client(zccmd *input) {

}

static void __zui_push_client(zscmd *cmd) {
	if (cmd->base.id >= ZSCMD_CLIP && cmd->base.id <= ZSCMD_TEXT) {
		void *ptr = __buf_alloc(&client->commands, cmd->base.bytes);
		memcpy(ptr, cmd, cmd->base.bytes);
	}
	// non-draw commands are executed immediately
	else {
		client->recv(cmd, client->user_data);
	}
}

void zui_init(zui_server_fn renderer, void *userdata) {
	zui_client_init(__zui_default_client, renderer, userdata);
	zui_server_init(__zui_push_client, __zui_default_client, userdata);
}

void zui_print_tree() {
	zapp_log("printing tree...\n");
	__ui_print((zcmd_widget*)ctx->ui.data, 0);
}

void zui_register(i32 widget_id, void *size_cb, void *pos_cb, void *draw_cb) {
    ctx->registry.used = (widget_id - ZW_FIRST) * sizeof(zui_type);
    zui_type *t = __buf_alloc(&ctx->registry, sizeof(zui_type));
    t->size = (zvec2(*)(void*, zvec2))size_cb;
    t->pos = (void(*)(void*, zvec2, i32))pos_cb;
    t->draw = (void(*)(void*))draw_cb;
}

void zui_justify(u32 justification) {
    ctx->flags = justification & 15;
}

// set zui font
void zui_font(zfont *font) {
    ctx->font = font;
}

void __zui_qsort(u64 *nums, i32 count) {
    if (count < 2) return;
    u64 pivot = nums[0];
    i32 i = 1, j = count - 1;
    do {
        while (nums[i] <= pivot && i < j) i++;
        while (nums[j] > pivot && i < j) j--;
        if (j <= i) i = 0;
		SWAP(u64, nums[i], nums[j]);
    } while (i);
    __zui_qsort(nums, j++);
    __zui_qsort(nums + j, count - j);
}

// ends any container (window / grid)
void zui_end() {
    zcmd_widget* prev = (zcmd_widget*)(ctx->ui.data + ctx->container);
    ctx->container = *(i32*)__buf_pop(&ctx->stack, 4);
    prev->next = ctx->container;

    // active container isn't the most recent element (it has children)
    if (ctx->container != ctx->ui.used) { 
        __ui_widget(ctx->container)->flags |= ZF_PARENT;
    }
    // window was ended, calculate sizes and generate draw commands
    if(ctx->stack.used == 0) {
        ((zcmd_widget*)ctx->ui.data)->next = 0;

        ctx->next_size = (zvec2) { Z_FILL, Z_FILL };
        zcmd_widget *root = __ui_widget(0);
        __ui_sz(root, (zvec2) { ctx->width, ctx->height });
        root->bounds.x = 0;
        root->bounds.y = 0;

        ctx->hovered = 0;
        __ui_pos(root, (zvec2) { 0, 0 }, 0);
        if (ctx->__focused) {
            ctx->focused = ctx->__focused;
            ctx->__focused = 0;
        }
	
        __ui_draw(root);

        // needed for most 2D drawing API's. Without a z-buffer, you must sort the draw calls
        __zui_qsort((u64*)ctx->zdeque.data, ctx->zdeque.used / sizeof(u64));

        ctx->deque_reader = (u64*)ctx->zdeque.data;

        ctx->input.prev_mouse_pos = ctx->input.mouse_pos;
        ctx->input.prev_mouse_state = ctx->input.mouse_state;
        ctx->input.text.used = 0;
        //ctx->input.clipboard = 0;
        //ctx->input.ctrl_a = false;
    }
}

void zui_blank() {
    __ui_alloc(ZW_BLANK, sizeof(zcmd_widget));
}
static zvec2 __zui_blank_size(zcmd_widget *w, zvec2 bounds) { return bounds; }
static void __zui_blank_draw(zcmd_widget *w) {}

// returns true if window is displayed
void zui_box() {
    __cont_alloc(ZW_BOX, sizeof(zcmd_box));
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
    zcmd_box *l = __cont_alloc(ZW_BOX, sizeof(zcmd_box));
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

void zui_window(i32 width, i32 height, float dt) {
    ctx->ui.used = 0;
    ctx->width = width;
    ctx->height = height;
    ctx->flags = 0;
    float ms = dt * 1000.0f + ctx->time.delta;
    u32 new_ms = (u32)ms;
    ctx->time.delta = ms - new_ms;
    ctx->time.ms += new_ms;
    zui_box();
}

// LABEL
void zui_label_n(char *text, i32 len) {
    zcmd_label *l = __ui_alloc(ZW_LABEL, sizeof(zcmd_label));
    l->text = text;
    l->len = len;
}

void zui_label(const char *text) {
    zui_label_n(text, (i32)strlen(text));
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
bool zui_button(const char *text, u8 *state) {
    zcmd_btn *l = __cont_alloc(ZW_BTN, sizeof(zcmd_btn));
    l->state = state;
    zui_label(text);
    zui_end();
    return *state;
}

// A button is just a clickable box
// This allows trivial addition of various button kinds: with images, multiple labels, etc.
// Reuse __zui_box_size for sizing

static void __zui_button_draw(zcmd_btn *btn) {
    zcolor c = (zcolor) { 80, 80, 80, 255 };
    if(__ui_cont_hovered(&btn->_)) {// hovered
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

bool zui_check(u8 *state) {
    zcmd_check *c = __ui_alloc(ZW_CHECK, sizeof(zcmd_check));
    c->state = state;
    return *state;
}

static zvec2 __zui_check_size(zcmd_check *data, zvec2 bounds) {
    zfont *font = ctx->font;
    zvec2 sz = font->text_size(font, "\xE2\x88\x9A", 3);
    return (zvec2) { sz.y + 2, sz.y + 2 };
}

static void __zui_check_draw(zcmd_check *data) {
    zcolor on =  (zcolor) { 60,  90,  250, 255 };
    zcolor off = (zcolor) { 200, 200, 200, 255 };
    if (__ui_hovered(&data->_)) {// hovered
        *data->state ^= __ui_clicked(ZM_LEFT_CLICK);
        on  = (zcolor) {  80, 110, 250, 255 };
        off = (zcolor) { 230, 230, 230, 255 };
    }
    zfont *font = ctx->font;
    zvec2 sz = font->text_size(font, "\xE2\x88\x9A", 3);
    zrect r = data->_.used;
    if (*data->state) {
        __push_rect_cmd(r, on, data->_.zindex);
        r = __rect_add(r, (zrect) { 1, 1, -2, -2 });
        __push_text_cmd(font, (zvec2) { r.x + (r.w - sz.x) / 2, r.y }, (zcolor) { 250, 250, 250, 255 }, "\xE2\x88\x9A", 3, data->_.zindex);
    }
    else {
        __push_rect_cmd(r, off, data->_.zindex);
    }
}

// create a slider with a formatted tooltip which accepts *value
void zui_sliderf(char *tooltip, f32 min, f32 max, f32 *value);
void zui_slideri(char *tooltip, i32 min, i32 max, i32 *value);

char *__zui_combo_get_option(zcmd_combo *c, i32 n, i32 *len) {
    if(n < 0) {
        *len = (i32)strlen(c->tooltip);
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
    if (cnt != n) return 0;
    *len = (i32)(s - option);
    return option;
}
// create a combo box with comma-seperated options
i32 zui_combo(char *tooltip, char *csoptions, i32 *state) {
    zcmd_combo *c = __cont_alloc(ZW_COMBO, sizeof(zcmd_combo));
    c->tooltip = tooltip;
    c->csoptions = csoptions;
    c->state = state;
    i32 len, i = 0;
    char *str;
    zui_blank();
    while ((str = __zui_combo_get_option(c, i++, &len))) {
        zui_box();
            zui_label_n(str, len);
        zui_end();
    }
    zui_end();
    return (*state >> 1) - 1;
}
static zvec2 __zui_combo_size(zcmd_combo *data, zvec2 bounds) {
    zfont *font = ctx->font;
    zvec2 auto_sz = font->text_size(font, data->tooltip, (i32)strlen(data->tooltip));
    zvec2 back_sz = (zvec2) { 0, 0 };
    bounds.y = Z_AUTO;
    zcmd_widget *background = 0, *child = 0;
    if (*data->state & 1) {
        background = __ui_get_child(&data->_);
        FOR_SIBLINGS(data, background) {
            zvec2 sz = __ui_sz(child, bounds);
            back_sz.x = auto_sz.x = max(auto_sz.x, sz.x);
            back_sz.y += sz.y;
        }
    }

    auto_sz.x += 10;
    auto_sz.y += 10;
    if(bounds.x != Z_AUTO)
        auto_sz.x = bounds.x;
    back_sz.x = auto_sz.x;
    if (*data->state & 1)
        __ui_sz(background, back_sz);
    return auto_sz;
}
static void __zui_combo_pos(zcmd_combo *data, zvec2 pos, i32 zindex) {
    if (~*data->state & 1) return;
    pos.y += data->_.used.h;
    zcmd_widget *background = __ui_get_child(&data->_);
    i32 prev = ctx->flags;
    ctx->flags = ZJ_LEFT;
    zindex += 1;
    __ui_pos(background, pos, zindex);
    FOR_SIBLINGS(data, background) {
        __ui_pos(child, pos, zindex);
        pos.y += child->used.h;
    }
    ctx->flags = prev;
}
static void __zui_combo_draw(zcmd_combo *box) {
    bool is_focused = &box->_ == __ui_widget(ctx->focused);
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
    __push_clip_cmd(background->used, background->zindex);
    __push_rect_cmd(background->used, (zcolor) { 80, 80, 80, 255 }, background->zindex);
    i32 i = 0;
    FOR_SIBLINGS(box, background) {
        __push_clip_cmd(background->used, child->zindex);
        if(__ui_cont_focused(child) && __ui_clicked(ZM_LEFT_CLICK)) {
            *box->state = ((i + 1) << 1); // close popup and set selected
        } else if(__ui_cont_hovered(child)) {
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
void zui_text(char *buffer, i32 len, zs_text *state) {
    zcmd_text *l = __ui_alloc(ZW_TEXT, sizeof(zcmd_text));
    l->_.flags |= ZF_TABBABLE;
    l->buffer = buffer;
    l->len = len;
    l->state = state;
}

static zvec2 __zui_text_size(zcmd_text *data, zvec2 bounds) {
    zfont *font = ctx->font;
    zvec2 sz = font->text_size(font, data->buffer, (i32)strlen(data->buffer));
    sz.x += 10;
    sz.y += 10;
    if(bounds.x != Z_AUTO) sz.x = bounds.x;
    return sz;
}

static i32 __zui_text_get_index(zcmd_text *data, i32 len) {
    zfont *font = ctx->font;
    zvec2 mp = ctx->input.mouse_pos;
    bool found = false;
    for (i32 i = 0; i < len; i++) {
        zvec2 sz = font->text_size(font, data->buffer, i + 1);
        sz.x += 5 - data->state->ofs;
        //zrect r = { data->_.used.x, data->_.used.y, sz.x + 5, sz.y + 10 };
        if (mp.x >= data->_.used.x && mp.x <= data->_.used.x + sz.x)
            return i;
    }
    if (!found && __vec_within(mp, data->_.used))
        return len;
    return -1;
}

static void __zui_text_draw(zcmd_text *data) {
    zs_text tctx = *(zs_text*)data->state;
    zfont *font = ctx->font;
    i32 len = (i32)strlen(data->buffer);
    if(&data->_ == __ui_widget(ctx->focused)) {
        i32 start = 0;
        // when there is a selection, the first input often has special behavior as the selection is removed
        if (ctx->input.text.used && tctx.selection) {
            char c = *(char*)ctx->input.text.data;
            switch (c) {
            case 9:
            case 27:
            case 20:
            case 19: break;
            case 18: tctx.index += tctx.selection;
            case 17: tctx.selection = 0; start++; break;
            case '\b': case 127: start++;
            default:
                for (i32 i = tctx.index; i < len; i++) {
                    data->buffer[i] = data->buffer[i + tctx.selection];
                    if (data->buffer[i] == 0)
                        break;
                }
                len -= tctx.selection;
                tctx.selection = 0;
                break;
            }
        }
        for(i32 i = start; i < ctx->input.text.used; i++) {
            char c = ((char*)ctx->input.text.data)[i];
            if (c == 9) { // on tab, switch focus to next tabbable element
                zcmd_widget *w = __ui_find_with_flag(&data->_, ZF_TABBABLE);
                if (w)
                    zapp_log("focusing %d\n", __ui_index(w));
                __ui_schedule_focus(w);
                break;
            }
            if(c == 27) { // escape focus on esc
                ctx->focused = 0;
                break;
            }
            if(c >= 17 && c <= 20) { // arrow keys
                if(c == 17 && tctx.index > 0)
                    tctx.index--;
                else if(c == 18 && tctx.index < len)
                    tctx.index++;
                continue;
            }
            if(c != '\b' && c != 127) { // write character
                if(len < data->len - 1) {
                    len++;
                    for(i32 j = data->len - 1; j > tctx.index; j--)
                        data->buffer[j] = data->buffer[j - 1];
                    data->buffer[tctx.index++] = c;
                }
                continue;
            }
            // backspace & delete
            if (c == 127 && !data->buffer[tctx.index]) continue;
            if (c == '\b') {
                if (tctx.index == 0) continue;
                tctx.index--;
            }
            len--;
            if(tctx.ofs > 0) {
                char tmp[2] = { data->buffer[tctx.index], 0 };
                tctx.ofs -= font->text_size(font, tmp, 1).x;
                tctx.ofs = max(tctx.ofs, 0);
            }
            for(i32 j = tctx.index; j < data->len - 1; j++)
                data->buffer[j] = data->buffer[j + 1];
        }
        data->buffer[data->len - 1] = 0;
    }
    else {
        tctx.selection = 0;
    }
    zvec2 textpos = { data->_.used.x + 5 - tctx.ofs, data->_.used.y + 5 };

    // handle mouse selection
    if (&data->_ == __ui_widget(ctx->focused)) {
        if (ctx->input.ctrl_a) {
            tctx.index = 0;
            tctx.selection = len;
        }
        else if (__ui_clicked(ZM_LEFT_CLICK)) {
            i32 index = __zui_text_get_index(data, len);
            tctx.selection = 0;
            tctx.flags = 0;
            if (index >= 0)
                tctx.index = index;
        }
        else if (__ui_dragged(ZM_LEFT_CLICK)) {
            i32 index = __zui_text_get_index(data, len);
            if (index >= 0) {
                i32 diff = index - tctx.index;
                if (diff > tctx.selection)
                    tctx.flags = 0;
                if (diff >= 0 && tctx.flags == 0) {
                    tctx.selection = diff;
                } else {
                    tctx.flags = 1;
                    tctx.selection -= diff;
                    tctx.index += diff;
                }
            }
        }
        //if (ctx->input.clipboard) // if copy request, send data
        //    ctx->input.clipboard(data->buffer + tctx.index, tctx.selection);
    }

    // handle text that is wider than the box (auto scroll left / right)
    zvec2 sz = font->text_size(font, data->buffer, tctx.index);
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

    // generate draw calls
    __push_rect_cmd(data->_.used, (zcolor) { 30, 30, 30, 255 }, data->_.zindex);
    if (true) {
        zvec2 selection = font->text_size(font, data->buffer + tctx.index, tctx.selection);
        zrect r = { textpos.x + sz.x, textpos.y, selection.x, selection.y };
        __push_rect_cmd(r, (zcolor) { 60, 60, 200, 255 }, data->_.zindex);
    }
    __push_text_cmd(ctx->font, textpos, (zcolor) { 250, 250, 250, 255 }, data->buffer, len, data->_.zindex);

    if(__ui_widget(ctx->focused) == &data->_) // draw cursor
        __push_rect_cmd(cursor, (zcolor) { 200, 200, 200, 255 }, data->_.zindex);

    *data->state = tctx;
}

// creates a multi-line text input
void zui_textbox(char *buffer, i32 len, i32 *state);

static void zui_layout(i32 id, i32 n, float *sizes) {
    i32 bytes = sizeof(zcmd_layout) + (n - 1) * sizeof(float);
    zcmd_layout *l = __cont_alloc(id, bytes);
    l->count = n;
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
	i32 i = 0;
	FOR_CHILDREN(data) {
		__ui_draw(child);
		i++;
	}
}

static zvec2 __zui_layout_size(zcmd_layout *data, zvec2 bounds) {
    // minimum size of empty container is 0, 0
    if(!__ui_has_child(&data->_))
        return (zvec2) { 0, 0 };

    // We share logic between rows and columns by having an AXIS variable
    // AXIS is x for ZW_ROW, y for ZW_COL
    bool AXIS = data->_.id - ZW_ROW;
    i32 i = 0, j = 0, end = data->count;
    i32 major_bound = bounds.e[AXIS];
    i32 minor_bound = bounds.e[!AXIS];
    i32 major = ctx->padding.e[AXIS] * (end - 1);
    i32 minor = 0; 

    // temporary steal some scratch memory from the ctx->ui buffer
    typedef struct { zcmd_widget *w; i32 idx; float sz; } cfg;
    cfg *configs = __buf_alloc(&ctx->draw, sizeof(cfg) * data->count);
    __buf_pop(&ctx->draw, sizeof(cfg) * data->count);

	FOR_CHILDREN(&data->_) configs[j] = (cfg) { child, j, data->data[j] }, j++;
    
    // move percentages to end of list (calculate them last)
    while(i < j) {
        while((configs[i].sz < -1.0f || configs[i].sz >= -0.0f) && i < j) i++;
		if (i >= j) break;
		do j--; while ((configs[j].sz >= -1.0f && configs[j].sz < -0.0f) && i < j);
        SWAP(cfg, configs[i], configs[j]);
    }
    float total_percent = 0;
    for(i = j; i < end; i++) total_percent += data->data[configs[i].idx];

    zvec2 child_bounds;
    child_bounds.e[!AXIS] = minor_bound;
    i32 pixels_total = 0;
    for(i = 0; i < end; i++) {
        float f = data->data[configs[i].idx];
		i32 bound = (i32)f;
        if(i >= j) {
            bound = (i32)((major_bound - major) * f / total_percent + 0.5f);
            total_percent -= f;
        }
        child_bounds.e[AXIS] = bound;
        zvec2 child_sz = __ui_sz(configs[i].w, child_bounds);
        minor = max(minor, child_sz.e[!AXIS]);
        major += bound == Z_AUTO ? child_sz.e[AXIS] : bound;
    }
	FOR_CHILDREN(data) child->bounds.e[2 + !AXIS] = minor;
    bounds.e[AXIS] = major;
    bounds.e[!AXIS] = minor;
    return bounds;
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
    zcmd_grid *l = __cont_alloc(ZW_GRID, bytes);
    l->rows = rows;
    l->cols = cols;
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
    __buf_init(&global_ctx.zdeque, 256);
    __buf_init(&global_ctx.input.text, 256);
    global_ctx.padding = (zvec2) { 5, 5 };
    global_ctx.next_size = (zvec2) { Z_NONE, Z_NONE };
    global_ctx.container = 0;
    ctx = &global_ctx;
    zui_register(ZW_BLANK, __zui_blank_size, 0, __zui_blank_draw);
    zui_register(ZW_BOX, __zui_box_size, __zui_box_pos, __zui_box_draw);
    zui_register(ZW_POPUP, __zui_popup_size, __zui_popup_pos, __zui_popup_draw);
    zui_register(ZW_LABEL, __zui_label_size, 0, __zui_label_draw);
    zui_register(ZW_COL, __zui_layout_size, __zui_layout_pos, __zui_layout_draw);
    zui_register(ZW_ROW, __zui_layout_size, __zui_layout_pos, __zui_layout_draw);
    zui_register(ZW_BTN, __zui_box_size, __zui_box_pos, __zui_button_draw);
    zui_register(ZW_CHECK, __zui_check_size, 0, __zui_check_draw);
    zui_register(ZW_TEXT, __zui_text_size, 0, __zui_text_draw);
    zui_register(ZW_COMBO, __zui_combo_size, __zui_combo_pos, __zui_combo_draw);
    //zui_register(ZW_GRID, __zui_grid_size, __zui_grid_pos, __zui_grid_draw);
}

void zui_close() {
    free(ctx->draw.data);
    free(ctx->ui.data);
    free(ctx->registry.data);
    free(ctx->stack.data);
    free(ctx->zdeque.data);
    free(ctx->input.text.data);
    ctx = 0;
}
