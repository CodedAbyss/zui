#include "zui.h"
#include <stdlib.h>
#include <string.h>

#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))

//#define assert(bool, msg) { if(!(bool)) printf(msg); exit(1); }
#define FOR_CHILDREN(ui) for(zw_base* child = _ui_get_child((zw_base*)ui); child != (zw_base*)ui; child = _ui_next(child))
#define FOR_SIBLINGS(ui, sibling) for(zw_base* child = (zw_base*)sibling; child != (zw_base*)ui; child = _ui_next(child))
#define FOR_N_SIBLINGS(ui, sibling, n) for(i32 i = 0; sibling != (zw_base*)ui && i < n; sibling = _ui_next(sibling), i++)
#define SWAP(type, a, b) { type tmp = a; a = b; b = tmp; }

// Represents a registry entry (defines functions for a widget-id)
typedef struct zui_type {
    u16  (*size)(void*, bool, u16);
    void (*pos)(void*, zvec2, i32);
    void (*draw)(void*);
} zui_type;
// Growable buffer (most often used as a stack)
typedef struct zui_buf {
    i32 used;
    u16 cap;
    u16 alignsub1;
    u8 *data;
} zui_buf;
// Initialize buffer
static void _buf_init(zui_buf *l, i32 cap, i32 alignment) {
    // get log2 of cap
    if (alignment & (alignment - 1))
        zui_log("ERROR: Buffer alignment must be a power of two");
    union { f32 f; i32 i; } tmp = { .f = (float)(cap - 1) };
    l->cap = (tmp.i >> 23) - 126;
    l->used = 0;
    l->alignsub1 = alignment - 1;
    l->data = malloc(cap);
}
// Allocate an aligned memory block on a given buffer
static void *_buf_alloc(zui_buf *l, i32 size) {
    size = (size + l->alignsub1) & ~l->alignsub1;
    if(l->used + size > (1 << l->cap)) {
        l->cap++;
        l->data = realloc(l->data, 1 << l->cap);
    }
    void *ret = l->data + l->used;
    l->used += size;
    return ret;
}
inline static i32 _buf_align(zui_buf *l, i32 n) {
    return (n + l->alignsub1) & ~l->alignsub1;
}
inline static void *_buf_peek(zui_buf *l, i32 size) {
    return l->data + l->used - ((size + l->alignsub1) & ~l->alignsub1);
}
inline static void *_buf_pop(zui_buf *l, i32 size) {
    l->used -= ((size + l->alignsub1) & ~l->alignsub1);
    return l->data + l->used;
}

typedef struct zstyle { u16 widget_id; u16 style_id; union { zcolor c; zvec2 v; i32 i; u32 u; f32 f; } value; } zstyle;

