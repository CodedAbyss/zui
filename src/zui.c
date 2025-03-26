#include "zui.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))

//#define assert(bool, msg) { if(!(bool)) printf(msg); exit(1); }
#define FOR_CHILDREN(ui) for(zcmd_widget* child = __ui_get_child((zcmd_widget*)ui); child != (zcmd_widget*)ui; child = __ui_next(child))
#define FOR_SIBLINGS(ui, sibling) for(zcmd_widget* child = (zcmd_widget*)sibling; child != (zcmd_widget*)ui; child = __ui_next(child))
#define FOR_N_SIBLINGS(ui, sibling, n) for(i32 i = 0; sibling != (zcmd_widget*)ui && i < n; sibling = __ui_next(sibling), i++)
#define SWAP(type, a, b) { type tmp = a; a = b; b = tmp; }

typedef struct zui_type {
    zvec2 (*size)(void*, zvec2);
    void (*pos)(void*, zvec2, i32);
    void (*draw)(void*);
} zui_type;

typedef struct zui_buf {
    i32 used;
    u16 cap;
    u16 alignsub1;
    u8 *data;
} zui_buf;

static void __buf_init(zui_buf *l, i32 cap, i32 alignment) {
    // get log2 of cap
    union { f32 f; i32 i; } tmp = { .f = (float)(cap - 1) };
    l->cap = (tmp.i >> 23) - 126;
    l->used = 0;
    l->alignsub1 = alignment - 1;
    l->data = malloc(cap);
}
static void *__buf_alloc(zui_buf *l, i32 size) {
    size = (size + l->alignsub1) & ~l->alignsub1;
    if(l->used + size > (1 << l->cap)) {
        l->cap++;
        l->data = realloc(l->data, 1 << l->cap);
    }
    void *ret = l->data + l->used;
    l->used += size;
    return ret;
}
static void *__buf_pop(zui_buf *l, i32 size) {
    l->used -= ((size + l->alignsub1) & ~l->alignsub1);
    return l->data + l->used;
}

// Dead simple map implementation
// Our use case doesn't require deletions which simplifies logic quite a bit
typedef struct zmap {
    u32 cap;
    u32 used;
    u64 *data; // layout: <1 bit if slot occupied><31 bits of value><32 bits for key>
} zmap;
static void _zmap_init(zmap *map) {
    map->used = 0;
    map->cap = 16;
    map->data = calloc(map->cap, sizeof(u64));
}
// hash bits so we don't have to deal with collisions as much
// This hashing function is 31 bit. We use the top bit to determine whether a hashmap slot is filled.
// That's why it does key << 1 >> 17. This does a 16-bit right shift while also clearing the top bit
// It's also reversible so the hashing creates no collisions
static u32 _zmap_hash(u32 key) {
    key = (((key << 1 >> 17) ^ key) * 0x45d9f3b);
    key = (((key << 1 >> 17) ^ key) * 0x45d9f3b);
    return ((key << 1 >> 17) ^ key) | 0x80000000;
}
static u64 *_zmap_node(zmap *map, u32 key) {
    i32 index = key % map->cap;
    while ((u32)map->data[index] && (u32)map->data[index] != key)
    //while ((i32)((u32)map->data[index] ^ key) > 0)
        index = (index + 1) % map->cap;
    return &map->data[index];
}
static bool _zmap_get(zmap *map, u32 key, u32 *value) {
    u64 *node = _zmap_node(map, key);
    *value = (*node >> 32);
    return *(u32*)node != 0;
}
static void _zmap_set(zmap *map, u32 key, u32 value) {
    if (map->used * 4 > map->cap * 3) { // if load-factor > 75%, rehash
        u64 *old = (u64*)map->data;
        map->cap *= 2;
        map->data = calloc(map->cap, sizeof(u64));
        for (i32 i = 0; i < map->cap / 2; i++)
            if ((u32)old[i])
                *_zmap_node(map, (u32)old[i]) = old[i];
        free(old);
    }
    u64 *node = _zmap_node(map, key);
    if (!*node) map->used++;
    *node = ((u64)value << 32) | key;
}
// zui-glyph-cache hash
static u32 _zgc_hash(u16 font_id, i32 codepoint) {
    // code point must be under U+10FFFF so we can include the font_id in the key
    return _zmap_hash((font_id << 21) | (codepoint & 0x1FFFFF));
}
// zui-style hash
static u32 _zs_hash(u16 widget_id, u16 style_id) {
    return _zmap_hash(((u32)widget_id << 16) | style_id);
}

static u8 __utf8_masks[] = { 0, 0x7F, 0x1F, 0xF, 0x7 };
i32 __utf8_val(char *text, u32 *codepoint) {
    static u8 utf8len[] = { 1,1,1,1,1,1,1,1,0,0,0,0,2,2,3,4 };
    i32 len = utf8len[(u8)(*text) >> 4];
    *codepoint = text[0] & __utf8_masks[len];
    for (i32 i = 1; i < len; i++)
        *codepoint = (*codepoint << 6) | (text[i] & 0x3F);
    return len;
}
i32 __utf8_len(u32 codepoint) {
    if (codepoint < 0) return 0;
    if (codepoint < 0x80) return 1;
    if (codepoint < 0x800) return 2;
    if (codepoint < 0x10000) return 3;
    if (codepoint < 0x110000) return 4;
    return 0;
}
void __utf8_print(char *text, u32 codepoint, i32 len) {
    for (i32 i = len - 1; i > 0; codepoint >>= 6)
        text[i--] = 0xC0 | (codepoint & 0x3F);
    text[0] = codepoint & __utf8_masks[len];
}