// Dead simple map implementation
// Our use case doesn't require deletions which simplifies logic quite a bit
typedef struct zmap {
    u32 cap;
    u32 used;
    u64 *data; // layout: <1 bit if slot occupied><31 bits of value><32 bits for key>
} zmap;
// Initialize map
static void _zmap_init(zmap *map) {
    map->used = 0;
    map->cap = 16;
    map->data = calloc(map->cap, sizeof(u64));
}
// Hash bits so we don't have to deal with collisions as much.
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
    //while (((i32)map->data[index] ^ (i32)key) > 0)
        index = (index + 1) % map->cap;
    return &map->data[index];
}
static bool _zmap_get(zmap *map, u32 key, u32 *value) {
    u64 *node = _zmap_node(map, key);
    *value = (*node >> 32);
    return (u32)*node != 0;
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

static u8 _utf8_masks[] = { 0, 0x7F, 0x1F, 0xF, 0x7 };
// returns the byte length of the first utf8 character in <text> and puts the unicode value in <codepoint>
i32 utf8_val(char *text, u32 *codepoint) {
    static u8 utf8len[] = { 1,1,1,1,1,1,1,1,0,0,0,0,2,2,3,4 };
    i32 len = utf8len[(u8)(*text) >> 4];
    *codepoint = text[0] & _utf8_masks[len];
    for (i32 i = 1; i < len; i++)
        *codepoint = (*codepoint << 6) | (text[i] & 0x3F);
    return len;
}
// returns the utf8 byte length of a given codepoint
i32 utf8_len(u32 codepoint) {
    if (codepoint < 0) return 0;
    if (codepoint < 0x80) return 1;
    if (codepoint < 0x800) return 2;
    if (codepoint < 0x10000) return 3;
    if (codepoint < 0x110000) return 4;
    return 0;
}
// fills text with the utf8 encoding of <codepoint> given its utf8 byte length <len>
void utf8_print(char *text, u32 codepoint, i32 len) {
    for (i32 i = len - 1; i > 0; codepoint >>= 6)
        text[i--] = 0xC0 | (codepoint & 0x3F);
    text[0] = codepoint & _utf8_masks[len];
}

// A zui_client is used as a client for server-side rendering.
// Window / input events are sent to the server, and draw calls sent back to the client
// The implementation is based on the <send> and <recv> functions.
// Check the examples folder on how to use a zui_client, or implement your own
// typedef struct zui_client {
//     zui_render_fn renderer;
//     zui_log_fn log;
//     void *user_data;
//     zvec2 mouse_pos;
//     zvec2 window_sz;
//     u16 mouse_state;
//     u16 keyboard_modifiers;
//     u16 font_cnt;
//     u16 ctx_state;
//     zui_buf commands;
//     zmap glyphs;
//     i32 response;
//
//     zui_client_fn send;
//     zscmd *active_cmd;
// } zui_client;

// static zui_client *client;

typedef struct zui_ctx {
    zui_render_fn renderer;
    zui_log_fn log;
    void *user_data;
    zvec2 mouse_pos;
    zvec2 window_sz;
    u16 mouse_state;
    u16 keyboard_modifiers;
    u16 font_cnt;
    u16 ctx_state;
    zui_buf ui;
    zmap glyphs;
    i32 response;

    zvec2 padding;
    u16 font_id;
    u32 flags;
    i32 latest;
    i32 style_edits;
    // We can combine registry / stack / draw, with a # to determine where to reset to
    zui_buf registry;   // lifetime: all the time. Changes are rare
    zui_buf cont_stack; // lifetime: tree creation
    zui_buf draw;       // lifetime: generating draw calls
    zui_buf zdeque;     // lifetime: generating draw calls
    zui_buf text;
    zmap style;
    i32 __focused; // used for calculating focused
    i32 focused;
    i32 hovered;
    zvec2 prev_mouse_pos;
    u16 prev_mouse_state;
    u16 prev_keyboard_modifiers;
} zui_ctx;

static zui_ctx *ctx = 0;


// initialize a zui_client. <send> defines how to send commands to the server. <recv> defines how to interpret render commands from the server
// void zui_client_init(zui_client_fn send, zui_render_fn recv, zui_log_fn log, void *user_data) {
//     static zui_client global_client = { 0 };
//     client = &global_client;
//     client->log = log;
//     client->send = send;
//     client->renderer = recv;
//     client->user_data = user_data;
//     _buf_init(&client->commands, 256, sizeof(void*));
//     _zmap_init(&client->glyphs);
// }

// void zui_client_respond(i32 value) {
//     client->response = value;
//     zscmd *cmd = client->active_cmd;
//     if (!cmd) return;
//     switch (cmd->base.id) {
//     case ZSCMD_GLYPH: {
//         zccmd_glyph glyph = {
//             .header = { ZCCMD_GLYPH, sizeof(zccmd_glyph) },
//             .c = { cmd->glyph.c.font_id, (u16)client->response, cmd->glyph.c.c }
//         };
//         client->send((zccmd*)&glyph, client->user_data);
//     } break;
//     }
// }

// Pushes a command onto the command stack (unless it should be processed immediately).
// void zui_client_push(zscmd *cmd) {
//     if (cmd->base.id >= ZSCMD_CLIP && cmd->base.id <= ZSCMD_TEXT) { // draw commands are pushed to the draw stack
//         if (client->ctx_state) { // only clear previous draw commands once we receive a new set.
//             client->ctx_state = false;
//             client->commands.used = 0;
//         }
//         memcpy(_buf_alloc(&client->commands, cmd->base.bytes), cmd, cmd->base.bytes);
//         return;
//     }
//     // non-draw commands are executed immediately
//     client->active_cmd = cmd;
//     client->renderer(cmd, client->user_data);
//     client->active_cmd = 0;
// }

// Push/process a buffer of packed commands onto the command stack.
// void zui_client_push_raw(char *bytes, i32 len) {
//     if (len < sizeof(zcmd)) return;
//     zscmd *cmd = (zscmd*)bytes;
//     if (cmd->base.bytes > len) return;
//     zui_client_push((zscmd*)cmd);
//     zui_client_push_raw(bytes + cmd->base.bytes, len - cmd->base.bytes);
// }

// Initiate a render on the client. This should be called when a ZSCMD_DRAW command is received.
// void zui_client_render() {
//     char *ptr = (char*)client->commands.data;
//     char *end = ptr + client->commands.used;
//     while (ptr < end) {
//         zscmd *cmd = (zscmd*)ptr;
//         client->renderer(cmd, client->user_data);
//         ptr += _buf_align(&client->commands, cmd->base.bytes);
//     }
//     client->ctx_state = true;
// }

// static void _zui_send_mouse_data() {
//     zccmd_mouse data = {
//         .header = { ZCCMD_MOUSE, sizeof(zccmd_mouse) },
//         .pos = client->mouse_pos,
//         .state = client->mouse_state
//     };
//     client->send((zccmd*)&data, client->user_data);
// }

// Logs using specified log function
void zui_log(char *fmt, ...) {
    if (!ctx) return;
    void *user_data = ctx->user_data;
    zui_log_fn fn = ctx->log;
    if (!fn) return;
    va_list args;
    va_start(args, fmt);
    fn(fmt, args, user_data);
    va_end(args);
}

// Returns the width and height of text given the font id [S]
u16 zui_text_axis(u16 font_id, char *text, i32 len, bool axis) {
    u32 codepoint;
    u32 sz = 0;
    if (axis) return _zmap_get(&ctx->glyphs, _zgc_hash(font_id, 0x1FFFFF), &sz) ? sz : 0;
    for (u32 bw, w = 0, i = 0; i < len; i += bw, sz += w) {
        bw = utf8_val(text + i, &codepoint);
        if (!bw) return 0;
        u32 key = _zgc_hash(font_id, codepoint);
        if (_zmap_get(&ctx->glyphs, key, &w)) {
            //zui_log("Char %c: %d\n", codepoint, w);
            continue;
        }
        // request renderer for glyph width
        zcmd_glyph cmd = {
            .header = { ZCMD_GLYPH_SZ, sizeof(zcmd_glyph) },
            .c = { font_id, 0, codepoint }    
        };
        ctx->renderer((zcmd_any*)&cmd, ctx->user_data);
        _zmap_set(&ctx->glyphs, key, cmd.c.width);
        w = cmd.c.width;
        //zui_log("Char %c: %d\n", codepoint, w);
    }
    return sz;
}
zvec2 zui_text_sz(u16 font_id, char *text, i32 len) {
    zvec2 ret;
    ret.x = zui_text_axis(font_id, text, len, 0);
    ret.y = zui_text_axis(font_id, text, len, 1);
    return ret;
}
// Report mouse button press
void zui_mouse_down(u16 btn) { ctx->mouse_state |= btn; }
// Report mouse button release
void zui_mouse_up(u16 btn) { ctx->mouse_state &= ~btn; }
// Report mouse move
void zui_mouse_move(zvec2 pos) { ctx->mouse_pos = pos; }
// Report key modifiers shift/alt/etc.
void zui_key_mods(u16 mod) { ctx->keyboard_modifiers = mod; }

// Report key press
void zui_key_char(i32 c) {
    i32 len = utf8_len(c);
    char *utf8 = (char*)_buf_alloc(&ctx->text, len);
    utf8_print(utf8, c, len);
    // if (!client) {
    //     return;
    // }
    // // send glyph info for all fonts if necessary
    // for (u16 id = 0; id < client->font_cnt; id++) {
    //     u32 value;
    //     if (_zmap_get(&client->glyphs, _zgc_hash(id, c), &value)) continue;
    //     zcmd_glyph glyph = {
    //         .header = { ZCMD_GLYPH_SZ, sizeof(zcmd_glyph) },
    //         .c = (zglyph_data) {
    //             .font_id = id,
    //             .c = c,
    //             .width = 0
    //         }
    //     };
    //     zui_client_push((zcmd*)&glyph);
    // }
    // zccmd_keys key = {
    //     .header = { ZCCMD_KEYS, sizeof(zccmd_keys) },
    //     .modifiers = client->keyboard_modifiers,
    //     .key = c
    // };
    // client->send((zccmd*)&key, client->user_data);
}
// Report window resize
void zui_resize(u16 width, u16 height) {
    ctx->window_sz = (zvec2) { width, height };
}
// Returns widget pointer given index
static inline zw_base *_ui_widget(i32 index) {
    return (zw_base*)(ctx->ui.data + index);
}
// Returns index given widget pointer
static inline i32 _ui_index(zw_base *ui) {
    return (i32)((u8*)ui - (u8*)ctx->ui.data);
}
// Returns next ui element in the container. If none left, returns parent
static inline zw_base *_ui_next(zw_base *widget) {
    return _ui_widget(widget->next);
}
// Allocates space on the top of the UI stack
// Sets the widgets' flags, bytes, and id as necessary
static void *_ui_alloc(i32 id, i32 size) {
    if (ctx->cont_stack.used) {
        i32 parent = *(i32*)_buf_peek(&ctx->cont_stack, sizeof(i32));
        ((zw_cont*)_ui_widget(parent))->children++;
    }
    zw_base *prev = _ui_widget(ctx->latest);
    ctx->latest = prev->next = ctx->ui.used;
    zw_base *widget = _buf_alloc(&ctx->ui, size);
    memset(widget, 0, size);
    widget->id = id;
    widget->bytes = size;
    widget->flags = ctx->flags;
    ctx->flags &= ~(ZF_FILL_X | ZF_FILL_Y);
    return widget;
}
// An extention to _ui_alloc for containers, setting the ZF_CONTAINER flag
// It pops all local style changes off the CONT stack and associates them with the container 
// It pushes the location of this container to the top of the CONT stack
// The widgets' byte count is adjusted to consider the style edits
static void *_cont_alloc(i32 id, i32 size) {
    i32 len = (i32)sizeof(zstyle) * ctx->style_edits;
    zstyle *edits =    _buf_pop(&ctx->cont_stack, len);
    zw_cont *cont = _ui_alloc(id, size);
    zstyle *new_edits = _buf_alloc(&ctx->ui, len);
    memcpy(new_edits, edits, len);
    *(i32*)_buf_alloc(&ctx->cont_stack, sizeof(i32)) = _ui_index((zw_base*)cont);
    cont->bytes += len;
    cont->style_edits = ctx->style_edits;
    cont->flags |= ZF_CONTAINER;
    ctx->style_edits = 0;
    return cont;
}
static void *_draw_alloc(u16 id, u16 size, i32 zindex) {
    u64 *index = _buf_alloc(&ctx->zdeque, sizeof(u64));
    zcmd *ret = (zcmd*)_buf_alloc(&ctx->draw, size);
    ret->id = id;
    ret->bytes = size;
    // high bytes represent z-index, low bits are index into the pointer
    // we can sort this deque as 64 bit integers: which will sort zindex first, then by insertion order
    *index = ((u64)zindex << 32) | ((u8*)ret - ctx->draw.data);
    return ret;
}
static zrect _rect_add(zrect a, zrect b) {
    return (zrect) { a.x + b.x, a.y + b.y, a.w + b.w, a.h + b.h };
}
// increase width/height by padding*2, offset x/y to remain centered
static zrect _rect_pad(zrect r, zvec2 padding) {
    return (zrect) { r.x - padding.x, r.y - padding.y, r.w + padding.x * 2, r.h + padding.y * 2 };
}
static void _push_rect_cmd(zrect rect, zcolor color, i32 zindex) {
    zcmd_rect *r = _draw_alloc(ZCMD_DRAW_RECT, sizeof(zcmd_rect), zindex);
    r->rect = rect;
    r->color = color;
}
static void _push_clip_cmd(zrect rect, i32 zindex) {
    zcmd_clip *r = _draw_alloc(ZCMD_DRAW_CLIP, sizeof(zcmd_clip), zindex);
    r->rect = rect;
}
static void _push_text_cmd(u16 font_id, zvec2 coord, zcolor color, char *text, i32 len, i32 zindex) {
    zcmd_text *r = _draw_alloc(ZCMD_DRAW_TEXT, sizeof(zcmd_text) + len, zindex);
    r->font_id = font_id;
    r->pos = coord;
    r->color = color;
    memcpy(r->text, text, len);
}

static zvec2 _vec_max(zvec2 a, zvec2 b) { return (zvec2) { max(a.x, b.x), max(a.y, b.y) }; }
static zvec2 _vec_min(zvec2 a, zvec2 b) { return (zvec2) { min(a.x, b.x), min(a.y, b.y) }; }
static zvec2 _vec_add(zvec2 a, zvec2 b) { return (zvec2) { a.x + b.x, a.y + b.y }; }

static bool _vec_within(zvec2 v, zrect bounds) {
    return (v.x >= bounds.x) && (v.x <= bounds.x + bounds.w) && (v.y >= bounds.y) && (v.y <= bounds.y + bounds.h);
}
static bool _rect_within(zrect r, zrect bounds) {
    return (r.x >= bounds.x) && (r.x + r.w <= bounds.x + bounds.w) && (r.y >= bounds.y) && (r.y + r.h <= bounds.y + bounds.h);
}
static bool _rect_intersect(zrect a, zrect b, zrect *intersect) {
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
static void _rect_justify(zrect *used, zrect bounds, i32 justification) {
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
void _ui_schedule_focus(zw_base *widget) {
    if (!widget) return;
    ctx->focused = 0;
    ctx->__focused = _ui_index(widget);
}
zw_base *_ui_find_with_flag(zw_base *start, u32 flags) {
    i32 si = _ui_index(start);
    for(i32 index = si + start->bytes; index < ctx->ui.used; index += _ui_widget(index)->bytes) {
        zw_base *w = _ui_widget(index);
        if (w->flags & flags)
            return w;
    }
    for (i32 index = 0; index < si; index += _ui_widget(index)->bytes) {
        zw_base *w = _ui_widget(index);
        if (w->flags & flags)
            return w;
    }
    return 0;
}
zw_base *_ui_get_child(zw_base *ui) {
    if (~ui->flags & ZF_PARENT) return ui;
    return (zw_base*)((u8*)ui + ((ui->bytes + ctx->ui.alignsub1) & ~ctx->ui.alignsub1));
}
bool _ui_pressed(i32 buttons) {
    if(ctx->mouse_state & buttons)
        return true;
    return false;
}
bool _ui_dragged(i32 buttons) {
    if(ctx->mouse_state & ctx->prev_mouse_state & buttons)
        return true;
    return false;
}
bool _ui_clicked(i32 buttons) {
    if(ctx->mouse_state & (ctx->mouse_state ^ ctx->prev_mouse_state) & buttons)
        return true;
    return false;
}

void _ui_print(zw_base *cmd, int indent) {
    zui_log("%04x | ", _ui_index(cmd));
    for (i32 i = 0; i < indent; i++)
        zui_log("    ");    
    zrect b = cmd->bounds;
    zrect u = cmd->used;
    zui_log("(id: %d, next: %04x, bounds: {%d,%d,%d,%d}, used: {%d,%d,%d,%d})\n", cmd->id, cmd->next, b.x, b.y, b.w, b.h, u.x, u.y, u.w, u.h);
    FOR_CHILDREN(cmd)
        _ui_print(child, indent + 1);
}

u16 _ui_sz(zw_base *ui, bool axis, u16 bound) {
    zui_type type = ((zui_type*)ctx->registry.data)[ui->id - ZW_FIRST];
    u16 sz = type.size(ui, axis, bound);
    ui->used.sz[axis] = sz;
    ui->bounds.sz[axis] = bound == Z_AUTO ? ui->used.sz[axis] : bound;
    return ui->bounds.sz[axis];
}
bool _ui_is_child(zw_base *container, zw_base *other) {
    if(other < container) return false; // children must have a greater index (ptr)
    if(other == container) return true; // we consider a ui to be a child of itself (useful for hovering / focusing)
    i32 index = _ui_index(other);
    if(_ui_index(container) < index && index < container->next) // if index of other is between the container and its sibling, it's a child
        return true;
    // TODO: this can likely be improved.
    // Worst case: when container->next is its parent __ui_is_child checks all children
    // loop through children, if any of the children have a greater index than other, other is a sub-child
    FOR_CHILDREN(container)
        if(child >= other)
            return true;
    return false;
}
static inline bool _ui_hovered(zw_base *ui) { return ui == _ui_widget(ctx->hovered); }
static inline bool _ui_cont_hovered(zw_base *ui) { return _ui_is_child(ui, _ui_widget(ctx->hovered)); }
static inline bool _ui_focused(zw_base *ui) { return ui == _ui_widget(ctx->focused); }
static inline bool _ui_cont_focused(zw_base *ui) { return _ui_is_child(ui, _ui_widget(ctx->focused)); }

void _ui_pos(zw_base *ui, zvec2 pos, i32 zindex) {
    zui_type type = ((zui_type*)ctx->registry.data)[ui->id - ZW_FIRST];
    ui->bounds.x = pos.x;
    ui->bounds.y = pos.y;
    ui->zindex = zindex;
    if (ui->flags & ZF_FILL_X) ui->used.w = ui->bounds.w;
    if (ui->flags & ZF_FILL_Y) ui->used.h = ui->bounds.h;
    _rect_justify(&ui->used, ui->bounds, ui->flags);
    if ((ui->flags & ZF_DISABLED) && (ui->flags & ZF_PARENT))
        FOR_CHILDREN(ui) child->flags |= ZF_DISABLED; // all children of a disabled element must also be disabled
    if((~ui->flags & ZF_DISABLED) && _vec_within(ctx->mouse_pos, ui->used)) {
        if (zindex >= _ui_widget(ctx->hovered)->zindex)
            ctx->hovered = _ui_index(ui);
        if(zindex >= _ui_widget(ctx->__focused)->zindex && _ui_clicked(ZM_LEFT_CLICK))
            ctx->__focused = _ui_index(ui);
    }
    if(type.pos) // pos functions are only needed for containers to position children
        type.pos(ui, *(zvec2*)&ui->used, zindex);
}

void _ui_draw(zw_base *ui) {
    zui_type type = ((zui_type*)ctx->registry.data)[ui->id - ZW_FIRST];
    zw_cont *c;
    zstyle *edits;
    // apply style changes for this container and it's children
    if (ui->flags & ZF_CONTAINER) {
        c = (zw_cont*)ui;
        edits = (zstyle*)((u8*)ui + _buf_align(&ctx->ui, ui->bytes - c->style_edits * sizeof(zstyle)));
        for (i32 i = 0; i < c->style_edits; i++) {
            u32 value, key = _zs_hash(edits[i].widget_id, edits[i].style_id);
            if (!_zmap_get(&ctx->style, key, &value))
                zui_log("WARNING: No default style for widget-id:%d,style-id:%d\n", edits[i].widget_id, edits[i].style_id);
            _zmap_set(&ctx->style, key, edits[i].value.u);
            edits[i].value.u = value; // save previous value
        }
    }
    type.draw(ui);
    if (~ui->flags & ZF_CONTAINER) return;
    // restore original style
    for (i32 i = 0; i < c->style_edits; i++)
        _zmap_set(&ctx->style, _zs_hash(edits[i].widget_id, edits[i].style_id), edits[i].value.u);
}

static zvec2 _ui_sz_ofs(zvec2 bounds, zvec2 ofs) {
    zvec2 ret = { 0, 0 };
    if(bounds.x == Z_AUTO) ret.x = Z_AUTO;
    else ret.x = max(bounds.x + ofs.x, 0);
    if(bounds.y == Z_AUTO) ret.y = Z_AUTO;
    else ret.y = max(bounds.y + ofs.y, 0);
    return ret;
}

static zvec2 _ui_sz_auto(zvec2 bounds, zvec2 auto_sz) {
    zvec2 ret = { 0, 0 };
    if(bounds.x == Z_AUTO) ret.x = auto_sz.x;
    else ret.x = bounds.x;
    if(bounds.y == Z_AUTO) ret.y = auto_sz.y;
    else ret.y = bounds.y;
    return ret;
}

void zui_print_tree() {
    zui_log("printing tree...\n");
    _ui_print(_ui_widget(0), 0);
}

void zui_register(i32 widget_id, void *size_cb, void *pos_cb, void *draw_cb) {
    ctx->registry.used = (widget_id - ZW_FIRST) * sizeof(zui_type);
    zui_type *t = _buf_alloc(&ctx->registry, sizeof(zui_type));
    t->size = (u16(*)(void*, bool, u16))size_cb;
    t->pos = (void(*)(void*, zvec2, i32))pos_cb;
    t->draw = (void(*)(void*))draw_cb;
}

void zui_justify(u32 justification) {
    ctx->flags = justification & 15;
}

void zui_fill(u32 axis) {
    ctx->flags |= axis & (ZF_FILL_X | ZF_FILL_Y);
}

u16 zui_new_font(char *family, i32 size) {
    _zmap_set(&ctx->glyphs, _zgc_hash(ctx->font_cnt, 0x1FFFFF), size);
    i32 bytes = sizeof(zcmd_reg_font) + strlen(family);
    zcmd_reg_font *font = _alloca(bytes);
    font->header = (zcmd) { ZCMD_REG_FONT, bytes };
    font->font_id = ctx->font_cnt;
    font->size = size;
    memcpy(font->family, family, bytes - sizeof(zcmd_reg_font));
    ctx->renderer((zcmd_any*)font, ctx->user_data);
    return ctx->font_cnt++;
}

// set zui font
void zui_font(u16 font_id) {
    ctx->font_id = font_id;
}

void _zui_qsort(u64 *nums, i32 count) {
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
    _zui_qsort(nums, j++);
    _zui_qsort(nums + j, count - j);
}

// move all values with either of the high bits set to the end of the array
static i32 _zui_partition(u64 *values, i32 len) {
    i32 i = 0, j = len - 1;
    while (1) {
        while (!(values[i] >> 63) && i < j) i++;
        while ((values[j] >> 63) && i <= j) j--;
        if (j <= i) break;
        SWAP(u64, values[i], values[j]);
    }
    return j;
}

// ends any container (window / grid)
void zui_end() {
    i32 container = *(i32*)_buf_pop(&ctx->cont_stack, sizeof(i32) + sizeof(zstyle) * ctx->style_edits);
    // if active container isn't the most recent element set parent flag (it has children)
    zw_base* latest = _ui_widget(ctx->latest);
    if (container != ctx->latest) {
        _ui_widget(container)->flags |= ZF_PARENT;
        ctx->latest = latest->next = container;
    }
}

void zui_style(u32 widget_id, ...) {
    va_list args;
    va_start(args, widget_id);
    for (u32 style_id; (style_id = va_arg(args, u32)) != ZS_DONE; ) {
        zstyle *style = (zstyle*)_buf_alloc(&ctx->cont_stack, sizeof(zstyle));
        *style = (zstyle) { .widget_id = widget_id, .style_id = style_id, .value.u = va_arg(args, u32) };
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
        zui_log("No style exists for widget:%d,style:%d\n", widget_id, style_id);
}
zcolor zui_stylec(u16 widget_id, u16 style_id) { zcolor ret; _zui_get_style(widget_id, style_id, &ret); return ret; }
zvec2  zui_stylev(u16 widget_id, u16 style_id) { zvec2  ret; _zui_get_style(widget_id, style_id, &ret); return ret; }
float  zui_stylef(u16 widget_id, u16 style_id) { float  ret; _zui_get_style(widget_id, style_id, &ret); return ret; }
i32    zui_stylei(u16 widget_id, u16 style_id) { i32    ret; _zui_get_style(widget_id, style_id, &ret); return ret; }

void zui_render() {
    if (ctx->cont_stack.used != 0 || ctx->window_sz.x == 0 || ctx->window_sz.y == 0)
        return;

    // calculate sizes
    zw_base *root = _ui_widget(0);
    root->next = 0;
    _ui_sz(root, 0, ctx->window_sz.x);
    _ui_sz(root, 1, ctx->window_sz.y);

    // calculate positions
    ctx->hovered = 0;
    root->bounds.x = 0;
    root->bounds.y = 0;
    _ui_pos(root, (zvec2) { 0, 0 }, 0);
    if (ctx->__focused) {
        ctx->focused = ctx->__focused;
        ctx->__focused = 0;
    }

    // generate draw commands
    _ui_draw(root);

    // sort draw commands by zindex / index (order of creation)
    // despite qsort not being a stable sort, the order of draw cmd creation is preserved due to index being part of each u64
    u64 *deque_reader = (u64*)ctx->zdeque.data;
    _zui_qsort(deque_reader, ctx->zdeque.used / sizeof(u64));
    zcmd_any begin = { .base = { ZCMD_RENDER_BEGIN, sizeof(zcmd) } };
    ctx->renderer(&begin, ctx->user_data);
    while(deque_reader < (u64*)(ctx->zdeque.data + ctx->zdeque.used)) {
        u64 next_pair = *deque_reader++;
        i32 index = next_pair & 0x7FFFFFFF;
        zcmd_any *next = (zcmd_any*)(ctx->draw.data + index);
        ctx->renderer(next, ctx->user_data); 
    }
    zcmd_any end = { .base = { ZCMD_RENDER_END, sizeof(zcmd) } };
    ctx->renderer(&end, ctx->user_data);
    ctx->prev_mouse_pos = ctx->mouse_pos;
    ctx->prev_mouse_state = ctx->mouse_state;
    ctx->text.used = 0;
    ctx->zdeque.used = 0;
    ctx->draw.used = 0;
    ctx->ui.used = 0;
}

// void zui_push(zccmd *cmd) {
//     switch (cmd->base.id) {
//     case ZCCMD_MOUSE:
//         ctx->mouse_pos = cmd->mouse.pos;
//         ctx->mouse_state = cmd->mouse.state;
//         break;
//     case ZCCMD_KEYS: {
//         i32 len = _utf8_len(cmd->keys.key);
//         char *utf8 = (char*)_buf_alloc(&ctx->text, len);
//         _utf8_print(utf8, cmd->keys.key, len);
//     } break;
//     case ZCCMD_GLYPH:
//         _zmap_set(&ctx->glyphs, _zgc_hash(cmd->glyph.c.font_id, cmd->glyph.c.c), cmd->glyph.c.width);
//         break;
//     case ZCCMD_WIN:
//         ctx->window_sz = cmd->win.sz;
//         break;
//     }
// }

void zui_blank() {
    _ui_alloc(ZW_BLANK, sizeof(zw_base));
}
static zvec2 __zui_blank_size(zw_base *w, zvec2 bounds) { return bounds; }
static void __zui_blank_draw(zw_base *w) {}

// returns true if window is displayed
void zui_box() {
    _cont_alloc(ZW_BOX, sizeof(zw_box));
}
static u16 __zui_box_size(zw_box *box, bool axis, u16 bound) {
    if (bound != Z_AUTO) bound -= ctx->padding.e[axis] * 2;
    u16 auto_sz = 0;
    FOR_CHILDREN(box)
        auto_sz = max(auto_sz, _ui_sz(child, axis, bound));
    return (bound == Z_AUTO ? auto_sz : bound) + ctx->padding.e[axis] * 2;
}
static void __zui_box_pos(zw_box *box, zvec2 pos, i32 zindex) {
    zvec2 child_pos = { pos.x + ctx->padding.x, pos.y + ctx->padding.y };
    FOR_CHILDREN(box) _ui_pos(child, child_pos, zindex);
}
static void __zui_box_draw(zw_box *box) {
    _push_rect_cmd(box->widget.bounds, zui_stylec(ZW_BOX, ZSC_BACKGROUND), box->widget.zindex);
    FOR_CHILDREN(box) _ui_draw(child);
}

void zui_window() {
    ctx->ui.used = 0;
    ctx->flags = 0;
    _cont_alloc(ZW_WINDOW, sizeof(zw_box));
}
static u16 __zui_window_size(zw_box *window, bool axis, u16 bound) {
    FOR_CHILDREN(window) _ui_sz(child, axis, bound);
    return bound;
}
static void __zui_window_pos(zw_box *window, zvec2 pos) {
    FOR_CHILDREN(window) _ui_pos(child, pos, 1);
}

zvec2 zui_window_sz() {
    return ctx->window_sz;
}

// LABEL
void zui_label_n(char *text, i32 len) {
    zw_label *l = _ui_alloc(ZW_LABEL, sizeof(zw_label));
    l->text = text;
    l->len = len;
}

void zui_label(const char *text) {
    zui_label_n((char*)text, (i32)strlen(text));
}

static u16 _zui_label_size(zw_label *data, bool axis, u16 bound) {
    u16 tmp = zui_text_axis(ctx->font_id, data->text, data->len, axis);
    return tmp;
}

static void _zui_label_draw(zw_label *data) {
    zvec2 coords = { data->widget.used.x, data->widget.used.y };
    _push_text_cmd(ctx->font_id, coords, zui_stylec(ZW_LABEL, ZSC_FOREGROUND), data->text, data->len, data->widget.zindex);
}

// BUTTON
bool zui_button(const char *text, u8 *state) {
    zw_btn *l = _cont_alloc(ZW_BTN, sizeof(zw_btn));
    l->state = state;
    zui_label(text);
    zui_end();
    return *state;
}

// A button is just a clickable box
// This allows trivial addition of various button kinds: with images, multiple labels, etc.
// Reuse __zui_box_size for sizing

static void _zui_button_draw(zw_btn *btn) {
    zcolor c = (zcolor) { 80, 80, 80, 255 };
    if(_ui_cont_hovered(&btn->widget)) {// hovered
        *btn->state = _ui_clicked(ZM_LEFT_CLICK);
        if(_ui_pressed(ZM_LEFT_CLICK))
            c = (zcolor) { 120, 120, 120, 255 };
        else
            c = (zcolor) { 100, 100, 100, 255 };
    }
    _push_rect_cmd(btn->widget.used, c, btn->widget.zindex);
    FOR_CHILDREN(btn)
        _ui_draw(child);
}

bool zui_check(u8 *state) {
    zw_check *c = _ui_alloc(ZW_CHECK, sizeof(zw_check));
    c->state = state;
    return *state;
}

static u16 _zui_check_size(zw_check *data, bool axis, zvec2 bound) {
    return zui_text_axis(ctx->font_id, "\xE2\x88\x9A", 3, axis) + 2;
}

static void _zui_check_draw(zw_check *data) {
    zcolor on =  (zcolor) { 60,  90,  250, 255 };
    zcolor off = (zcolor) { 200, 200, 200, 255 };
    if (_ui_hovered(&data->widget)) {// hovered
        *data->state ^= _ui_clicked(ZM_LEFT_CLICK);
        on  = (zcolor) {  80, 110, 250, 255 };
        off = (zcolor) { 230, 230, 230, 255 };
    }
    zvec2 sz = zui_text_sz(ctx->font_id, "\xE2\x88\x9A", 3);
    zrect r = data->widget.used;
    if (*data->state) {
        _push_rect_cmd(r, on, data->widget.zindex);
        r = _rect_add(r, (zrect) { 1, 1, -2, -2 });
        _push_text_cmd(ctx->font_id, (zvec2) { r.x + (r.w - sz.x) / 2, r.y }, (zcolor) { 250, 250, 250, 255 }, "\xE2\x88\x9A", 3, data->widget.zindex);
    }
    else {
        _push_rect_cmd(r, off, data->widget.zindex);
    }
}

// create a slider with a formatted tooltip which accepts *value
void zui_sliderf(char *tooltip, f32 min, f32 max, f32 *value);
void zui_slideri(char *tooltip, i32 min, i32 max, i32 *value);

char *_zui_combo_get_option(zw_combo *c, i32 n, i32 *len) {
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
    zw_combo *c = _cont_alloc(ZW_COMBO, sizeof(zw_combo));
    c->tooltip = tooltip;
    c->csoptions = csoptions;
    c->state = state;
    i32 len, i = 0;
    char *str;
    zui_blank();
    while ((str = _zui_combo_get_option(c, i++, &len))) {
        zui_box();
            zui_label_n(str, len);
        zui_end();
    }
    zui_end();
    return (*state >> 1) - 1;
}
static u16 _zui_combo_size(zw_combo *data, bool axis, u16 bound) {
    //zvec2 auto_sz = zui_text_sz(ctx->font_id, data->tooltip, (i32)strlen(data->tooltip));
    //zvec2 back_sz = (zvec2) { 0, 0 };
    //bounds.y = Z_AUTO;
    //zcmd_widget *background = 0, *child = 0;
    //if (*data->state & 1) {
    //    background = _ui_get_child(&data->widget);
    //    FOR_SIBLINGS(data, _ui_next(background)) {
    //        zvec2 sz = _ui_sz(child, bounds);
    //        back_sz.x = auto_sz.x = max(auto_sz.x, sz.x);
    //        back_sz.y += sz.y;
    //    }
    //}

    //auto_sz.x += 10;
    //auto_sz.y += 10;
    //if(bounds.x != Z_AUTO)
    //    auto_sz.x = bounds.x;
    //back_sz.x = auto_sz.x;
    //if (*data->state & 1)
    //    _ui_sz(background, back_sz);
    //return auto_sz;
    return 0;
}
static void _zui_combo_pos(zw_combo *data, zvec2 pos, i32 zindex) {
    if (~*data->state & 1) return;
    pos.y += data->widget.used.h;
    zw_base *background = _ui_get_child(&data->widget);
    i32 prev = ctx->flags;
    ctx->flags = ZJ_LEFT;
    zindex += 1;
    _ui_pos(background, pos, zindex);
    FOR_SIBLINGS(data, _ui_next(background)) {
        _ui_pos(child, pos, zindex);
        pos.y += child->used.h;
    }
    ctx->flags = prev;
}
static void _zui_combo_draw(zw_combo *box) {
    bool is_focused = &box->widget == _ui_widget(ctx->focused);
    i32 selected_index = (*box->state >> 1) - 1;
    _push_rect_cmd(box->widget.used, (zcolor) { 70, 70, 70, 255 }, box->widget.zindex);

    i32 len;
    char *text = _zui_combo_get_option(box, selected_index, &len);
    zvec2 pos = { box->widget.used.x + ctx->padding.x, box->widget.used.y + ctx->padding.y };
    _push_text_cmd(ctx->font_id, pos, (zcolor) { 230, 230, 230, 255 }, text, len, box->widget.zindex);

    // this detects whether our current widget (not any children) are focused
    if(&box->widget == _ui_widget(ctx->focused) && _ui_clicked(ZM_LEFT_CLICK)) {
        *box->state ^= 1;
        return;
    }
    if(~*box->state & 1) return;
    zw_base *background = _ui_get_child(&box->widget);
    _push_clip_cmd(background->used, background->zindex);
    _push_rect_cmd(background->used, (zcolor) { 80, 80, 80, 255 }, background->zindex);
    i32 i = 0;
    FOR_SIBLINGS(box, _ui_next(background)) {
        _push_clip_cmd(background->used, child->zindex);
        if(_ui_cont_focused(child) && _ui_clicked(ZM_LEFT_CLICK)) {
            *box->state = ((i + 1) << 1); // close popup and set selected
        } else if(_ui_cont_hovered(child)) {
            _push_rect_cmd(child->used, (zcolor) { 120, 120, 120, 255 }, child->zindex);
        } else if(i == selected_index) {
            _push_rect_cmd(child->used, (zcolor) { 100, 100, 100, 255 }, child->zindex);
        }
        _ui_draw(_ui_get_child(child));
        i++;
    }
}

// sets the text validator for text inputs
void zui_validator(bool (*validator)(char *text));

// creates a single-line text input
void zui_text(char *buffer, i32 len, zd_text *state) {
    zw_text *l = _ui_alloc(ZW_TEXT, sizeof(zw_text));
    l->widget.flags |= ZF_TABBABLE;
    l->buffer = buffer;
    l->len = len;
    l->state = state;
}

static u16 _zui_text_size(zw_text *data, bool axis, u16 bound) {
    if (!axis && bound != Z_AUTO) return bound;
    return zui_text_axis(ctx->font_id, data->buffer, (i32)strlen(data->buffer), axis) + 10;
}

static i32 _zui_text_get_index(zw_text *data, i32 len) {
    zvec2 mp = ctx->mouse_pos;
    bool found = false;
    for (i32 i = 0; i < len; i++) {
        zvec2 sz = zui_text_sz(ctx->font_id, data->buffer, i + 1);
        sz.x += 5 - data->state->ofs;
        //zrect r = { data->_.used.x, data->_.used.y, sz.x + 5, sz.y + 10 };
        if (mp.x >= data->widget.used.x && mp.x <= data->widget.used.x + sz.x)
            return i;
    }
    if (!found && _vec_within(mp, data->widget.used))
        return len;
    return -1;
}

static void _zui_text_draw(zw_text *data) {
    zd_text tctx = *(zd_text*)data->state;
    i32 len = (i32)strlen(data->buffer);
    if(&data->widget == _ui_widget(ctx->focused)) {
        i32 start = 0;
        // when there is a selection, the first input often has special behavior as the selection is removed
        if (ctx->text.used && tctx.selection) {
            char c = *(char*)ctx->text.data;
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
        for(i32 i = start; i < ctx->text.used; i++) {
            char c = ((char*)ctx->text.data)[i];
            if (c == 9) { // on tab, switch focus to next tabbable element
                zw_base *w = _ui_find_with_flag(&data->widget, ZF_TABBABLE);
                _ui_schedule_focus(w);
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
    zvec2 textpos = { data->widget.used.x + 5 - tctx.ofs, data->widget.used.y + 5 };

    // handle mouse selection
    if (&data->widget == _ui_widget(ctx->focused)) {
        //if (ctx->ctrl_a) {
        //    tctx.index = 0;
        //    tctx.selection = len;
        //}
        if (_ui_clicked(ZM_LEFT_CLICK)) {
            i32 index = _zui_text_get_index(data, len);
            tctx.selection = 0;
            tctx.flags = 0;
            if (index >= 0)
                tctx.index = index;
        }
        else if (_ui_dragged(ZM_LEFT_CLICK)) {
            i32 index = _zui_text_get_index(data, len);
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
    if(textpos.x + sz.x + 1 > data->widget.used.x + data->widget.used.w - 5) {
        i32 diff = textpos.x + sz.x + 6 - data->widget.used.x - data->widget.used.w;
        tctx.ofs += diff;
        textpos.x -= diff;
    }
    else if(textpos.x + sz.x < data->widget.used.x + 5) {
        i32 diff = textpos.x + sz.x - data->widget.used.x - 5;
        tctx.ofs += diff;
        textpos.x -= diff;
    }

    zrect cursor = { textpos.x + sz.x, textpos.y, 1, sz.y };

    // generate draw calls
    _push_rect_cmd(data->widget.used, (zcolor) { 30, 30, 30, 255 }, data->widget.zindex);

    zvec2 selection = zui_text_sz(ctx->font_id, data->buffer + tctx.index, tctx.selection);
    zrect r = { textpos.x + sz.x, textpos.y, selection.x, selection.y };
    _push_rect_cmd(r, (zcolor) { 60, 60, 200, 255 }, data->widget.zindex);

    _push_text_cmd(ctx->font_id, textpos, (zcolor) { 250, 250, 250, 255 }, data->buffer, len, data->widget.zindex);

    if(_ui_widget(ctx->focused) == &data->widget) // draw cursor
        _push_rect_cmd(cursor, (zcolor) { 200, 200, 200, 255 }, data->widget.zindex);

    *data->state = tctx;
}

// creates a multi-line text input
void zui_textbox(char *buffer, i32 len, i32 *state);

static void _zui_layout(i32 id, i32 n, float *sizes) {
    i32 bytes = sizeof(zw_layout) + (n - 1) * sizeof(float);
    zw_layout *l = _cont_alloc(id, bytes);
    l->count = n;
    if(sizes)
        memcpy(l->data, sizes, n * sizeof(float));
    else {
        for(i32 i = 0; i < n; i++)
            l->data[i] = Z_AUTO;
    }
}

void zui_col(i32 n, float *heights) {
    _zui_layout(ZW_COL, n, heights);
}

void zui_row(i32 n, float *heights) {
    _zui_layout(ZW_ROW, n, heights);
}

static void _zui_layout_draw(zw_layout *data) {
    FOR_CHILDREN(data)
        _ui_draw(child);
}

static u16 _zui_layout_size(zw_layout *data, bool axis, u16 bound) {
    //// minimum size of empty container is 0, 0
    //if(~data->widget.flags & ZF_PARENT)
    //    return (zvec2) { 0, 0 };

    //// We share logic between rows and columns by having an AXIS variable
    //// AXIS is x for ZW_ROW, y for ZW_COL
    //bool AXIS = data->widget.id - ZW_ROW;
    //i32 i = 0, j = 0, end = data->count;
    //i32 major_bound = bounds.e[AXIS];
    //i32 minor_bound = bounds.e[!AXIS];
    //i32 major = ctx->padding.e[AXIS] * (end - 1);
    //i32 minor = 0; 

    //u64 *children = _alloca(sizeof(u64) * data->count);
    //FOR_CHILDREN(data) children[j] = ((u64)(data->data[j] < 0) << 63) | ((u64)j << 32) | _ui_index(child);
 
    //// move percentages to end of list (calculate them last)
    //j = _zui_partition(children, data->count);
    //zvec2 child_bounds;
    //i32 pixels_left;
    //child_bounds.e[!AXIS] = minor_bound;
    //for(i = 0; i < end; i++) {
    //    float f = data->data[(u16)(children[i] >> 32)];
    //    i32 bound = f < 0 ? (i32)(pixels_left * -f + 0.5f) : (i32)f;
    //    child_bounds.e[AXIS] = bound;
    //    zvec2 child_sz = _ui_sz(_ui_widget((i32)children[i]), child_bounds);
    //    minor = max(minor, child_sz.e[!AXIS]);
    //    major += bound == Z_AUTO ? child_sz.e[AXIS] : bound;
    //    if (i == j) pixels_left = major_bound - major;
    //}
    //FOR_CHILDREN(data) child->bounds.sz[!AXIS] = minor;
    //bounds.e[AXIS] = major;
    //bounds.e[!AXIS] = minor;
    //return bounds;
    return 0;
}

static void _zui_layout_pos(zw_layout *data, zvec2 pos, i32 zindex) {
    FOR_CHILDREN(data) {
        _ui_pos(child, pos, zindex);
        if(data->widget.id == ZW_COL)
            pos.y += ctx->padding.y + child->bounds.h;
        else
            pos.x += ctx->padding.x + child->bounds.w;
    }
}
void zui_grid(i32 cols, i32 rows, float *col_row_settings) {
    i32 bytes = sizeof(zw_grid) + (rows + cols - 1) * sizeof(float);
    zw_grid *l = _cont_alloc(ZW_GRID, bytes);
    l->rows = rows;
    l->cols = cols;
    if(col_row_settings)
        memcpy(l->data, col_row_settings, (rows + cols) * sizeof(float));
    else {
        for(i32 i = 0; i < (rows + cols); i++)
            l->data[i] = Z_AUTO;
    }
}

static u16 _zui_grid_size(zw_grid *grid, bool axis, u16 bound) {
    u16 major_cnt = axis ? grid->rows : grid->cols;
    u16 minor_cnt = axis ? grid->cols : grid->rows;
    u16 sz = ctx->padding.e[axis] * (major_cnt - 1);
    typedef union {
        u64 u;
        struct { i32 index; u32 n : 30; u32 percent : 1; u32 fill : 1; };
    } flags;
    flags *children = _alloca(sizeof(flags) * grid->rows * grid->cols);
    i32 n = 0;
    FOR_CHILDREN(grid) {
        zvec2 pos = { n % grid->rows, n / grid->rows + grid->rows };
        children[n] = (flags) {
            .fill    = (child->flags & (ZF_FILL_X << axis)) != 0,
            .percent = grid->data[pos.e[axis]] < 0,
            .n       = n,
            .index   = _ui_index(child),
        };
        n++;
    }
    _zui_qsort((u64*)children, grid->rows * grid->cols);
    if (n != grid->rows * grid->cols)
        zui_log("ERR: grid has wrong # of children\r\n");
    u16 *sizes = _alloca(sizeof(u16) * (grid->rows + grid->cols));
    memset(sizes, 0, sizeof(u16) * (grid->rows + grid->cols));
    u16 non_percent = 0;
    for (i32 i = 0; i < n; i++) {
        i32 child = children[i].n;
        u16 pos = axis ? (child / grid->rows + grid->rows) : (child % grid->rows);
        zw_base *widget = _ui_widget(children[i].index);
        if (children[i].fill) {
            _ui_sz(widget, axis, sizes[pos]);
            continue;
        }
        float f = grid->data[pos];
        u16 child_bound = f < 0 ? (non_percent - bound) * f : f;
        u16 child_sz = _ui_sz(widget, axis, child_bound);
        u16 ofs = max(sizes[pos], child_sz) - sizes[pos];
        if (f >= 0) non_percent += ofs;
        sizes[pos] += ofs;
        sz += child_sz;
    }
    for (i32 i = 0; i < n; i++) {
        u16 pos = axis ? (children[i].n / grid->rows + grid->rows) : (children[i].n % grid->rows);
        if ((u16)grid->data[pos] == Z_AUTO) _ui_widget(children[i].index)->bounds.sz[axis] = sizes[pos];
    }

    /*
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
        children[n] = ((u64)(grid->data[x] < 0 || grid->data[y] < 0) << 63) | ((u64)n << 32) | _ui_index(child);
        n++;
    }
    if (n != grid->rows * grid->cols)
        _log("ERR: grid has wrong # of children\r\n");

    // high bit represents if the element is % sized, all percentage sized elements are calculated last
    zvec2 non_percent = used;
    zvec2 child_bounds;
    _zui_partition(children, n);
    for (i32 i = 0; i < n; i++) {
        i32 child = (children[i] >> 32) & 0x7FFFFFFF;
        i32 x = child % grid->cols, y = child / grid->cols + grid->cols;
        float w = grid->data[x], h = grid->data[y];
        child_bounds.x = w >= 0 ? w : (bounds.x - non_percent.x) * w;
        child_bounds.y = h >= 0 ? h : (bounds.y - non_percent.y) * h;
        zcmd_widget *widget = _ui_widget((u32)children[i]);
        zvec2 sz = _ui_sz(widget, child_bounds);
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
        if ((u16)grid->data[x] == Z_AUTO) _ui_widget((u32)children[i])->bounds.w = sizes[x];
        if ((u16)grid->data[y] == Z_AUTO) _ui_widget((u32)children[i])->bounds.h = sizes[y];
    }*/

    return sz;
}

static void _zui_grid_pos(zw_grid *grid, zvec2 pos, i32 zindex) {
    i32 n = 0;
    u16 x = pos.x;
    FOR_CHILDREN(grid) {
        _ui_pos(child, pos, zindex);
        pos.x += ctx->padding.x + child->bounds.w;
        if ((n + 1) % grid->cols == 0) {
            pos.y += ctx->padding.y + child->bounds.h;
            pos.x = x;
        }
        n++;
    }
}

static void _zui_grid_draw(zw_grid *grid) {
    zvec2 borders = zui_stylev(ZW_GRID, ZSV_BORDER);
    zcolor border_color = zui_stylec(ZW_GRID, ZSC_FOREGROUND);
    i32 i = 0;
    zrect used = grid->widget.used;
    zvec2 pos = { used.x, used.y };
    FOR_CHILDREN(grid) {
        if (i < grid->cols - 1 && borders.x > 0) {
            pos.x += child->bounds.w;
            zrect box = { pos.x + (ctx->padding.x - borders.x) / 2, used.y, borders.x, used.h };
            pos.x += ctx->padding.x;
            _push_rect_cmd(box, border_color, grid->widget.zindex);
        }
        if (i % grid->cols == 0 && i < grid->cols * (grid->rows - 1) && borders.y > 0) {
            pos.y += child->bounds.h;
            zrect box = { used.x, pos.y + (ctx->padding.y - borders.y) / 2, used.w, borders.y };
            pos.y += ctx->padding.y;
            _push_rect_cmd(box, border_color, grid->widget.zindex);
        }
        _ui_draw(child);
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
    zw_tabset *l = _cont_alloc(ZW_TABSET, sizeof(zw_tabset));
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

static u16 _zui_tabset_size(zw_tabset *tabs, bool axis, u16 bound) {
    zvec2 padding = zui_stylev(ZW_TABSET, ZSV_PADDING);
    //zvec2 tab_size = { padding.x * 2 * tabs->label_cnt, 0 };
    u16 tab_size = axis ? 0 : padding.x * 2 * tabs->label_cnt;
    zw_base *label = _ui_get_child(&tabs->widget);
    for (i32 i = 0; i < tabs->label_cnt; i++) {
        u16 sz = _ui_sz(label, axis, Z_AUTO);
        tab_size = axis ? max(tab_size, sz) : tab_size + sz;
        label = _ui_next(label);
    }
    if (axis) {
        tabs->tabheight = tab_size + padding.y * 2;
        if (bound != Z_AUTO) bound -= tabs->tabheight;
    }
    u16 child_sz = 0;
    i32 i = 0;
    FOR_SIBLINGS(tabs, label) {
        if (i != *tabs->state) child->flags |= ZF_DISABLED; // widgets that aren't visible aren't capable of being focused / hovered
        child_sz = max(child_sz, _ui_sz(child, axis, bound));
        i++;
    }
    if (axis) {
        bound += tabs->tabheight;
        if (bound == Z_AUTO) bound = max(tab_size, child_sz);
    }
    else if (bound == Z_AUTO) bound = tabs->tabheight + child_sz;
    return bound;
}

static void _zui_tabset_pos(zw_tabset *tabs, zvec2 pos) {
    zw_base *label = _ui_get_child(&tabs->widget);
    zvec2 padding = zui_stylev(ZW_TABSET, ZSV_PADDING);
    zvec2 tmp = { pos.x, pos.y + tabs->tabheight };
    pos.y += padding.y;
    for (i32 i = 0; i < tabs->label_cnt; i++) {
        pos.x += padding.x;
        _ui_pos(label, pos, tabs->widget.zindex);
        pos.x += label->bounds.w + padding.x;
        label = _ui_next(label);
    }
    pos = tmp;
    FOR_SIBLINGS(tabs, label)
        _ui_pos(child, pos, tabs->widget.zindex);
}

static void _zui_tabset_draw(zw_tabset *tabs) {
    zrect bounds = tabs->widget.bounds;
    zvec2 padding = zui_stylev(ZW_TABSET, ZSV_PADDING);
    i32 zindex = tabs->widget.zindex;
    zw_base *label = _ui_get_child(&tabs->widget);
    FOR_N_SIBLINGS(tabs, label, tabs->label_cnt) {
        zrect tab_rect = _rect_pad(label->bounds, padding);
        if (_ui_clicked(ZM_LEFT_CLICK) && _vec_within(ctx->mouse_pos, tab_rect))
            *tabs->state = i;
    }
    label = _ui_get_child(&tabs->widget);
    _push_rect_cmd((zrect) { bounds.x, bounds.y, bounds.w, tabs->tabheight }, zui_stylec(ZW_TABSET, ZSC_UNFOCUSED), zindex);
    FOR_N_SIBLINGS(tabs, label, tabs->label_cnt) {
        zrect tab_rect = _rect_pad(label->bounds, padding);
        if(i == *tabs->state)
            _push_rect_cmd(tab_rect, zui_stylec(ZW_TABSET, ZSC_BACKGROUND), zindex);
        else if(_vec_within(ctx->mouse_pos, tab_rect) && _ui_cont_hovered(&tabs->widget))
            _push_rect_cmd(tab_rect, zui_stylec(ZW_TABSET, ZSC_HOVERED), zindex);
        _ui_draw(label);
    }
    //_push_clip_cmd((zrect) { bounds.x, bounds.y + tabs->tabheight, bounds.w, bounds.h - tabs->tabheight }, zindex);
    // skip to selected sibling
    FOR_N_SIBLINGS(tabs, label, *tabs->state);
    _ui_draw(label);
}

void zui_init(zui_render_fn fn, zui_log_fn logger, void *user_data) {
    static zui_ctx global_ctx = { 0 };
    global_ctx.renderer = fn;
    global_ctx.log = logger;
    global_ctx.user_data = user_data;
    _buf_init(&global_ctx.draw, 256, sizeof(void*));
    _buf_init(&global_ctx.ui, 256, sizeof(void*));
    _buf_init(&global_ctx.registry, 256, sizeof(void*));
    _buf_init(&global_ctx.cont_stack, 256, sizeof(i32));
    _buf_init(&global_ctx.zdeque, 256, sizeof(u64));
    _buf_init(&global_ctx.text, 256, sizeof(char));
    _zmap_init(&global_ctx.glyphs);
    _zmap_init(&global_ctx.style);
    global_ctx.padding = (zvec2) { 15, 15 };
    global_ctx.latest = 0;
    ctx = &global_ctx;
    zui_register(ZW_BLANK, __zui_blank_size, 0, __zui_blank_draw);
    zui_register(ZW_WINDOW, __zui_window_size, __zui_window_pos, __zui_box_draw);

    zui_register(ZW_BOX, __zui_box_size, __zui_box_pos, __zui_box_draw);
    zui_default_style(ZW_BOX, ZSC_BACKGROUND, (zcolor) { 50, 50, 50, 255 }, ZS_DONE);

    //zui_register(ZW_POPUP, __zui_popup_size, __zui_popup_pos, __zui_popup_draw);
    zui_register(ZW_LABEL, _zui_label_size, 0, _zui_label_draw);
    zui_default_style(ZW_LABEL, ZSC_FOREGROUND, (zcolor) { 250, 250, 250, 255 }, ZS_DONE);

    zui_register(ZW_COL, _zui_layout_size, _zui_layout_pos, _zui_layout_draw);
    zui_register(ZW_ROW, _zui_layout_size, _zui_layout_pos, _zui_layout_draw);
    zui_register(ZW_BTN, __zui_box_size, __zui_box_pos, _zui_button_draw);
    zui_register(ZW_CHECK, _zui_check_size, 0, _zui_check_draw);
    zui_register(ZW_TEXT, _zui_text_size, 0, _zui_text_draw);
    zui_register(ZW_COMBO, _zui_combo_size, _zui_combo_pos, _zui_combo_draw);
    zui_register(ZW_GRID, _zui_grid_size, _zui_grid_pos, _zui_grid_draw);
    zui_default_style(ZW_GRID,
        ZSC_BACKGROUND, (zcolor) { 30, 30, 30, 255 },
        ZSC_FOREGROUND, (zcolor) { 150, 150, 150, 255 }, // border color
        ZSV_PADDING,    (zvec2)  { 15, 15 },
        ZSV_SPACING,    (zvec2)  { 15, 15 },
        ZSV_BORDER,     (zvec2)  { 0, 0 },
        ZS_DONE);

    zui_register(ZW_TABSET, _zui_tabset_size, _zui_tabset_pos, _zui_tabset_draw);
    zui_default_style(ZW_TABSET,
        ZSC_UNFOCUSED,  (zcolor) { 30, 30, 30, 255 },
        ZSC_BACKGROUND, (zcolor) { 50, 50, 50, 255 },
        ZSC_HOVERED,    (zcolor) { 40, 40, 40, 255 },
        ZSV_PADDING,    (zvec2)  { 15, 5 },
        ZS_DONE);

    zcmd start = { .id = ZCMD_INIT, .bytes = sizeof(zcmd) };
    fn((zcmd_any*)&start, user_data);
}

void zui_close() {
    free(ctx->draw.data);
    free(ctx->ui.data);
    free(ctx->registry.data);
    free(ctx->cont_stack.data);
    free(ctx->zdeque.data);
    free(ctx->text.data);
    free(ctx->glyphs.data);
    free(ctx->style.data);
    ctx = 0;
}