typedef struct zstyle { u16 widget_id; u16 style_id; union { zcolor c; zvec2 v; i32 i; u32 u; f32 f; } value; } zstyle;

typedef struct zui_client {
    zvec2 window_sz;
    zvec2 mouse_pos;
    u16 mouse_state;
    u16 modifiers;
    u16 font_cnt;
    zui_client_fn send;
    zui_render_fn recv;
    zui_log_fn log;
    void *user_data;
    zui_buf commands;
    zmap glyphs;
    zscmd *active_cmd;
    i32 response;
    bool just_rendered;
} zui_client;

static zui_client *client;

void zui_client_init(zui_client_fn send, zui_render_fn recv, zui_log_fn log, void *user_data) {
    static zui_client global_client = { 0 };
    client = &global_client;
    client->log = log;
    client->send = send;
    client->recv = recv;
    client->user_data = user_data;
    __buf_init(&client->commands, 256, sizeof(void*));
    _zmap_init(&client->glyphs);
}

void zui_client_respond(i32 value) {
    client->response = value;
    zscmd *cmd = client->active_cmd;
    if (!cmd) return;
    switch (cmd->base.id) {
    case ZSCMD_GLYPH: {
        zccmd_glyph glyph = {
            .header = { ZCCMD_GLYPH, sizeof(zccmd_glyph) },
            .c = { cmd->glyph.c.font_id, (u16)client->response, cmd->glyph.c.c }
        };
        client->send((zccmd*)&glyph, client->user_data);
    } break;
    //case ZSCMD_FONT: {
    //    zccmd_font
    //}
    }
}

void zui_client_push(zscmd *cmd) {
    if (cmd->base.id >= ZSCMD_CLIP && cmd->base.id <= ZSCMD_TEXT) { // draw commands are pushed to the draw stack
        if (client->just_rendered) { // only clear previous draw commands once we receive a new set.
            client->just_rendered = false;
            client->commands.used = 0;
        }
        memcpy(__buf_alloc(&client->commands, cmd->base.bytes), cmd, cmd->base.bytes);
        return;
    }
    // non-draw commands are executed immediately
    client->active_cmd = cmd;
    client->recv(cmd, client->user_data);
    client->active_cmd = 0;
}

void zui_client_push_raw(char *bytes, i32 len) {
    if (len < sizeof(zcmd)) return;
    zscmd *cmd = (zscmd*)bytes;
    if (cmd->base.bytes > len) return;
    zui_client_push((zscmd*)cmd);
    zui_client_push_raw(bytes + cmd->base.bytes, len - cmd->base.bytes);
}

void zui_client_render() {
    char *ptr = (char*)client->commands.data;
    char *end = ptr + client->commands.used;
    while (ptr < end) {
        zscmd *cmd = (zscmd*)ptr;
        client->recv(cmd, client->user_data);
        ptr += (cmd->base.bytes + client->commands.alignsub1) & ~client->commands.alignsub1;
    }
    client->just_rendered = true;
}

void __zui_send_mouse_data() {
    zccmd_mouse data = {
        .header = { ZCCMD_MOUSE, sizeof(zccmd_mouse) },
        .pos = client->mouse_pos,
        .state = client->mouse_state
    };
    client->send((zccmd*)&data, client->user_data);
}

typedef struct zui_ctx {
    zvec2 window_sz;
    zui_render_fn renderer;
    zui_log_fn log;
    void *user_data;
    zvec2 padding;
    u16 font_id;
    zvec2 next_size;
    u32 flags;
    u32 font_cnt;
    i32 latest;
    i32 style_edits;
    zui_buf registry;
    zui_buf ui;
    zui_buf draw;
    zui_buf stack;
    zui_buf zdeque;
    zmap glyphs;
    zmap style;
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

static zui_ctx *ctx = 0;

static void _log(char *fmt, ...) {
    if (!client && !ctx) return;
    void *user_data = client ? client->user_data : ctx->user_data;
    zui_log_fn fn = client ? client->log : ctx->log;
    if (!fn) return;
    va_list args;
    va_start(args, fmt);
    fn(fmt, args, user_data);
    va_end(args);
}

zvec2 zui_text_sz(u16 font_id, char *text, i32 len) {
    zvec2 ret = { 0, 0 };
    u32 codepoint, tmp, w = 0, h = 0;
    if(!_zmap_get(&ctx->glyphs, _zgc_hash(font_id, 0x1FFFFF), &h))
        return (zvec2) { 0, 0 };
    for (i32 bw, i = 0; i < len; i += bw, w += tmp) {
        bw = __utf8_val(text + i, &codepoint);
        if (!bw) return (zvec2) { 0, 0 };
        u32 key = _zgc_hash(font_id, codepoint);
        if (_zmap_get(&ctx->glyphs, key, &tmp))
            continue;
        // request renderer for glyph width
        zscmd_glyph cmd = {
            .header = { ZSCMD_GLYPH, sizeof(zscmd_glyph) },
            .c = { font_id, 0, codepoint }    
        };
        ctx->renderer((zscmd*)&cmd, ctx->user_data);
        if (!_zmap_get(&ctx->glyphs, key, &tmp))
            return (zvec2) { 0, 0 };
    }
    return (zvec2) { (u16)w, (u16)h };    
}

void zui_mouse_down(u16 btn) {
    if (!client) {
        ctx->input.mouse_state |= btn;
        return;
    }
    client->mouse_state |= btn;
    __zui_send_mouse_data();
}
void zui_mouse_up(u16 btn) {
    if (!client) {
        ctx->input.mouse_state &= ~btn;
        return;
    }
    client->mouse_state &= ~btn;
    __zui_send_mouse_data();
}
void zui_mouse_move(zvec2 pos) {
    if (!client) {
        ctx->input.mouse_pos = pos;
        return;
    }
    client->mouse_pos = pos;
    __zui_send_mouse_data();
}
void zui_key_mods(u16 mod) {
    if (!client) return;
    client->modifiers = mod;
}
void zui_key_char(i32 c) {
    // send glyph info for all fonts if necessary
    if (!client) {
        i32 len = __utf8_len(c);
        char *utf8 = (char*)__buf_alloc(&ctx->input.text, len);
        __utf8_print(utf8, c, len);
        return;
    }
    for (u16 id = 0; id < client->font_cnt; id++) {
        u32 value;
        if (_zmap_get(&client->glyphs, _zgc_hash(id, c), &value)) continue;
        zscmd_glyph glyph = {
            .header = { ZSCMD_GLYPH, sizeof(zscmd_glyph) },
            .c = (zglyph_data) {
                .font_id = id,
                .c = c,
                .width = 0
            }
        };
        zui_client_push((zscmd*)&glyph);
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
    client->send((zccmd*)&win, client->user_data);
}
static inline zcmd_widget *__ui_widget(i32 index) {
    return (zcmd_widget*)(ctx->ui.data + index);
}
static inline i32 __ui_index(zcmd_widget *ui) {
    return (i32)((u8*)ui - (u8*)ctx->ui.data);
}
// returns next ui element in the container
// if none left, returns parent
static inline zcmd_widget *__ui_next(zcmd_widget *widget) {
    return __ui_widget(widget->next);
}
static void *__ui_alloc(i32 id, i32 size) {
    zcmd_widget *prev = __ui_widget(ctx->latest);
    ctx->latest = prev->next = ctx->ui.used;
    zcmd_widget *widget = __buf_alloc(&ctx->ui, size);
    memset(widget, 0, size);
    widget->id = id;
    widget->bytes = size;
    widget->flags = ctx->flags;
    return widget;
}
static void *__cont_alloc(i32 id, i32 size) {
    zstyle *edits = (zstyle*)(ctx->stack.data + ctx->stack.used) - ctx->style_edits;
    for (i32 i = 0; i < ctx->style_edits; i++) {
        u32 value, key = _zs_hash(edits[i].widget_id, edits[i].style_id);
        if (!_zmap_get(&ctx->style, key, &value))
            _log("No default style associated with widget:%d, style-id:%d\n", edits[i].widget_id, edits[i].style_id);
        _zmap_set(&ctx->style, key, edits[i].value.u); // load new value into map
        edits[i].value.u = value; // backup previous value on the stack
    }
    i32 *arr = __buf_alloc(&ctx->stack, sizeof(i32) * 2);
    arr[0] = ctx->ui.used;
    arr[1] = ctx->style_edits;
    ctx->style_edits = 0;
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
static zrect __rect_add(zrect a, zrect b) {
    return (zrect) { a.x + b.x, a.y + b.y, a.w + b.w, a.h + b.h };
}
static zrect __rect_pad(zrect r, zvec2 padding) {
    return (zrect) { r.x - padding.x, r.y - padding.y, r.w + padding.x * 2, r.h + padding.y * 2 };
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
static void __push_text_cmd(u16 font_id, zvec2 coord, zcolor color, char *text, i32 len, i32 zindex) {
    zscmd_text *r = __draw_alloc(ZSCMD_TEXT, sizeof(zscmd_text) + len, zindex);
    r->font_id = font_id;
    r->pos = coord;
    r->color = color;
    memcpy(r->text, text, len);
}

static zvec2 _vec_max(zvec2 a, zvec2 b) { return (zvec2) { max(a.x, b.x), max(a.y, b.y) }; }
static zvec2 _vec_min(zvec2 a, zvec2 b) { return (zvec2) { min(a.x, b.x), min(a.y, b.y) }; }
static zvec2 _vec_add(zvec2 a, zvec2 b) { return (zvec2) { a.x + b.x, a.y + b.y }; }

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
    if (~ui->flags & ZF_PARENT) return ui;
    return (zcmd_widget*)((u8*)ui + ((ui->bytes + ctx->ui.alignsub1) & ~ctx->ui.alignsub1));
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

void __ui_print(zcmd_widget *cmd, int indent) {
    _log("%04x | ", __ui_index(cmd));
    for (i32 i = 0; i < indent; i++)
        _log("    ");    
    zrect b = cmd->bounds;
    zrect u = cmd->used;
    _log("(id: %d, next: %04x, bounds: {%d,%d,%d,%d}, used: {%d,%d,%d,%d})\n", cmd->id, cmd->next, b.x, b.y, b.w, b.h, u.x, u.y, u.w, u.h);
    FOR_CHILDREN(cmd)
        __ui_print(child, indent + 1);
}

zvec2 __ui_sz(zcmd_widget *ui, zvec2 bounds) {
    zui_type type = ((zui_type*)ctx->registry.data)[ui->id - ZW_FIRST];
    zvec2 sz = type.size(ui, bounds);
    ui->used.w = sz.x;
    ui->used.h = sz.y;
    ui->bounds.w = bounds.x == Z_AUTO ? ui->used.w : bounds.x;
    ui->bounds.h = bounds.y == Z_AUTO ? ui->used.h : bounds.y;
    return (zvec2) { ui->bounds.w, ui->bounds.h };
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
    if(__ui_has_child(ui))
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

void zui_print_tree() {
    _log("printing tree...\n");
    __ui_print(__ui_widget(0), 0);
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

u16 zui_new_font(char *family, i32 size) {
    _zmap_set(&ctx->glyphs, _zgc_hash(ctx->font_cnt, 0x1FFFFF), size);
    i32 bytes = sizeof(zscmd_font) + strlen(family);
    zscmd_font *font = _alloca(bytes);
    font->header = (zcmd) { ZSCMD_FONT, bytes };
    font->font_id = ctx->font_cnt;
    font->size = size;
    memcpy(font->family, family, bytes - sizeof(zscmd_font));
    ctx->renderer((zscmd*)font, ctx->user_data);
    return ctx->font_cnt++;
}

// set zui font
void zui_font(u16 font_id) {
    ctx->font_id = font_id;
}

void __zui_qsort(u64 *nums, i32 count) {
    if (count < 2) return;
    // the median should generally be in the center as most widgets will have the same zindex
    SWAP(u64, nums[0], nums[count / 2]);
    u64 pivot = nums[0];
    i32 i = 1, j = count - 1;
    do {
        while (nums[i] <= pivot && i < j) i++;
        while (nums[j] > pivot && i <= j) j--;
        if (j <= i) i = 0;
        SWAP(u64, nums[i], nums[j]);
    } while (i);
    __zui_qsort(nums, j++);
    __zui_qsort(nums + j, count - j);
}

// ends any container (window / grid)
void zui_end() {
    // nums is the index of the container, followed by the # of style edits
    i32 *nums = (i32*)__buf_pop(&ctx->stack, sizeof(i32) * 2);
    zstyle *edits = (zstyle*)__buf_pop(&ctx->stack, sizeof(zstyle) * nums[1]);
    for (i32 i = 0; i < nums[1]; i++)
        _zmap_set(&ctx->style, _zs_hash(edits[i].widget_id, edits[i].style_id), edits[i].value.u); // restore old style value to map

    // if active container isn't the most recent element set parent flag (it has children)
    zcmd_widget* latest = __ui_widget(ctx->latest);
    if (nums[0] != ctx->latest) {
        __ui_widget(nums[0])->flags |= ZF_PARENT;
        ctx->latest = latest->next = nums[0];
    }
}

void zui_style(u32 widget_id, ...) {
    va_list args;
    va_start(args, widget_id);
    for (u32 style_id; (style_id = va_arg(args, u32)) != ZS_DONE; ) {
        zstyle *style = (zstyle*)__buf_alloc(&ctx->stack, sizeof(zstyle));
        *style = (zstyle) { widget_id, style_id, .value.u = va_arg(args, u32) };
        ctx->style_edits++;
    }
    va_end(args);
}
void zui_default_style(u32 widget_id, ...) {
    va_list args;
    va_start(args, widget_id);
    for (u32 style_id; (style_id = va_arg(args, u32)) != ZS_DONE; ) {
        u32 value = va_arg(args, u32);
        _zmap_set(&ctx->style, _zs_hash(widget_id, style_id), value);
    }
    va_end(args);
}
static void _zui_get_style(u16 widget_id, u16 style_id, void *ptr) {
    if(!_zmap_get(&ctx->style, _zs_hash(widget_id, style_id), (u32*)ptr))
        _log("No style exists for widget:%d,style:%d\n", widget_id, style_id);
}
zcolor zui_stylec(u16 widget_id, u16 style_id) { zcolor ret; _zui_get_style(widget_id, style_id, &ret); return ret; }
zvec2  zui_stylev(u16 widget_id, u16 style_id) { zvec2  ret; _zui_get_style(widget_id, style_id, &ret); return ret; }
float  zui_stylef(u16 widget_id, u16 style_id) { float  ret; _zui_get_style(widget_id, style_id, &ret); return ret; }
i32    zui_stylei(u16 widget_id, u16 style_id) { i32    ret; _zui_get_style(widget_id, style_id, &ret); return ret; }

void zui_render() {
    if (ctx->stack.used != 0 || ctx->window_sz.x == 0 || ctx->window_sz.y == 0)
        return;

    // calculate sizes
    zcmd_widget *root = __ui_widget(0);
    root->next = 0;
    __ui_sz(root, ctx->window_sz);

    // calculate positions
    ctx->hovered = 0;
    root->bounds.x = 0;
    root->bounds.y = 0;
    __ui_pos(root, (zvec2) { 0, 0 }, 0);
    if (ctx->__focused) {
        ctx->focused = ctx->__focused;
        ctx->__focused = 0;
    }

    // generate draw commands
    __ui_draw(root);

    // sort draw commands by zindex / index (order of creation)
    // despite qsort not being a stable sort, the order of draw cmd creation is preserved due to index being part of each u64
    u64 *deque_reader = (u64*)ctx->zdeque.data;
    __zui_qsort(deque_reader, ctx->zdeque.used / sizeof(u64));
    while(deque_reader < (u64*)(ctx->zdeque.data + ctx->zdeque.used)) {
        u64 next_pair = *deque_reader++;
        i32 index = next_pair & 0x7FFFFFFF;
        zscmd *next = (zscmd*)(ctx->draw.data + index);
        ctx->renderer(next, ctx->user_data);
    }
    zcmd draw = { ZSCMD_DRAW, sizeof(zcmd) };
    ctx->renderer((zscmd*)&draw, ctx->user_data);
    ctx->input.prev_mouse_pos = ctx->input.mouse_pos;
    ctx->input.prev_mouse_state = ctx->input.mouse_state;
    ctx->input.text.used = 0;
    ctx->zdeque.used = 0;
    ctx->draw.used = 0;
    ctx->ui.used = 0;
}

void zui_push(zccmd *cmd) {
    switch (cmd->base.id) {
    case ZCCMD_MOUSE:
        ctx->input.mouse_pos = cmd->mouse.pos;
        ctx->input.mouse_state = cmd->mouse.state;
        break;
    case ZCCMD_KEYS: {
        i32 len = __utf8_len(cmd->keys.key);
        char *utf8 = (char*)__buf_alloc(&ctx->input.text, len);
        __utf8_print(utf8, cmd->keys.key, len);
    } break;
    case ZCCMD_GLYPH:
        _zmap_set(&ctx->glyphs, _zgc_hash(cmd->glyph.c.font_id, cmd->glyph.c.c), cmd->glyph.c.width);
        break;
    case ZCCMD_WIN:
        ctx->window_sz = cmd->win.sz;
        break;
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
    __push_rect_cmd(box->_.bounds, zui_stylec(ZW_BOX, ZSC_BACKGROUND), box->_.zindex);
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

void zui_window() {
    ctx->ui.used = 0;
    ctx->flags = 0;
    __cont_alloc(ZW_WINDOW, sizeof(zcmd_box));
}
static zvec2 __zui_window_size(zcmd_box *window, zvec2 bounds) {
    FOR_CHILDREN(window) __ui_sz(child, bounds);
    return bounds;
}
static void __zui_window_pos(zcmd_box *window, zvec2 pos) {
    FOR_CHILDREN(window) __ui_pos(child, pos, window->_.zindex);
}

zvec2 zui_window_sz() {
    return ctx->window_sz;
}

// LABEL
void zui_label_n(char *text, i32 len) {
    zcmd_label *l = __ui_alloc(ZW_LABEL, sizeof(zcmd_label));
    l->text = text;
    l->len = len;
}

void zui_label(const char *text) {
    zui_label_n((char*)text, (i32)strlen(text));
}

static zvec2 __zui_label_size(zcmd_label *data, zvec2 bounds) {
    return zui_text_sz(ctx->font_id, data->text, data->len);
}

static void __zui_label_draw(zcmd_label *data) {
    zvec2 coords = { data->_.used.x, data->_.used.y };
    __push_text_cmd(ctx->font_id, coords, zui_stylec(ZW_LABEL, ZSC_FOREGROUND), data->text, data->len, data->_.zindex);
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
    zvec2 sz = zui_text_sz(ctx->font_id, "\xE2\x88\x9A", 3);
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
    zvec2 sz = zui_text_sz(ctx->font_id, "\xE2\x88\x9A", 3);
    zrect r = data->_.used;
    if (*data->state) {
        __push_rect_cmd(r, on, data->_.zindex);
        r = __rect_add(r, (zrect) { 1, 1, -2, -2 });
        __push_text_cmd(ctx->font_id, (zvec2) { r.x + (r.w - sz.x) / 2, r.y }, (zcolor) { 250, 250, 250, 255 }, "\xE2\x88\x9A", 3, data->_.zindex);
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
    zvec2 auto_sz = zui_text_sz(ctx->font_id, data->tooltip, (i32)strlen(data->tooltip));
    zvec2 back_sz = (zvec2) { 0, 0 };
    bounds.y = Z_AUTO;
    zcmd_widget *background = 0, *child = 0;
    if (*data->state & 1) {
        background = __ui_get_child(&data->_);
        FOR_SIBLINGS(data, __ui_next(background)) {
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
    FOR_SIBLINGS(data, __ui_next(background)) {
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
    __push_text_cmd(ctx->font_id, pos, (zcolor) { 230, 230, 230, 255 }, text, len, box->_.zindex);

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
    FOR_SIBLINGS(box, __ui_next(background)) {
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
    zvec2 sz = zui_text_sz(ctx->font_id, data->buffer, (i32)strlen(data->buffer));
    sz.x += 10;
    sz.y += 10;
    if(bounds.x != Z_AUTO) sz.x = bounds.x;
    return sz;
}

static i32 __zui_text_get_index(zcmd_text *data, i32 len) {
    zvec2 mp = ctx->input.mouse_pos;
    bool found = false;
    for (i32 i = 0; i < len; i++) {
        zvec2 sz = zui_text_sz(ctx->font_id, data->buffer, i + 1);
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
                tctx.ofs -= zui_text_sz(ctx->font_id, tmp, 1).x;
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
    zvec2 sz = zui_text_sz(ctx->font_id, data->buffer, tctx.index);
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

    zvec2 selection = zui_text_sz(ctx->font_id, data->buffer + tctx.index, tctx.selection);
    zrect r = { textpos.x + sz.x, textpos.y, selection.x, selection.y };
    __push_rect_cmd(r, (zcolor) { 60, 60, 200, 255 }, data->_.zindex);

    __push_text_cmd(ctx->font_id, textpos, (zcolor) { 250, 250, 250, 255 }, data->buffer, len, data->_.zindex);

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
    FOR_CHILDREN(data)
        __ui_draw(child);
}

// move all values with either of the high bits set to the end of the array
static i32 __zui_partition(u64 *values, i32 len) {
    i32 i = 0, j = len - 1;
    while (1) {
        while (!(values[i] >> 63) && i < j) i++;
        while ((values[j] >> 63) && i <= j) j--;
        if (j <= i) break;
        SWAP(u64, values[i], values[j]);
    }
    return j;
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

    u64 *children = _alloca(sizeof(u64) * data->count);
    FOR_CHILDREN(data) children[j] = ((u64)(data->data[j] < 0) << 63) | ((u64)j << 32) | __ui_index(child);
 
    // move percentages to end of list (calculate them last)
    j = __zui_partition(children, data->count);
    zvec2 child_bounds;
    i32 pixels_left;
    child_bounds.e[!AXIS] = minor_bound;
    for(i = 0; i < end; i++) {
        float f = data->data[(u16)(children[i] >> 32)];
        i32 bound = f < 0 ? (i32)(pixels_left * -f + 0.5f) : (i32)f;
        child_bounds.e[AXIS] = bound;
        zvec2 child_sz = __ui_sz(__ui_widget((i32)children[i]), child_bounds);
        minor = max(minor, child_sz.e[!AXIS]);
        major += bound == Z_AUTO ? child_sz.e[AXIS] : bound;
        if (i == j) pixels_left = major_bound - major;
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
void zui_grid(i32 cols, i32 rows, float *col_row_settings) {
    i32 bytes = sizeof(zcmd_grid) + (rows + cols - 1) * sizeof(float);
    zcmd_grid *l = __cont_alloc(ZW_GRID, bytes);
    l->rows = rows;
    l->cols = cols;
    if(col_row_settings)
        memcpy(l->data, col_row_settings, (rows + cols) * sizeof(float));
    else {
        for(i32 i = 0; i < (rows + cols); i++)
            l->data[i] = Z_AUTO;
    }
}

static zvec2 __zui_grid_size(zcmd_grid *grid, zvec2 bounds) {
    zvec2 used = { ctx->padding.x * (grid->cols - 1), ctx->padding.y * (grid->rows - 1) };
    u16 *sizes = _alloca(sizeof(u16) * (grid->rows + grid->cols));
    u8 flag = 0;
    for (i32 i = 0; i < grid->rows + grid->cols; i++) {
        sizes[i] = 0;
        bool axis = i < grid->rows;
        if (grid->data[i] == Z_AUTO) flag |= axis ? 1 : 2;
        else if (grid->data[i] < 0)  flag |= axis ? 4 : 8;
    }
    if (flag == 15)
        _log("ERR: grid cannot have both percentage and auto sized layouts for both axes\n");

    // precalc percentages if possible
    for (i32 i = 0; i < grid->rows + grid->cols; i++) {
        bool axis = i < grid->rows;
        if (grid->data[i] < 0 && ((flag >> axis) & 5) == 4)
            grid->data[i] = (u16)((bounds.e[axis] - used.e[axis]) * grid->data[i]);
    }
    
    i32 n = 0;
    u64 *children = _alloca(sizeof(u64) * (grid->rows * grid->cols));
    FOR_CHILDREN(grid) {
        i32 x = n % grid->rows, y = n / grid->rows + grid->rows;
        children[n] = ((u64)(grid->data[x] < 0 || grid->data[y] < 0) << 63) | ((u64)n << 32) | __ui_index(child);
        n++;
    }
    if (n != grid->rows * grid->cols)
        _log("ERR: grid has wrong # of children\r\n");

    // high bit represents if the element is % sized, all percentage sized elements are calculated last
    zvec2 non_percent = used;
    zvec2 child_bounds;
    __zui_partition(children, n);
    for (i32 i = 0; i < n; i++) {
        i32 child = (children[i] >> 32) & 0x7FFFFFFF;
        i32 x = child % grid->cols, y = child / grid->cols + grid->cols;
        float w = grid->data[x], h = grid->data[y];
        child_bounds.x = w >= 0 ? w : (bounds.x - non_percent.x) * w;
        child_bounds.y = h >= 0 ? h : (bounds.y - non_percent.y) * h;
        zcmd_widget *widget = __ui_widget((u32)children[i]);
        zvec2 sz = __ui_sz(widget, child_bounds);
        u16 ofsx = max(sizes[x], sz.x) - sizes[x];
        u16 ofsy = max(sizes[y], sz.y) - sizes[y];
        if (w < 0) non_percent.x += ofsx;
        if (h < 0) non_percent.y += ofsy;
        sizes[x] += ofsx;
        sizes[y] += ofsy;
        used.x += ofsx;
        used.y += ofsy;
    }

    for (i32 i = 0; i < n; i++) {
        i32 child = (children[i] >> 32) & 0x7FFFFFFF;
        i32 x = child % grid->cols, y = child / grid->cols + grid->cols;
        if ((u16)grid->data[x] == Z_AUTO) __ui_widget((u32)children[i])->bounds.w = sizes[x];
        if ((u16)grid->data[y] == Z_AUTO) __ui_widget((u32)children[i])->bounds.h = sizes[y];
    }

    return used;
}

static void __zui_grid_pos(zcmd_grid *grid, zvec2 pos, i32 zindex) {
    i32 n = 0;
    u16 x = pos.x;
    FOR_CHILDREN(grid) {
        __ui_pos(child, pos, zindex);
        pos.x += ctx->padding.x + child->bounds.w;
        if ((n + 1) % grid->cols == 0) {
            pos.y += ctx->padding.y + child->bounds.h;
            pos.x = x;
        }
        n++;
    }
}

static void __zui_grid_draw(zcmd_grid *grid) {
    zvec2 borders = zui_stylev(ZW_GRID, ZSV_BORDER);
    zcolor border_color = zui_stylec(ZW_GRID, ZSC_FOREGROUND);
    i32 i = 0;
    zrect used = grid->_.used;
    zvec2 pos = { used.x, used.y };
    FOR_CHILDREN(grid) {
        if (i < grid->cols - 1) {
            pos.x += child->bounds.w;
            zrect box = { pos.x + (ctx->padding.x - borders.x) / 2, used.y, borders.x, used.h };
            pos.x += ctx->padding.x;
            __push_rect_cmd(box, border_color, grid->_.zindex);
        }
        if (i % grid->cols == 0 && i < grid->cols * (grid->rows - 1)) {
            pos.y += child->bounds.h;
            zrect box = { used.x, pos.y + (ctx->padding.y - borders.y) / 2, used.w, borders.y };
            pos.y += ctx->padding.y;
            __push_rect_cmd(box, border_color, grid->_.zindex);
        }
        __ui_draw(child);
        i++;
    }
}

static bool _zui_cslen(char *cs, i32 *len) {
    i32 i = 0;
    while (cs[i] && cs[i] != ',') i++;
    *len = i;
    return cs[i] != 0;
}

void zui_tabset(char *cstabs, i32 *state) {
    zcmd_tabset *l = __cont_alloc(ZW_TABSET, sizeof(zcmd_tabset));
    l->cstabs = cstabs;
    l->state = state;
    i32 len;
    bool loop;
    do {
        loop = _zui_cslen(cstabs, &len);
        zui_label_n(cstabs, len);
        cstabs += len + 1;
        l->label_cnt++;
    } while (loop);
}

static zvec2 _zui_tabset_size(zcmd_tabset *tabs, zvec2 bounds) {
    zvec2 padding = zui_stylev(ZW_TABSET, ZSV_PADDING);
    zvec2 tab_size = { padding.x * 2 * tabs->label_cnt, 0 };
    zcmd_widget *label = __ui_get_child(&tabs->_);
    for (i32 i = 0; i < tabs->label_cnt; i++) {
        zvec2 sz = __ui_sz(label, (zvec2) { Z_AUTO, Z_AUTO });
        tab_size.x += sz.x;
        tab_size.y = max(tab_size.y, sz.y);
        label = __ui_next(label);
    }
    tabs->tabheight = tab_size.y + padding.y * 2;
    zvec2 child_size = { 0, 0 };
    if (bounds.y != Z_AUTO) bounds.y -= tabs->tabheight;
    FOR_SIBLINGS(tabs, label) {
        zvec2 sz = __ui_sz(child, bounds);
        child_size = _vec_max(child_size, sz);
    }
    if (bounds.x == Z_AUTO) bounds.x = max(tab_size.x, child_size.x);
    if (bounds.y == Z_AUTO) bounds.y = tab_size.y + child_size.y;
    return bounds;
}

static void _zui_tabset_pos(zcmd_tabset *tabs, zvec2 pos) {
    zcmd_widget *label = __ui_get_child(&tabs->_);
    zvec2 padding = zui_stylev(ZW_TABSET, ZSV_PADDING);
    zvec2 tmp = { pos.x, pos.y + tabs->tabheight };
    pos.y += padding.y;
    for (i32 i = 0; i < tabs->label_cnt; i++) {
        pos.x += padding.x;
        __ui_pos(label, pos, tabs->_.zindex);
        pos.x += label->bounds.w + padding.x;
        label = __ui_next(label);
    }
    pos = tmp;
    FOR_SIBLINGS(tabs, label)
        __ui_pos(child, pos, tabs->_.zindex);
}

static void _zui_tabset_draw(zcmd_tabset *tabs) {
    zrect bounds = tabs->_.bounds;
    zvec2 padding = zui_stylev(ZW_TABSET, ZSV_PADDING);
    zcmd_widget *label = __ui_get_child(&tabs->_);
    FOR_N_SIBLINGS(tabs, label, tabs->label_cnt) {
        zrect tab_rect = __rect_pad(label->bounds, padding);
        if (__ui_clicked(ZM_LEFT_CLICK) && __vec_within(ctx->input.mouse_pos, tab_rect))
            *tabs->state = i;
    }
    label = __ui_get_child(&tabs->_);
    __push_rect_cmd((zrect) { bounds.x, bounds.y, bounds.w, tabs->tabheight }, zui_stylec(ZW_TABSET, ZSC_UNFOCUSED), tabs->_.zindex);
    FOR_N_SIBLINGS(tabs, label, tabs->label_cnt) {
        zrect tab_rect = __rect_pad(label->bounds, padding);
        if(i == *tabs->state)
            __push_rect_cmd(tab_rect, zui_stylec(ZW_TABSET, ZSC_BACKGROUND), tabs->_.zindex);
        else if(__vec_within(ctx->input.mouse_pos, tab_rect) && __ui_cont_hovered(&tabs->_))
            __push_rect_cmd(tab_rect, zui_stylec(ZW_TABSET, ZSC_HOVERED), tabs->_.zindex);
        __ui_draw(label);
    }
    __push_clip_cmd((zrect) { bounds.x, bounds.y + tabs->tabheight, bounds.w, bounds.h - tabs->tabheight }, tabs->_.zindex);
    // skip to selected sibling
    FOR_N_SIBLINGS(tabs, label, *tabs->state);
    __ui_draw(label);
}

void zui_init(zui_render_fn fn, zui_log_fn logger, void *user_data) {
    static zui_ctx global_ctx = { 0 };
    global_ctx.renderer = fn;
    global_ctx.log = logger;
    global_ctx.user_data = user_data;
    __buf_init(&global_ctx.draw, 256, sizeof(void*));
    __buf_init(&global_ctx.ui, 256, sizeof(void*));
    __buf_init(&global_ctx.registry, 256, sizeof(void*));
    __buf_init(&global_ctx.stack, 256, sizeof(i32));
    __buf_init(&global_ctx.zdeque, 256, sizeof(u64));
    __buf_init(&global_ctx.input.text, 256, sizeof(char));
    _zmap_init(&global_ctx.glyphs);
    _zmap_init(&global_ctx.style);
    global_ctx.padding = (zvec2) { 15, 15 };
    global_ctx.latest = 0;
    ctx = &global_ctx;
    zui_register(ZW_BLANK, __zui_blank_size, 0, __zui_blank_draw);
    zui_register(ZW_WINDOW, __zui_window_size, __zui_window_pos, __zui_box_draw);

    zui_register(ZW_BOX, __zui_box_size, __zui_box_pos, __zui_box_draw);
    zui_default_style(ZW_BOX, ZSC_BACKGROUND, (zcolor) { 50, 50, 50, 255 }, ZS_DONE);

    zui_register(ZW_POPUP, __zui_popup_size, __zui_popup_pos, __zui_popup_draw);
    zui_register(ZW_LABEL, __zui_label_size, 0, __zui_label_draw);
    zui_default_style(ZW_LABEL, ZSC_FOREGROUND, (zcolor) { 250, 250, 250, 255 }, ZS_DONE);

    zui_register(ZW_COL, __zui_layout_size, __zui_layout_pos, __zui_layout_draw);
    zui_register(ZW_ROW, __zui_layout_size, __zui_layout_pos, __zui_layout_draw);
    zui_register(ZW_BTN, __zui_box_size, __zui_box_pos, __zui_button_draw);
    zui_register(ZW_CHECK, __zui_check_size, 0, __zui_check_draw);
    zui_register(ZW_TEXT, __zui_text_size, 0, __zui_text_draw);
    zui_register(ZW_COMBO, __zui_combo_size, __zui_combo_pos, __zui_combo_draw);
    zui_register(ZW_GRID, __zui_grid_size, __zui_grid_pos, __zui_grid_draw);
    zui_default_style(ZW_GRID,
        ZSC_BACKGROUND, (zcolor) { 30, 30, 30, 255 },
        ZSC_FOREGROUND, (zcolor) { 150, 150, 150, 255 }, // border color
        ZSV_PADDING,    (zvec2)  { 15, 15 },
        ZSV_SPACING,    (zvec2)  { 15, 15 },
        ZSV_BORDER,     (zvec2)  { 3, 3 },
        ZS_DONE);

    zui_register(ZW_TABSET, _zui_tabset_size, _zui_tabset_pos, _zui_tabset_draw);
    zui_default_style(ZW_TABSET,
        ZSC_UNFOCUSED,  (zcolor) { 30, 30, 30, 255 },
        ZSC_BACKGROUND, (zcolor) { 50, 50, 50, 255 },
        ZSC_HOVERED,    (zcolor) { 40, 40, 40, 255 },
        ZSV_PADDING,    (zvec2)  { 15, 5 },
        ZS_DONE);
}

void zui_close() {
    free(ctx->draw.data);
    free(ctx->ui.data);
    free(ctx->registry.data);
    free(ctx->stack.data);
    free(ctx->zdeque.data);
    free(ctx->input.text.data);
    free(ctx->glyphs.data);
    ctx = 0;
}
