#define ZUI_DEV
#define ZUI_BUF
#define ZUI_SRC
#include "zui.h"
#include <stdlib.h>
#include <string.h>

#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))

//#define assert(bool, msg) { if(!(bool)) printf(msg); exit(1); }

// Represents a registry entry (defines functions for a widget-id)
typedef struct zui_type {
    char *name;
    i16  (*size)(void*, bool, i16);
    void (*pos)(void*, zvec2, i32);
    void (*draw)(void*);
} zui_type;

// Initialize buffer
void zbuf_init(zui_buf *l, i32 cap, i32 alignment) {
    // get log2 of cap
    if (alignment & (alignment - 1))
        zui_log("ERROR: Buffer alignment must be a power of two");
    union { f32 f; i32 i; } tmp = { .f = (float)(cap - 1) };
    l->cap = (tmp.i >> 23) - 126;
    l->used = 0;
    l->alignsub1 = alignment - 1;
    l->data = malloc(cap);
}
void _zbuf_resize(zui_buf *l) {
    if(l->used <= (1 << l->cap)) return;
    l->cap++;
    l->data = realloc(l->data, 1 << l->cap);
}
// Allocate an aligned memory block on a given buffer
void *zbuf_alloc(zui_buf *l, i32 size) {
    i32 used = l->used;
    l->used += (size + l->alignsub1) & ~l->alignsub1;
    _zbuf_resize(l);
    return l->data + used;
}
i32 zbuf_align(zui_buf *l, i32 n) {
    return (n + l->alignsub1) & ~l->alignsub1;
}
void *zbuf_peek(zui_buf *l, i32 size) {
    return l->data + l->used - ((size + l->alignsub1) & ~l->alignsub1);
}
void *zbuf_pop(zui_buf *l, i32 size) {
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
void zmap_init(zmap *map) {
    map->used = 0;
    map->cap = 16;
    map->data = calloc(map->cap, sizeof(u64));
}
// Hash bits so we don't have to deal with collisions as much.
// This hashing function is 31 bit. We use the top bit to determine whether a hashmap slot is filled.
// That's why it does key << 1 >> 17. This does a 16-bit right shift while also clearing the top bit
// It's also reversible so the hashing creates no collisions
u32 zmap_hash(u32 key) {
    key = (((key << 1 >> 17) ^ key) * 0x45d9f3b);
    key = (((key << 1 >> 17) ^ key) * 0x45d9f3b);
    return ((key << 1 >> 17) ^ key) | 0x80000000;
}
u64 *zmap_node(zmap *map, u32 key) {
    i32 index = key % map->cap;
    while ((u32)map->data[index] && (u32)map->data[index] != key)
    //while (((i32)map->data[index] ^ (i32)key) > 0)
        index = (index + 1) % map->cap;
    return &map->data[index];
}
u32 *zmap_get_ptr(zmap *map, u32 key) {
    u64 *node = zmap_node(map, key);
    if((u32)*node == 0) return 0;
    return (u32*)node + 1;
}
bool zmap_get(zmap *map, u32 key, u32 *value) {
    u64 *node = zmap_node(map, key);
    *value = (*node >> 32);
    return (u32)*node != 0;
}
void zmap_set(zmap *map, u32 key, u32 value) {
    if (map->used * 4 > map->cap * 3) { // if load-factor > 75%, rehash
        u64 *old = (u64*)map->data;
        map->cap *= 2;
        map->data = calloc(map->cap, sizeof(u64));
        for (i32 i = 0; i < map->cap / 2; i++)
            if ((u32)old[i])
                *zmap_node(map, (u32)old[i]) = old[i];
        free(old);
    }
    u64 *node = zmap_node(map, key);
    if (!*node) map->used++;
    *node = ((u64)value << 32) | key;
}
// zui-glyph-cache hash
ZUI_PRIVATE u32 _zgc_hash(u16 font_id, i32 codepoint) {
    // code point must be under U+10FFFF so we can include the font_id in the key
    return zmap_hash((font_id << 21) | (codepoint & 0x1FFFFF));
}
// zui-style hash
ZUI_PRIVATE u32 _zs_hash(u16 widget_id, u16 style_id) {
    return zmap_hash(((u32)widget_id << 16) | style_id);
}

ZUI_PRIVATE u8 _utf8_masks[] = { 0, 0x7F, 0x1F, 0xF, 0x7 };
// returns the byte length of the first utf8 character in <text> and puts the unicode value in <codepoint>
i32 utf8_val(char *text, u32 *codepoint) {
    if(*text >= 0) { // most often case
        *codepoint = text[0];
        return 1;
    }
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

typedef struct zui_ctx {
    zui_render_fn renderer;
    zui_log_fn log;
    void (*wrapper)(void *augment_data);
    void *wrapper_data;
    void *user_data;
    zvec2 mouse_pos;
    zvec2 window_sz;
    u16 mouse_state;
    i32 mouse_scroll;
    u16 keyboard_modifiers;
    u16 font_cnt;
    u16 ctx_state;
    zmap glyphs;
    i32 response;
    i32 next_wid;
    i32 next_sid;
    zvec2 padding;
    u16 font_id;
    u16 longest_registry_name;
    i64 *diagnostics;
    u32 flags;
    i32 latest;
    i32 style_edits;

    zrect clip_rect;
    // We can combine registry / stack / draw, with a # to determine where to reset to
    zui_buf ui;
    zui_buf registry;   // lifetime: all the time. Changes are rare
    zui_buf cont_stack; // lifetime: tree creation
    zui_buf draw;       // lifetime: generating draw calls
    zui_buf zdeque;     // lifetime: generating draw calls
    zui_buf text;
    zui_buf json;       // lifetime: json serialization
    zmap style;
    i32 __focused; // used for calculating focused
    i32 focused;
    i32 hovered;
    zvec2 prev_mouse_pos;
    u16 prev_mouse_state;
    u16 prev_keyboard_modifiers;
    #ifdef ZUI_DEBUG
    u32 meta;
    char *filelist[16];
    #endif
} zui_ctx;

static zui_ctx *ctx = 0;

#ifdef ZUI_DEBUG
void zui_meta(i32 line, char *file) {
    for(i32 i = 0; i < 16; i++) {
        if(!ctx->filelist[i]) {
            ctx->filelist[i] = strdup(file);
            ctx->meta = i + ((u32)line << 8);
            return;
        }
        if(!strcmp(ctx->filelist[i], file)) {
            ctx->meta = i + ((u32)line << 8);
            return;
        }
    }
}
char *zui_meta_file(u32 meta) {
    if(meta == 0) return "";
    return ctx->filelist[meta & 0xFF];
}
i32 zui_meta_line(u32 meta) {
    return (meta >> 8);
}
#endif

char *zui_type_name(i32 id) {
    return ((zui_type*)ctx->registry.data)[id].name;
}

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

#ifndef ZUI_DEBUG
#define zui_err(widget, ...) zui_log("ERROR: " __VA_ARGS__)
#else
#define zui_err(widget, fmt, ...) \
    zui_log("ERROR [%s:%d]: " fmt, \
        zui_meta_file(((zw_base*)widget)->meta), \
        zui_meta_line(((zw_base*)widget)->meta), ##__VA_ARGS__)
#endif

i64 zui_ts() {
    zcmd_any ts = { .base = { ZCMD_TIMESTAMP, sizeof(zcmd_timestamp) } };
    ctx->renderer(&ts, ctx->user_data);
    return ts.timestamp.resp_ns;
}

// Returns the width and height of text given the font id [S]
i32 zui_text_width(u16 font_id, char *text, i32 len) {
    if(len == -1) len = 0x7FFFFFFF;
    u32 codepoint, v;
    i32 ret = 0;
    i64 tmp = zui_ts();
    for(i32 n, i = 0; (n = utf8_val(&text[i], &codepoint)) && codepoint && i < len; i += n, ret += v) {
        u32 hash = _zgc_hash(font_id, (i32)codepoint);
        if(zmap_get(&ctx->glyphs, hash, &v))
            continue;
        zcmd_any sz = { .glyph_sz = {
            .header = { ZCMD_GLYPH_SZ, sizeof(zcmd_glyph_sz) },
            .font_id = font_id,
            .codepoint = codepoint
        }};
        ctx->renderer(&sz, ctx->user_data);
        v = sz.glyph_sz.response.x;
        zmap_set(&ctx->glyphs, hash, v);
    }
    tmp = zui_ts() - tmp;
    ctx->diagnostics[0] += tmp;
    return ret;
}

i32 zui_text_height(u16 font_id) {
    u32 h;
    zmap_get(&ctx->glyphs, _zgc_hash(font_id, 0x1FFFFF), &h);
    return h;
}

zvec2 zui_text_vec(u16 font_id, char *text, i32 len) {
    return (zvec2) {
        .x = zui_text_width(font_id, text, len),
        .y = zui_text_height(font_id)
    };
}

i32 _zui_text_height(u16 font_id, char *text, i32 len) {
    return zui_text_height(font_id);
}

i32 (*zui_text_sz[2])(u16 font_id, char *text, i32 len) = {
    zui_text_width,
    _zui_text_height
};

// Report mouse button press
void zui_mouse_down(u16 btn) { ctx->mouse_state |= btn; }
// Report mouse button release
void zui_mouse_up(u16 btn) { ctx->mouse_state &= ~btn; }

void zui_mouse_scroll(i32 delta) { ctx->mouse_scroll += delta; }
// Report mouse move
void zui_mouse_move(zvec2 pos) { ctx->mouse_pos = pos; }
// Report key modifiers shift/alt/etc.
void zui_key_mods(u16 mod) { ctx->keyboard_modifiers = mod; }

// Report key press
void zui_key_char(i32 c) {
    i32 len = utf8_len(c);
    char *utf8 = (char*)zbuf_alloc(&ctx->text, len);
    utf8_print(utf8, c, len);
}
bool zui_key_pressed(i32 c) {
    char utf8[4];
    i32 len = utf8_len(c);
    utf8_print(utf8, c, len);
    for(i32 i = 0; i < ctx->text.used - len + 1; i++)
        if(!memcmp(utf8, ctx->text.data + i, len))
            return true;
    return false;
}
// Report window resize
void zui_resize(u16 width, u16 height) {
    ctx->window_sz = (zvec2) { width, height };
}
// Returns widget pointer given index
zw_base *_ui_widget(i32 index) {
    return (zw_base*)(ctx->ui.data + index);
}
// Returns index given widget pointer
i32 _ui_index(zw_base *ui) {
    return (i32)((u8*)ui - (u8*)ctx->ui.data);
}
// Returns next ui element in the container. If none left, returns parent
zw_base *_ui_next(zw_base *widget) {
    return (widget->flags & ZF_LAST_CHILD) ? 0 : _ui_widget(widget->next);
}
// Gets container at the top of the container stack
zw_cont *_ui_parent() {
    if (!ctx->cont_stack.used) return 0;
    i32 size = sizeof(i32) + sizeof(zstyle) * ctx->style_edits;
    i32 parent = *(i32*)zbuf_peek(&ctx->cont_stack, size);
    return (zw_cont*)_ui_widget(parent);
}
// Allocates space on the top of the UI stack
// Sets the widgets' flags, bytes, and id as necessary
void *_ui_alloc(i32 id, i32 size) {
    static bool wrapping = false;
    zw_cont *parent = _ui_parent();
    bool wrapped = parent ? ((parent->flags & ZF_WRAPPER) > 0) : 0;
    if(wrapped && !wrapping) {
        wrapping = true;
        ctx->wrapper(ctx->wrapper_data);
        wrapping = false;
    }
    parent = _ui_parent();
    if (parent)
        parent->children++;
    zw_base *prev = _ui_widget(ctx->latest);
    ctx->latest = prev->next = ctx->ui.used;
    zw_base *widget = zbuf_alloc(&ctx->ui, size);
    memset(widget, 0, size);
    widget->id = id;
    widget->bytes = size;
    widget->flags = ctx->flags;
    ctx->flags &= ZF_PERSIST;
    #ifdef ZUI_DEBUG
    widget->meta = ctx->meta;
    ctx->meta = 0;
    #endif
    if(wrapped && !wrapping) zui_end();
    return widget;
}
// An extention to _ui_alloc for containers, setting the ZF_CONTAINER flag
// It pops all local style changes off the CONT stack and associates them with the container
// It pushes the location of this container to the top of the CONT stack
// The widgets' byte count is adjusted to consider the style edits
void *_cont_alloc(i32 id, i32 size) {
    i32 len = (i32)sizeof(zstyle) * ctx->style_edits;
    zw_cont *cont = _ui_alloc(id, size);
    cont->bytes += len;
    cont->style_edits = ctx->style_edits;
    cont->flags |= ZF_CONTAINER;
    zstyle *new_edits = zbuf_alloc(&ctx->ui, len);
    zstyle *edits = zbuf_pop(&ctx->cont_stack, len);
    ctx->style_edits = 0;
    memcpy(new_edits, edits, len);
    *(i32*)zbuf_alloc(&ctx->cont_stack, sizeof(i32)) = _ui_index((zw_base*)cont);
    return cont;
}
zcmd_any *_draw_alloc(u16 id, u16 size, i32 zindex) {
    u64 *index = zbuf_alloc(&ctx->zdeque, sizeof(u64));
    zcmd_any *ret = (zcmd_any*)zbuf_alloc(&ctx->draw, size);
    ret->base.id = id;
    ret->base.bytes = size;
    // high bytes represent z-index, low bits are index into the pointer
    // we can sort this deque as 64 bit integers: which will sort zindex first, then by insertion order
    *index = ((u64)zindex << 32) | ((u8*)ret - ctx->draw.data);
    return ret;
}
zrect _rect_add(zrect a, zrect b) {
    return (zrect) { a.x + b.x, a.y + b.y, a.w + b.w, a.h + b.h };
}
// increase width/height by padding*2, offset x/y to remain centered
zrect _rect_pad(zrect r, zvec2 padding) {
    return (zrect) { r.x - padding.x, r.y - padding.y, r.w + padding.x * 2, r.h + padding.y * 2 };
}
void _push_rect_cmd(zrect rect, zcolor color, i32 zindex) {
    zcmd_rect *r = &_draw_alloc(ZCMD_DRAW_RECT, sizeof(zcmd_rect), zindex)->rect;
    r->rect = rect;
    r->color = color;
}
void _push_clip_cmd(zrect rect, i32 zindex) {
    zcmd_clip *r = &_draw_alloc(ZCMD_DRAW_CLIP, sizeof(zcmd_clip), zindex)->clip;
    r->rect = rect;
}
void _push_text_cmd(u16 font_id, zvec2 coord, zcolor color, char *text, i32 len, i32 zindex) {
    if(len == -1) len = strlen(text);
    zcmd_text *r = &_draw_alloc(ZCMD_DRAW_TEXT, sizeof(zcmd_text) + len, zindex)->text;
    r->font_id = font_id;
    r->pos = coord;
    r->color = color;
    memcpy(r->text, text, len);
}
void _push_bezier_cmd(i32 cnt, zvec2 *points, i32 width, zcolor color, i32 zindex) {
    zcmd_bezier *b = &_draw_alloc(ZCMD_DRAW_BEZIER, sizeof(zcmd_bezier) + cnt * sizeof(zvec2), zindex)->bezier;
    b->color = color;
    b->width = width;
    for(i32 i = 0; i < cnt; i++) b->points[i] = points[i];
}

zvec2 _vec_max(zvec2 a, zvec2 b) { return (zvec2) { max(a.x, b.x), max(a.y, b.y) }; }
zvec2 _vec_min(zvec2 a, zvec2 b) { return (zvec2) { min(a.x, b.x), min(a.y, b.y) }; }
zvec2 _vec_add(zvec2 a, zvec2 b) { return (zvec2) { a.x + b.x, a.y + b.y }; }
zvec2 _vec_sub(zvec2 a, zvec2 b) { return (zvec2) { a.x - b.x, a.y - b.y }; }
i32 _vec_distsq(zvec2 a, zvec2 b) {
    zvec2 v = _vec_sub(a, b);
    return v.x*v.x + v.y*v.y;
}

bool _vec_within(zvec2 v, zrect bounds) {
    return (v.x >= bounds.x) && (v.x <= bounds.x + bounds.w) && (v.y >= bounds.y) && (v.y <= bounds.y + bounds.h);
}
bool _rect_within(zrect r, zrect bounds) {
    return (r.x >= bounds.x) && (r.x + r.w <= bounds.x + bounds.w) && (r.y >= bounds.y) && (r.y + r.h <= bounds.y + bounds.h);
}
bool _rect_intersect(zrect a, zrect b, zrect *intersect) {
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
void _rect_justify(zrect *used, zrect bounds, i32 justification) {
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
    for(i32 index = si + start->bytes; index < ctx->ui.used;) {
        zw_base *w = _ui_widget(index);
        if (w->flags & flags)
            return w;
        index += zbuf_align(&ctx->ui, _ui_widget(index)->bytes);
    }
    for (i32 index = 0; index < si;) {
        zw_base *w = _ui_widget(index);
        if (w->flags & flags)
            return w;
        index += zbuf_align(&ctx->ui, _ui_widget(index)->bytes);
    }
    return 0;
}
zw_base *_ui_get_child(zw_base *ui) {
    if ((~ui->flags & ZF_CONTAINER) || ((zw_cont*)ui)->children <= 0) return 0;
    return (zw_base*)((u8*)ui + ((ui->bytes + ctx->ui.alignsub1) & ~ctx->ui.alignsub1));
}
zw_base *_ui_nth_child(zw_base *ui, i32 n) {
    zw_base *c = _ui_get_child(ui);
    for(i32 i = 0; i < n && c; i++)
        c = _ui_next(c);
    return c;
}
bool _ui_pressed(i32 buttons) {
    return !!(ctx->mouse_state & buttons);
}
bool _ui_dragged(i32 buttons) {
    return !!(ctx->mouse_state & ctx->prev_mouse_state & buttons);
}
bool _ui_clicked(i32 buttons) {
    return !!(ctx->mouse_state & (ctx->mouse_state ^ ctx->prev_mouse_state) & buttons);
}
bool _ui_released(i32 buttons) {
    return !!(~ctx->mouse_state & (ctx->mouse_state ^ ctx->prev_mouse_state) & buttons);
}

void _ui_print(zw_base *cmd, int indent, bool expand_children) {
    zrect b = cmd->bounds, u = cmd->used;
    char *name = ((zui_type*)ctx->registry.data)[cmd->id].name;
    if(cmd->flags & ZF_CONTAINER)
        zui_log("%04x | %-*s[%s] (next: %04x, bounds: {%d,%d,%d,%d}, used: {%d,%d,%d,%d}, z: %d, children: %d)\n", _ui_index(cmd), indent, "", name, cmd->next, b.x, b.y, b.w, b.h, u.x, u.y, u.w, u.h, cmd->zindex, ((zw_cont*)cmd)->children);
    else
        zui_log("%04x | %-*s[%s] (next: %04x, bounds: {%d,%d,%d,%d}, used: {%d,%d,%d,%d}, z: %d)\n", _ui_index(cmd), indent, "", name, cmd->next, b.x, b.y, b.w, b.h, u.x, u.y, u.w, u.h, cmd->zindex);
    if(expand_children)
        FOR_CHILDREN(cmd)
            _ui_print(child, indent + 2, expand_children);
}

zvec2 _ui_mpos() { return ctx->mouse_pos; }
zvec2 _ui_mdelta() { return _vec_sub(ctx->mouse_pos, ctx->prev_mouse_pos); }

i32 _ui_child_cnt(zw_base *ui) { return (ui->flags & ZF_CONTAINER) ? ((zw_cont*)ui)->children : 0; }
bool _ui_is_child(zw_base *container, zw_base *other) {
    return container <= other && other < _ui_widget(container->next);
}
bool _ui_hovered(zw_base *ui) { return ui == _ui_widget(ctx->hovered); }
bool _ui_cont_hovered(zw_base *ui) { return _ui_is_child(ui, _ui_widget(ctx->hovered)); }
bool _ui_focused(zw_base *ui) { return ui == _ui_widget(ctx->focused); }
bool _ui_cont_focused(zw_base *ui) { return _ui_is_child(ui, _ui_widget(ctx->focused)); }

zstyle *_ui_get_styles(zw_cont *ui) {
    return (zstyle*)((u8*)ui + zbuf_align(&ctx->ui, ui->bytes - ui->style_edits * sizeof(zstyle)));
}

bool _ui_apply_styles(zw_base *ui) {
    zw_cont *c = (zw_cont*)ui;
    if((~ui->flags & ZF_CONTAINER) || !c->style_edits) return false;
    zstyle *edits = _ui_get_styles(c);
    for (i32 i = 0; i < c->style_edits; i++) {
        u32 value, key = _zs_hash(edits[i].widget_id, edits[i].style_id);
        if (!zmap_get(&ctx->style, key, &value))
            zui_log("WARNING: No default style for widget-id:%d,style-id:%d\n", edits[i].widget_id, edits[i].style_id);
        zmap_set(&ctx->style, key, edits[i].value.u);
        edits[i].value.u = value; // save previous value
    }
    return true;
}

void _ui_restore_styles(zw_base *ui) {
    zw_cont *c = (zw_cont*)ui;
    zstyle *edits = _ui_get_styles(c);
    for (i32 i = 0; i < c->style_edits; i++) {
        u32 *ptr = zmap_get_ptr(&ctx->style, _zs_hash(edits[i].widget_id, edits[i].style_id));
        u32 style_edit = *ptr;
        *ptr = edits[i].value.u;
        edits[i].value.u = style_edit;
    }
}

i16 _ui_sz(zw_base *ui, bool axis, i16 bound) {
    if(!ui) return 0;
    static bool recalc = false;
    if(recalc && (~ui->flags & ZF_CONTAINER)) {
        if(bound == Z_AUTO) return ui->bounds.sz.e[axis];
        return (ui->bounds.sz.e[axis] = bound);
    }
    zui_type type = ((zui_type*)ctx->registry.data)[ui->id - ZW_FIRST];
    bool applied = _ui_apply_styles(ui);
    if(ui->flags & (ZF_SELF_WIDTH << axis)) bound = ui->bounds.sz.e[axis];
    i16 sz = type.size(ui, axis, bound);
    // second pass if FILL on axis with auto size.
    if((ui->flags & (ZF_FILL_X << axis)) && bound == Z_AUTO) {
        recalc = true;
        type.size(ui, axis, sz);
        recalc = false;
    }
    ui->used.sz.e[axis] = sz;
    ui->bounds.sz.e[axis] = bound == Z_AUTO ? ui->used.sz.e[axis] : bound;
    if(applied) _ui_restore_styles(ui);
    return (ui->flags & (ZF_SELF_WIDTH << axis)) ? 0 : ui->bounds.sz.e[axis];
}

// FOR INTERNAL USE OF _ui_pos ONLY
void _ui_pos_updater(zw_base *ui, zrect rect) {
    _rect_justify(&ui->used, ui->bounds, ui->flags);
    if(ui->flags & ZF_SELF_POS) ctx->clip_rect = ui->used;
    else if(!_rect_intersect(ui->used, rect, &ctx->clip_rect)) return;
    if((~ui->flags & ZF_DISABLED) && _vec_within(ctx->mouse_pos, ctx->clip_rect)) {
        if (ui->zindex >= _ui_widget(ctx->hovered)->zindex)
            ctx->hovered = _ui_index(ui);
        if(_ui_clicked(ZM_LEFT_CLICK) && ui->zindex >= _ui_widget(ctx->__focused)->zindex)
            ctx->__focused = _ui_index(ui);
    }
}
// FOR INTERNAL USE OF _ui_pos ONLY
void _ui_pos_recurse(zw_base *ui, zvec2 pos, i32 zindex) {
    zui_type type = ((zui_type*)ctx->registry.data)[ui->id - ZW_FIRST];
    if(!type.pos) return;
    bool applied = _ui_apply_styles(ui);
    type.pos(ui, pos, zindex);
    if(applied) _ui_restore_styles(ui);
}
// _ui_pos serves two purposes.
// For normal containers it simply calculates the position of child widgets
// For anything with an ZF_INDEPENDENT flag, it calculates both it's own position,
// and the position of any children (if necessary).
//
// Standard positioning:
// 1. Disable all necessary widgets
// 2. Justify widget within bounds
// 3. Based on zindex, update focused/hovered widgets
// 4. Calculate position of widget's children via recursive _ui_pos
//
// Positioning for an independent:
// 1. Disable all necessary widgets
// 2. Schedule justification, and focused/hovered updating for next _ui_pos call
// 3. Call _ui_pos to get position and whatnot
//  - if _ui_pos recurses it will immediately trigger the scheduled updates before
//    the children get positioned. This makes mouse selection work normally
void _ui_pos(zw_base *ui, zvec2 pos, i32 zindex) {
    if(!ui) return;
    zui_type type = ((zui_type*)ctx->registry.data)[ui->id - ZW_FIRST];
    if ((ui->flags & ZF_DISABLED) && (ui->flags & ZF_CONTAINER))
        FOR_CHILDREN(ui) child->flags |= ZF_DISABLED; // all children of a disabled element must also be disabled
    static i32 independent = -1;
    static zrect old_rect;
    if(independent != -1) { // process scheduled independent
        _ui_pos_updater(_ui_widget(independent), old_rect);
        independent = -1;
    }
    zrect prev = ctx->clip_rect;
    if(ui->flags & ZF_SELF_POS) {
        independent = _ui_index(ui);
        _ui_pos_recurse(ui, pos, zindex);
        if(!_ui_child_cnt(ui)) // without children, the scheduling won't work
            _ui_pos_updater(ui, prev);
        independent = -1;
    } else {
        ui->zindex = zindex;
        ui->bounds.pos = pos;
        _ui_pos_updater(ui, prev);
        _ui_pos_recurse(ui, ui->used.pos, zindex);
    }
    ctx->clip_rect = prev;
}

void _ui_draw(zw_base *ui) {
    static i32 indent = 0;
    zui_type type = ((zui_type*)ctx->registry.data)[ui->id - ZW_FIRST];
    zrect prev_clip = ctx->clip_rect;
    if(ui->flags & ZF_SELF_POS ?
        (ctx->clip_rect = ui->used, true) :
        _rect_intersect(ui->bounds, prev_clip, &ctx->clip_rect))
    {
        _push_clip_cmd(ctx->clip_rect, ui->zindex);
        bool applied = _ui_apply_styles(ui);
        type.draw(ui);
        if(applied) _ui_restore_styles(ui);
        _push_clip_cmd(prev_clip, ui->zindex);
    }
    ctx->clip_rect = prev_clip;
}

void zui_print_tree() {
    zui_log("printing tree...\n");
    _ui_print(_ui_widget(0), 0, true);
}

void zui_print_active() {
    zui_log("active widgets...\n");
    zui_log("hovered: ");
    _ui_print(_ui_widget(ctx->hovered), 0, false);
    zui_log("focused: ");
    _ui_print(_ui_widget(ctx->focused), 0, false);
}


i32 zui_new_wid() { return ctx->next_wid++; }
i32 zui_new_sid() { return ctx->next_sid++; }

void zui_register(i32 widget_id, char *widget_name, void *size_cb, void *pos_cb, void *draw_cb) {
    ctx->registry.used = (widget_id - ZW_FIRST) * sizeof(zui_type);
    zui_type *t = zbuf_alloc(&ctx->registry, sizeof(zui_type));
    u16 len = strlen(widget_name);
    if(len > ctx->longest_registry_name)
        ctx->longest_registry_name = len;
    t->name = widget_name;
    t->size = (i16(*)(void*, bool, i16))size_cb;
    t->pos = (void(*)(void*, zvec2, i32))pos_cb;
    t->draw = (void(*)(void*))draw_cb;
}

void zui_justify(u32 justification) {
    ctx->flags = justification & 15;
}

void zui_disable() {
    ctx->flags |= ZF_DISABLED;
}

void zui_fill(u32 axis) {
    ctx->flags |= axis & (ZF_FILL_X | ZF_FILL_Y);
}

u16 zui_new_font(char *family, i32 size) {
    i32 len = strlen(family), bytes = sizeof(zcmd_reg_font) + len + 1;
    zcmd_reg_font *font = _alloca(bytes);
    font->header = (zcmd) { ZCMD_REG_FONT, bytes };
    font->font_id = ctx->font_cnt;
    font->size = size;
    font->response_height = 0;
    memcpy(font->family, family, len);
    font->family[len] = 0;
    ctx->renderer((zcmd_any*)font, ctx->user_data);
    if(!font->response_height) {
        zui_log("Failed to create font %s\n", family);
        return 0;
    }
    zmap_set(&ctx->glyphs, _zgc_hash(ctx->font_cnt, 0x1FFFFF), font->response_height);
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

// ends any container (window / grid)
void zui_end() {
    if(ctx->cont_stack.used <= 0) {
        zui_log("Too many zui_end calls. Broken tree\n");
        return;
    }
    i32 container = *(i32*)zbuf_pop(&ctx->cont_stack, sizeof(i32) + sizeof(zstyle) * ctx->style_edits);
    zw_base *latest = _ui_widget(ctx->latest);
    zw_cont *cont = (zw_cont*)_ui_widget(container);
    // if active container isn't the most recent element set parent flag (it has children)
    if (container != ctx->latest) {
        latest->flags |= ZF_LAST_CHILD;
        latest->next = ctx->ui.used;
        ctx->latest = container;
    }
    if(cont->flags & ZF_END_PARENT)
        zui_end();
}

void zui_style(u32 widget_id, ...) {
    va_list args;
    va_start(args, widget_id);
    for (u32 style_id; (style_id = va_arg(args, u32)) != ZS_DONE; ) {
        zstyle *style = (zstyle*)zbuf_alloc(&ctx->cont_stack, sizeof(zstyle));
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
        zmap_set(&ctx->style, _zs_hash(widget_id, style_id), value);
    }
    va_end(args);
}
ZUI_PRIVATE void _zui_get_style(u16 widget_id, u16 style_id, void *ptr) {
    if(!zmap_get(&ctx->style, _zs_hash(widget_id, style_id), (u32*)ptr))
        zui_log("No style exists for widget:%d,style:%d\n", widget_id, style_id);
}
zcolor zui_stylec(u16 widget_id, u16 style_id) { zcolor ret; _zui_get_style(widget_id, style_id, &ret); return ret; }
zvec2  zui_stylev(u16 widget_id, u16 style_id) { zvec2  ret; _zui_get_style(widget_id, style_id, &ret); return ret; }
float  zui_stylef(u16 widget_id, u16 style_id) { float  ret; _zui_get_style(widget_id, style_id, &ret); return ret; }
i32    zui_stylei(u16 widget_id, u16 style_id) { i32    ret; _zui_get_style(widget_id, style_id, &ret); return ret; }

void zui_render() {
    if (ctx->cont_stack.used != 0) {
        zui_log("incorrect # of zui_end calls\n");
        return;
    }
    if(ctx->window_sz.x == 0 || ctx->window_sz.y == 0) return;

    i64 tmp = 0;
    ctx->diagnostics = &tmp;

    // calculate sizes
    zw_base *root = _ui_widget(0);
    root->next = 0;
    i64 szx_time = zui_ts();
    _ui_sz(root, 0, ctx->window_sz.x);
    szx_time = zui_ts() - szx_time;
    i64 szy_time = zui_ts();
    _ui_sz(root, 1, ctx->window_sz.y);
    szy_time = zui_ts() - szy_time;

    // calculate positions
    ctx->hovered = 0;
    root->bounds.x = 0;
    root->bounds.y = 0;
    i64 pos_time = zui_ts();
    _ui_pos(root, (zvec2) { 0, 0 }, 0);
    pos_time = zui_ts() - pos_time;
    if (ctx->__focused) {
        ctx->focused = ctx->__focused;
        ctx->__focused = 0;
    }

    // generate draw commands
    i64 draw_time = zui_ts();
    ctx->clip_rect = root->used;
    _ui_draw(root);
    draw_time = zui_ts() - draw_time;

    // sort draw commands by zindex / index (order of creation)
    // despite qsort not being a stable sort, the order of draw cmd creation is preserved due to index being part of each u64
    u64 *deque_reader = (u64*)ctx->zdeque.data;
    _zui_qsort(deque_reader, ctx->zdeque.used / sizeof(u64));
    i64 render_time = zui_ts();
    zcmd_any begin = { .base = { ZCMD_RENDER_BEGIN, sizeof(zcmd) } };
    ctx->renderer(&begin, ctx->user_data);
    i32 i = 0;
    while(deque_reader < (u64*)(ctx->zdeque.data + ctx->zdeque.used)) {
        u64 next_pair = *deque_reader++;
        i32 index = next_pair & 0x7FFFFFFF;
        zcmd_any *next = (zcmd_any*)(ctx->draw.data + index);
        ctx->renderer(next, ctx->user_data);
    }
    zcmd_any end = { .base = { ZCMD_RENDER_END, sizeof(zcmd) } };
    ctx->renderer(&end, ctx->user_data);
    render_time = zui_ts() - render_time;

    ctx->prev_mouse_pos = ctx->mouse_pos;
    ctx->prev_mouse_state = ctx->mouse_state;
    ctx->mouse_scroll = 0;
    ctx->text.used = 0;
    ctx->zdeque.used = 0;
    ctx->draw.used = 0;
    ctx->ui.used = 0;

    // zui_log("DIAGNOSTICS\n");
    // zui_log("txt sz: %.2fms\n", ctx->diagnostics[0] / 1000000.0);
    // zui_log("size:   %.2fms | %.2f + %.2f\n", (szx_time + szy_time) / 1000000.0, szx_time / 1000000.0, szy_time / 1000000.0);
    // zui_log("pos:    %.2fms\n", pos_time / 1000000.0);
    // zui_log("draw:   %.2fms\n", draw_time / 1000000.0);
    // zui_log("render: %.2fms\n", render_time / 1000000.0);
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
ZUI_PRIVATE i16 _zui_blank_size(zw_base *w, bool axis, i16 bound) { return bound == Z_AUTO ? 0 : bound; }
ZUI_PRIVATE void _zui_blank_draw(zw_base *w) {}

// returns true if window is displayed
void zui_box() {
    _cont_alloc(ZW_BOX, sizeof(zw_box));
}
ZUI_PRIVATE i16 _zui_box_size(zw_box *box, bool axis, i16 bound) {
    zvec2 padding = zui_stylev(box->cmd.id, ZSV_PADDING);
    if (bound != Z_AUTO) bound -= padding.e[axis] * 2;
    i16 auto_sz = 0;
    FOR_CHILDREN(box)
        auto_sz = max(auto_sz, _ui_sz(child, axis, bound));
    return (bound == Z_AUTO ? auto_sz : bound) + padding.e[axis] * 2;
}
ZUI_PRIVATE void _zui_box_pos(zw_box *box, zvec2 pos, i32 zindex) {
    zvec2 padding = zui_stylev(box->cmd.id, ZSV_PADDING);
    zvec2 child_pos = { pos.x + padding.x, pos.y + padding.y };
    FOR_CHILDREN(box) _ui_pos(child, child_pos, zindex);
}
ZUI_PRIVATE void _zui_box_draw(zw_box *box) {
    _push_rect_cmd(box->widget.bounds, zui_stylec(box->cmd.id, ZSC_BACKGROUND), box->widget.zindex);
    FOR_CHILDREN(box) _ui_draw(child);
}

void zui_popup(i32 width, i32 height, zd_popup *state) {
    ctx->flags |= ZF_SELF_POS | ZF_SELF_WIDTH | ZF_SELF_HEIGHT;
    zw_popup *popup = _cont_alloc(ZW_POPUP, sizeof(zw_popup));
    popup->widget.bounds.sz = (zvec2) { width, height };
    popup->state = state;
    popup->widget.zindex = 1000;
}
ZUI_PRIVATE i16 _zui_popup_size(zw_popup *p, bool axis, i16 bound) {
    if(!p->state->init) {
        zvec2 diff = _vec_sub(ctx->window_sz, p->widget.bounds.sz);
        p->state->pos = (zvec2) { diff.x / 2, diff.y / 2 };
        p->state->init = 1;
    }
    // since zw_popup is INDEPENDENT we must also set the position
    _ui_sz(_ui_get_child(&p->widget), axis, bound - 4);
    return bound;
}
ZUI_PRIVATE void _zui_popup_pos(zw_popup *p, zvec2 pos, i32 zindex) {
    p->widget.zindex = zindex = 1000;
    pos = p->cont.bounds.pos = p->cont.used.pos = p->state->pos;
    _ui_pos(_ui_get_child(&p->widget), _vec_add(pos, (zvec2) { 2, 2 }), zindex);
}
ZUI_PRIVATE void _zui_popup_draw(zw_popup *p) {
    zvec2 border = { -2, -2 };
    if(_ui_cont_focused(&p->widget) && _ui_dragged(ZM_LEFT_CLICK))
        p->state->pos = _vec_add(p->state->pos, _ui_mdelta());
    _push_rect_cmd(p->widget.bounds, (zcolor) { 255, 255, 255, 255 }, p->widget.zindex);
    zrect r = _rect_pad(p->widget.bounds, border);
    _push_rect_cmd(r, zui_stylec(p->cmd.id, ZSC_BACKGROUND), p->widget.zindex);
    _ui_draw(_ui_get_child(&p->widget));
}

void zui_window() {
    ctx->ui.used = 0;
    ctx->flags = 0;
    _cont_alloc(ZW_WINDOW, sizeof(zw_box));
}
ZUI_PRIVATE i16 _zui_window_size(zw_box *window, bool axis, i16 bound) {
    FOR_CHILDREN(window) _ui_sz(child, axis, bound);
    return bound;
}
ZUI_PRIVATE void _zui_window_pos(zw_box *window, zvec2 pos) {
    FOR_CHILDREN(window) _ui_pos(child, pos, 1);
}

zvec2 zui_window_sz() {
    return ctx->window_sz;
}

// LABEL
ZUI_PRIVATE void _append(char *s, i32 n, char c) {
    if((n & ~ctx->ui.alignsub1) == n)
        zbuf_alloc(&ctx->ui, ctx->ui.alignsub1 + 1);
    s[n] = c;
}

ZUI_PRIVATE i32 _append_x32(char *s, i32 n, u32 v, i32 digits) {
    char buf[8];
    i32 end = digits ? digits : 8, i = 0;
    for(; i < end; i++) {
        if(!v && !digits && i > 0) break;
        char low = v & 15;
        buf[i] = low > 9 ? low - 10 + 'A' : low + '0';
        v >>= 4;
    }
    for(i32 j = i - 1; j >= 0; j--)
        _append(s, n++, buf[j]);
    return i;
}

ZUI_PRIVATE i32 _append_i32(char *s, i32 n, i32 v, i32 zeros) {
    if(v < 0) {
        _append(s, n, '-');
        return _append_i32(s, n + 1, -v, zeros) + 1;
    }
    i32 r = 0;
    if(v >= 10 || zeros > 1) r = _append_i32(s, n, v / 10, zeros - 1);
    _append(s, n + r, (v % 10) + '0');
    return r + 1;
}

ZUI_PRIVATE i32 _append_f64(char *s, i32 n, double v, i32 leading, i32 precision) {
    if(precision == 0)
        return _append_i32(s, n, (i32)(v + 0.5), leading);
    if(v < 0) {
        _append(s, n, '-');
        return _append_f64(s, n + 1, -v, leading, precision) + 1;
    }
    i32 r = _append_i32(s, n, (i32)v, leading);
    if(precision == 0) return r;
    _append(s, n + r, '.');
    r++;
    v *= 10;
    for(i32 i = 1; i < precision; i++, v *= 10) {
        _append(s, n + r++, ((i32)v % 10) + '0');
    }
    _append(s, n + r++, ((i32)(v + 0.5) % 10) + '0');
    return r;
}

void zui_labelf(const char *fmt, ...) {
    zw_labelf *l = _ui_alloc(ZW_LABELF, sizeof(zw_labelf));
    va_list args;
    va_start(args, fmt);
    i32 n = 0;
    for(; *fmt; fmt++) {
        if(*fmt != '%') {
            _append(l->text, n++, *fmt);
            continue;
        }
        fmt++;
        i32 mod = 0;
        while(*fmt >= '0' && *fmt <= '9')
            mod = (mod * 10) - '0' + *fmt++;

        switch(*fmt) {
            case 's': {
                char *s = va_arg(args, char*);
                for(; *s; s++) _append(l->text, n++, *s);
            } break;
            case 'x': n += _append_x32(l->text, n, va_arg(args, u32), mod); break;
            case 'd': n += _append_i32(l->text, n, va_arg(args, i32), 0); break;
            case 'f': n += _append_f64(l->text, n, va_arg(args, double), 0, 2); break;
            case '%': _append(l->text, n++, '%'); break;
        }
    }
    _append(l->text, n, 0);
    l->widget.bytes += n;
    va_end(args);
}

ZUI_PRIVATE i16 _zui_labelf_size(zw_labelf *data, bool axis, i16 bound) {
    i32 len = data->cmd.bytes - sizeof(zw_labelf);
    return zui_text_sz[axis](ctx->font_id, data->text, len);
}

ZUI_PRIVATE void _zui_labelf_draw(zw_labelf *data) {
    i32 len = data->cmd.bytes - sizeof(zw_labelf);
    _push_text_cmd(ctx->font_id, data->widget.used.pos, zui_stylec(ZW_LABEL, ZSC_FOREGROUND), data->text, len, data->widget.zindex);
}

void zui_labeln(char *text, i32 len) {
    zw_label *l = _ui_alloc(ZW_LABEL, sizeof(zw_label));
    l->text = text;
    l->len = len;
}

void zui_label(const char *text) {
    zui_labeln((char*)text, (i32)strlen(text));
}

ZUI_PRIVATE i16 _zui_label_size(zw_label *data, bool axis, i16 bound) {
    return zui_text_sz[axis](ctx->font_id, data->text, data->len);
}

ZUI_PRIVATE void _zui_label_draw(zw_label *data) {
    _push_text_cmd(ctx->font_id, data->widget.used.pos, zui_stylec(ZW_LABEL, ZSC_FOREGROUND), data->text, data->len, data->widget.zindex);
}

void zui_scroll(bool xbar, bool ybar, zd_scroll *state) {
    zw_scroll *s = _cont_alloc(ZW_SCROLL, sizeof(zw_scroll));
    s->xbar = xbar;
    s->ybar = ybar;
    s->state = state;
}

ZUI_PRIVATE i16 _zui_scroll_size(zw_scroll *s, bool axis, i16 bound) {
    i16 m = 0;
    if(bound == Z_AUTO) {
        FOR_CHILDREN(s) {
            i16 sz = _ui_sz(child, axis, bound);
            if(sz > m) m = sz;
        }
        return m + 5;
    }

    i16 child_bound = (axis ? s->ybar : s->xbar) ? Z_AUTO : bound - 5;
    FOR_CHILDREN(s)
        _ui_sz(child, axis, child_bound);
    return bound;
}

ZUI_PRIVATE void _zui_scroll_pos(zw_scroll *s, zvec2 pos, i32 zindex) {
    zw_base *child = _ui_get_child(&s->widget);
    i16 min = s->widget.used.h - child->used.h, max = 0;
        // this should be moved to draw
    if((~s->widget.flags & ZF_DISABLED) && _ui_cont_hovered(&s->widget) && min < max) {
        s->state->pos.y += ctx->mouse_scroll * 10;
        if     (s->state->pos.y > max) s->state->pos.y = max;
        else if(s->state->pos.y < min) s->state->pos.y = min;
    }
    _ui_pos(child, _vec_add(pos, s->state->pos), zindex);
}

ZUI_PRIVATE void _zui_scroll_draw(zw_scroll *s) {
    zrect used = s->widget.used;
    zrect ybarback = { used.x + used.w - 5, used.y, 5, used.h };
    zw_base *child = _ui_get_child(&s->widget);

    i32 travel_dist = child->used.h - s->widget.used.h, bar_dist = 0;
    bool show_drag = travel_dist > 0;

    zrect drag;
    if(show_drag) {
        i32 bar_size = s->widget.used.h * s->widget.used.h / child->used.h;
        if(bar_size < 20) bar_size = 20;
        bar_dist = s->widget.used.h - bar_size;
        i32 pos = s->state->pos.y * bar_dist / travel_dist;
        drag = (zrect) { ybarback.x, used.y - pos, 5, bar_size };
    }

    if(_ui_cont_focused(&s->widget) && show_drag) {
        if(_ui_clicked(ZM_LEFT_CLICK) && _vec_within(_ui_mpos(), drag))
            s->state->dragging = true;
        else if(!_ui_dragged(ZM_LEFT_CLICK))
            s->state->dragging = false;
        if(s->state->dragging) {
            i16 min = s->widget.used.h - child->used.h, max = 0;
            zvec2 v = _ui_mdelta();
            s->state->pos.y -= v.y * travel_dist / bar_dist;
            if     (s->state->pos.y > max) s->state->pos.y = max;
            else if(s->state->pos.y < min) s->state->pos.y = min;
        }
    } else {
        s->state->dragging = false;
    }
    i32 zindex = s->widget.zindex;
    if(s->ybar) {
        _push_rect_cmd(ybarback, (zcolor) { 200, 200, 200, 200 }, zindex);
        if(show_drag) {
            drag = _rect_pad(drag, (zvec2) { -1, -1 });
            _push_rect_cmd(drag, (zcolor) { 30, 30, 30, 255 }, zindex);
        }
    }
    _ui_draw(child);
}


// BUTTON
bool zui_radio_btn(u8 *state, u8 id) {
    zw_btn *l = _cont_alloc(ZW_BTN, sizeof(zw_btn));
    l->state = state;
    l->id = id;
    bool ret = (*state == id);
    if(id == 255) *state = 0;
    return ret;
}
bool zui_button(u8 *state) {
    return zui_radio_btn(state, 255);
}
bool zui_button_txt(const char *text, u8 *state) {
    bool ret = zui_button(state);
    zui_label(text);
    zui_end();
    return ret;
}
// A button is just a clickable box
// This allows trivial addition of various button kinds: with images, multiple labels, etc.
// Reuse __zui_box_size for sizing

ZUI_PRIVATE void _zui_button_draw(zw_btn *btn) {
    zcolor c = (zcolor) { 80, 80, 80, 255 };
    if(*btn->state == btn->id)
        c = (zcolor) { 120, 120, 120, 255 };
    else if(_ui_cont_hovered(&btn->widget)) {// hovered
        bool clicked = _ui_clicked(ZM_LEFT_CLICK);
        if(clicked) *btn->state = btn->id;
        else if(*btn->state == btn->id) *btn->state = 0;
        if(_ui_pressed(ZM_LEFT_CLICK))
            c = (zcolor) { 120, 120, 120, 255 };
        else
            c = (zcolor) { 100, 100, 100, 255 };
    }
    _push_rect_cmd(btn->widget.used, c, btn->widget.zindex);
    FOR_CHILDREN(btn) {
        if(child->id == ZW_DROPDOWN && *btn->state == btn->id) {
            u8 *dropstate = ((zw_dropdown*)child)->state;
            *dropstate |= 1;
        }
        _ui_draw(child);
    }
}

bool zui_check(u8 *state) {
    zw_check *c = _ui_alloc(ZW_CHECK, sizeof(zw_check));
    c->state = state;
    return *state;
}

ZUI_PRIVATE i16 _zui_check_size(zw_check *data, bool axis, i16 bound) {
    return zui_text_height(ctx->font_id) + 2;
}

ZUI_PRIVATE void _zui_check_draw(zw_check *data) {
    zcolor on =  (zcolor) { 60,  90,  250, 255 };
    zcolor off = (zcolor) { 200, 200, 200, 255 };
    if (_ui_hovered(&data->widget)) {// hovered
        *data->state ^= _ui_clicked(ZM_LEFT_CLICK);
        on  = (zcolor) {  80, 110, 250, 255 };
        off = (zcolor) { 230, 230, 230, 255 };
    }
    zvec2 sz = zui_text_vec(ctx->font_id, "\xE2\x88\x9A", 3);
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

void zui_dropdown_flags(i32 flags, u8 *state) {
    i32 parent_id = _ui_index((zw_base*)_ui_parent());
    zw_dropdown *b = _cont_alloc(ZW_DROPDOWN, sizeof(zw_dropdown));
    b->state = state;
    b->widget.flags |= ZF_SELF_POS | (flags & ~ZF_PERSIST);
    if(!*state) b->widget.flags |= ZF_DISABLED;
    b->widget.bounds.sz = (zvec2) { Z_AUTO, Z_AUTO };
    b->parent = parent_id;
    b->direction = flags & ZF_PERSIST;
}
void zui_dropdown(i32 dir, u8 *state) {
    zui_dropdown_flags(dir | ZF_SELF_HEIGHT | ZF_SELF_WIDTH, state);
}
i16 _zui_dropdown_size(zw_dropdown *data, bool axis, i16 bound) {
    return _ui_sz(_ui_get_child(&data->widget), axis, bound);
}
void _zui_dropdown_pos(zw_dropdown *d, zvec2 pos, i32 zindex) {
    zw_base *parent = _ui_widget(d->parent);
    zrect s = d->widget.bounds;
    zrect r = parent->bounds;
    switch(d->direction & ZF_PERSIST) {
        case ZJ_RIGHT: pos = (zvec2) { r.x + r.w, r.y }; break;
        case ZJ_DOWN:  pos = (zvec2) { r.x, r.y + r.h }; break;
        case ZJ_UP:    pos = (zvec2) { r.x, r.y - s.h }; break;
        case ZJ_LEFT:  pos = (zvec2) { r.x - s.w, r.y }; break;
        case 0: break;
    }
    zindex += 1000;
    d->widget.used.pos   = pos;
    d->widget.bounds.pos = pos;
    d->widget.zindex     = zindex;
    _ui_pos(_ui_get_child(&d->widget), pos, zindex + 1);
}
void _zui_dropdown_draw(zw_dropdown *d) {
    if(!*d->state) return;
    if(*d->state == 1) {
        _ui_schedule_focus(&d->widget);
        *d->state = 3;
    } else if(!_ui_cont_focused(&d->widget) && (*d->state & 2)) {
        *d->state = 0;
        return;
    }
    _push_rect_cmd(d->widget.bounds, zui_stylec(d->cmd.id, ZSC_BACKGROUND), d->widget.zindex);
    _ui_draw(_ui_get_child(&d->widget));
}

void zui_surrogate(i32 id_offset) {
    zw_surrogate *s = _ui_alloc(ZW_SURROGATE, sizeof(zw_surrogate));
    s->id_offset = id_offset;
}
i16 _zui_surrogate_size(zw_surrogate *s, bool axis, i16 bound) {
    //zui_log("offset: %d\n", s->id_offset);
    zw_base *widget = _ui_widget(*(i32*)(s->id_offset + ctx->ui.data));
    if(widget->flags & ZF_CONTAINER) {
        zui_err(s, "Cannot make surrogate of container\n");
        return 0;
    }
    return _ui_sz(widget, axis, bound);
}
void _zui_surrogate_draw(zw_surrogate *s) {
    zw_base *other = _ui_widget(*(i32*)(s->id_offset + ctx->ui.data));
    if(other->flags & ZF_CONTAINER) return;
    zw_base backup = *other;
    other->zindex = s->widget.zindex;
    other->used = s->widget.used;
    other->bounds = s->widget.bounds;
    _ui_draw(other);
    *other = backup;
}

void zui_combo_select(zd_combo *state, i32 index) { state->index = index + 1; }
void zui_combo_wrap_child(void *wrapper_data) {
    zd_combo *state = wrapper_data;
    zui_radio_btn(&state->index, ++state->_);
}
i32 zui_combo(char *tooltip, zd_combo *state) {
    zw_combo *c = _cont_alloc(ZW_COMBO, sizeof(zw_combo));
    i32 id = _ui_index(&c->widget);
    c->state = state;
    zui_label(tooltip);
    i32 label_id = ctx->latest;
    zui_row(2, Z_FILL, Z_AUTO);
    c = (zw_combo*)_ui_widget(id);
    c->tooltip = label_id;
    zui_surrogate((u8*)&c->tooltip - ctx->ui.data);
    zui_label("\xE2\x96\xBC");
    zui_end();
    zui_dropdown_flags(ZF_END_PARENT | ZF_SELF_HEIGHT | ZJ_DOWN, &state->dropdown);
    ctx->flags |= ZF_END_PARENT | ZF_WRAPPER;
    ctx->wrapper_data = state;
    ctx->wrapper = zui_combo_wrap_child;
    zui_style(ZW_COL, ZSV_SPACING, (zvec2) { 0, 0 }, ZS_DONE);
    zui_col(Z_AUTO_ALL);
    state->_ = 0;
    return state->index - 1;
}
// create a combo box with comma-seperated options
i32 zui_combo_txt(char *tooltip, char *csoptions, zd_combo *state) {
    zui_combo(tooltip, state);
    char *i, *s = csoptions;
    i32 n = 1;
    do {
        i32 len = (i = strchr(s, ',')) ? i - s : strlen(s);
        zui_labeln(s, len);
    } while(i && (s = i + 1));
    zui_end();
    return state->index - 1;
}
i16  _zui_combo_size(zw_combo *c, bool axis, i16 bound) {
    zvec2 padding = zui_stylev(c->cmd.id, ZSV_PADDING);
    zw_base *top = _ui_nth_child(&c->widget, 1);
    zw_base *dd = _ui_next(top);
    if(c->state->index > 0) {
        zw_base *col = _ui_get_child(dd);
        zw_base *button = _ui_nth_child(col, c->state->index - 1);
        c->tooltip = _ui_index(_ui_get_child(button));
    }
    if(bound == Z_AUTO)
        bound = _ui_sz(top, axis, bound) + padding.e[axis] * 2;
    else
        _ui_sz(top, axis, bound - padding.e[axis] * 2);
    _ui_sz(dd, axis, bound);
    return bound;
}
void _zui_combo_pos(zw_combo *c, zvec2 pos, i32 zindex) {
    zvec2 padding = zui_stylev(c->cmd.id, ZSV_PADDING);
    zw_base *label = _ui_get_child(&c->widget);
    zw_base *top = _ui_next(label);
    zw_base *dd = _ui_next(top);
    _ui_pos(label, pos, zindex);
    _ui_pos(top, _vec_add(pos, padding), zindex);
    _ui_pos(dd, pos, zindex);
}
void _zui_combo_draw(zw_combo *c) {
    zw_layout *top = (zw_layout*)_ui_nth_child(&c->widget, 1);
    zw_dropdown *dd =  (zw_dropdown*)_ui_next(&top->widget);
    _push_rect_cmd(c->widget.bounds, zui_stylec(c->cmd.id, ZSC_BACKGROUND), c->widget.zindex);
    if(_ui_cont_hovered(&c->widget) && _ui_clicked(ZM_LEFT_CLICK) && _vec_within(_ui_mpos(), c->widget.bounds)) {
        *dd->state |= 1;
    }
    _ui_draw(&top->widget);
    _ui_draw(&dd->widget);
    if(_ui_cont_focused(&dd->widget) && !_ui_focused(&dd->widget))
        _ui_schedule_focus(&c->widget);
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

ZUI_PRIVATE i16 _zui_text_size(zw_text *data, bool axis, i16 bound) {
    if (!axis && bound != Z_AUTO) return bound;
    return zui_text_sz[axis](ctx->font_id, data->buffer, (i32)strlen(data->buffer)) + 10;
}

ZUI_PRIVATE i32 _zui_text_get_index(zw_text *data, i32 len) {
    zvec2 mp = ctx->mouse_pos;
    bool found = false;
    for (i32 i = 0; i < len; i++) {
        i32 w = zui_text_width(ctx->font_id, data->buffer, i + 1);
        w += 5 - data->state->ofs;
        //zrect r = { data->_.used.x, data->_.used.y, sz.x + 5, sz.y + 10 };
        if (mp.x >= data->widget.used.x && mp.x <= data->widget.used.x + w)
            return i;
    }
    if (!found && _vec_within(mp, data->widget.used))
        return len;
    return -1;
}

ZUI_PRIVATE void _zui_text_draw(zw_text *data) {
    zd_text tctx = *(zd_text*)data->state;
    i32 len = (i32)strlen(data->buffer);
    zcolor background = zui_stylec(ZW_TEXT, ZSC_BACKGROUND);
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
                tctx.ofs -= zui_text_width(ctx->font_id, tmp, 1);
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
    zvec2 sz = zui_text_vec(ctx->font_id, data->buffer, tctx.index);
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
    _push_rect_cmd(data->widget.used, background, data->widget.zindex);

    zvec2 selection = zui_text_vec(ctx->font_id, data->buffer + tctx.index, tctx.selection);
    zrect r = { textpos.x + sz.x, textpos.y, selection.x, selection.y };
    _push_rect_cmd(r, (zcolor) { 60, 60, 200, 255 }, data->widget.zindex);

    _push_text_cmd(ctx->font_id, textpos, (zcolor) { 250, 250, 250, 255 }, data->buffer, len, data->widget.zindex);

    if(_ui_widget(ctx->focused) == &data->widget) // draw cursor
        _push_rect_cmd(cursor, (zcolor) { 200, 200, 200, 255 }, data->widget.zindex);

    *data->state = tctx;
}

// creates a multi-line text input
void zui_textbox(char *buffer, i32 len, i32 *state);

ZUI_PRIVATE void _zui_layout(i32 id, i32 n, va_list sizes) {
    i32 cnt = n == Z_AUTO_ALL ? 0 : n;
    i32 bytes = sizeof(zw_layout) + cnt * sizeof(i16);
    zw_layout *l = _cont_alloc(id, bytes);
    l->count = n;
    for(i32 i = 0; i < cnt; i++)
        l->sizes[i] = va_arg(sizes, i32);
}

void zui_col(i32 n, ...) {
    va_list args;
    va_start(args, n);
    _zui_layout(ZW_COL, n, args);
    va_end(args);
}

void zui_row(i32 n, ...) {
    va_list args;
    va_start(args, n);
    _zui_layout(ZW_ROW, n, args);
    va_end(args);
}

ZUI_PRIVATE void _zui_layout_draw(zw_layout *data) {
    FOR_CHILDREN(data) _ui_draw(child);
}

ZUI_PRIVATE i16 _zui_layout_size(zw_layout *data, bool axis, i16 bound) {
    bool AXIS = data->widget.id - ZW_ROW;
    i16 used = 0;
    if(data->count != data->cont.children && data->count != Z_AUTO_ALL)
        zui_err(data, "expected %d children, has %d\n", data->count, data->cont.children);
    if(axis != AXIS) { // not primary axis of layout. IE: Row & Y axis
        FOR_CHILDREN(data) {
            i16 sz = _ui_sz(child, axis, bound);
            used = max(used, sz);
        }
        FOR_CHILDREN(data)
            child->bounds.sz.e[axis] = used;
        return used;
    }
    i32 i = 0, fillcnt = 0;
    zvec2 spacing = zui_stylev(data->cont.id, ZSV_SPACING);
    used = (data->cont.children - 1) * spacing.e[axis];
    if(data->count == Z_AUTO_ALL) {
        FOR_CHILDREN(data)
            used += _ui_sz(child, axis, Z_AUTO);
        return used;
    }
    FOR_CHILDREN(data) {
        if(data->sizes[i] >= 0)
            used += _ui_sz(child, axis, data->sizes[i]);
        else fillcnt++;
        i++;
    }
    if(!fillcnt) return used;
    i = 0;
    FOR_CHILDREN(data) {
        if(data->sizes[i++] >= 0) continue;
        i32 sz = bound == Z_AUTO ? Z_AUTO : (bound - used) / fillcnt;
        used += _ui_sz(child, axis, sz);
        fillcnt--;
    }
    return used;
}

ZUI_PRIVATE void _zui_layout_pos(zw_layout *data, zvec2 pos, i32 zindex) {
    zvec2 spacing = zui_stylev(data->cont.id, ZSV_SPACING);
    FOR_CHILDREN(data) {
        _ui_pos(child, pos, zindex);
        if(data->widget.id == ZW_COL)
            pos.y += spacing.y + child->bounds.h;
        else
            pos.x += spacing.x + child->bounds.w;
    }
}

ZUI_PRIVATE bool _zui_grid_propogate_auto_all(i16 *data, i32 len, va_list *args) {
    for(i32 i = 0; i < len; i++) {
        data[i] = (i16)va_arg(*args, i32);
        if(data[i] != Z_AUTO_ALL) continue;
        if(i != 0) {
            zui_log("Z_AUTO_ALL can only be at the start of the respective size list\n");
            return false;
        }
        while(i < len)
            data[i++] = Z_AUTO;
        break;
    }
    return true;
}
void zui_grid(i32 cols, i32 rows, ...) {
    va_list args;
    va_start(args, rows);
    i32 bytes = sizeof(zw_grid) + (cols + rows) * sizeof(i16);
    zw_grid *l = _cont_alloc(ZW_GRID, bytes);
    l->order = 1;
    l->cols = cols;
    l->rows = rows;
    _zui_grid_propogate_auto_all(l->data, cols, &args);
    _zui_grid_propogate_auto_all(l->data + cols, rows, &args);
    va_end(args);

}
ZUI_PRIVATE i16 _zui_grid_size(zw_grid *grid, bool axis, i16 bound) {
    zvec2 spacing = zui_stylev(grid->cont.id, ZSV_SPACING);
    i16 i = 0, cnt = axis ? grid->rows : grid->cols;
    i16 sz = spacing.e[axis] * (cnt - 1);
    i16 real_sizes[cnt];
    i16 *cfg_sizes = grid->data + (axis ? grid->cols : 0);
    memset(real_sizes, 0, cnt * sizeof(i16));
    if(bound == Z_AUTO) {
        FOR_CHILDREN(grid) {
            i32 n = axis ? i / grid->cols : i % grid->cols; i++;
            i16 csz = _ui_sz(child, axis, cfg_sizes[n] < 0 ? Z_AUTO : cfg_sizes[n]); // Z_FILL is treated as Z_AUTO
            if(csz > real_sizes[n])
                real_sizes[n] = csz;
        }
        for(i32 i = 0; i < cnt; i++)
            sz += cfg_sizes[i] = real_sizes[i];
        return sz;
    }
    FOR_CHILDREN(grid) { // calculate Z_AUTO and pixel sizes
        i32 n = axis ? i / grid->cols : i % grid->cols; i++;
        if(cfg_sizes[n] < 0) continue;
        i16 csz = _ui_sz(child, axis, cfg_sizes[n]);
        if(csz > real_sizes[n])
            real_sizes[n] = csz;
    }
    i32 left = bound - sz;
    i32 fill = 0;
    for(i32 i = 0; i < cnt; i++) { // calculate leftover size
        left -= real_sizes[i];
        if(cfg_sizes[i] == Z_FILL) fill++;
    }
    for(i32 i = 0; i < cnt; i++) {
        if(cfg_sizes[i] != Z_FILL)
            cfg_sizes[i] = real_sizes[i];
        else
            left -= cfg_sizes[i] = left / fill--;
    }
    i = 0;
    FOR_CHILDREN(grid) { // size Z_FILL
        i32 n = axis ? i / grid->cols : i % grid->cols; i++;
        if(cfg_sizes[n] != Z_FILL) continue;
        i16 csz = _ui_sz(child, axis, cfg_sizes[n]);
    }
    return bound;
}
ZUI_PRIVATE void _zui_grid_pos(zw_grid *grid, zvec2 pos, i32 zindex) {
    zvec2 spacing = zui_stylev(grid->cont.id, ZSV_SPACING);
    zvec2 cpos = pos;
    i32 i = 0;
    if(false) {
        FOR_CHILDREN(grid) {
            i16 w = grid->data[i % grid->cols];
            i16 h = grid->data[grid->cols + i / grid->cols];
            if(i % grid->rows == 0) cpos.y = pos.y;
            _ui_pos(child, cpos, zindex);
            cpos.y += h + spacing.y;
            if(++i % grid->rows == 0)
                cpos.x += w + spacing.x;
        }
    } else {
        FOR_CHILDREN(grid) {
            i16 w = grid->data[i % grid->cols];
            i16 h = grid->data[grid->cols + i / grid->cols];
            if(i % grid->cols == 0) cpos.x = pos.x;
            _ui_pos(child, cpos, zindex);
            cpos.x += w + spacing.x;
            if(++i % grid->cols == 0)
                cpos.y += h + spacing.y;
        }
    }
}

ZUI_PRIVATE void _zui_grid_draw(zw_grid *grid) {
    FOR_CHILDREN(grid)
        _ui_draw(child);
}

ZUI_PRIVATE bool _zui_cslen(char *cs, i32 *len) {
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
    i32 id = _ui_index(&l->widget);
    i32 cnt = 0;
    do {
        loop = _zui_cslen(cstabs, &len);
        zui_labeln(cstabs, len);
        //l->label_cnt++;
        cstabs += len + 1;
        cnt++;
    } while (loop);
    // if we use l directly, it's possible that adding a label causes a ui-buffer realloc.
    // this would invalidate l, and cause a use after free
    ((zw_tabset*)_ui_widget(id))->label_cnt = cnt;
}

ZUI_PRIVATE i16 _zui_tabset_size(zw_tabset *tabs, bool axis, i16 bound) {
    zvec2 padding = zui_stylev(ZW_TABSET, ZSV_PADDING);
    //zvec2 tab_size = { padding.x * 2 * tabs->label_cnt, 0 };
    i16 tab_size = axis ? 0 : padding.x * 2 * tabs->label_cnt;
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
    i16 child_sz = 0;
    i32 i = 0;
    FOR_SIBLINGS(tabs, label) {
        // widgets that aren't visible aren't capable of being focused / hovered
        if (i != *tabs->state && child->id != ZW_DROPDOWN)
            child->flags |= ZF_DISABLED;
        child_sz = max(child_sz, _ui_sz(child, axis, bound));
        i++;
    }
    if (axis) {
        if (bound == Z_AUTO) bound = max(tab_size, child_sz);
        bound += tabs->tabheight;
    }
    else if (bound == Z_AUTO) bound = tabs->tabheight + child_sz;
    return bound;
}

ZUI_PRIVATE void _zui_tabset_pos(zw_tabset *tabs, zvec2 pos) {
    zw_base *label = _ui_get_child(&tabs->widget);
    zvec2 padding = zui_stylev(ZW_TABSET, ZSV_PADDING);
    zvec2 tmp = { pos.x, pos.y + tabs->tabheight };
    i32 xs[tabs->label_cnt];
    pos.y += padding.y;
    for (i32 i = 0; i < tabs->label_cnt; i++) {
        xs[i] = pos.x;
        pos.x += padding.x;
        _ui_pos(label, pos, tabs->widget.zindex);
        pos.x += label->bounds.w + padding.x;
        label = _ui_next(label);
    }
    pos = tmp;
    i32 i = 0;
    FOR_SIBLINGS(tabs, label) {
        if(child->id == ZW_DROPDOWN)
            _ui_pos(child, (zvec2) { xs[i], pos.y }, tabs->widget.zindex);
        else
            _ui_pos(child, pos, tabs->widget.zindex);
        i++;
    }
}

ZUI_PRIVATE void _zui_tabset_draw(zw_tabset *tabs) {
    zrect bounds = tabs->widget.bounds;
    zvec2 padding = zui_stylev(ZW_TABSET, ZSV_PADDING);
    zcolor unfocused = zui_stylec(ZW_TABSET, ZSC_UNFOCUSED);
    zcolor background = zui_stylec(ZW_TABSET, ZSC_BACKGROUND);
    zcolor hovered = zui_stylec(ZW_TABSET, ZSC_HOVERED);
    i32 zindex = tabs->widget.zindex, new_state = -1;
    zw_base *label = _ui_get_child(&tabs->widget);
    FOR_N_SIBLINGS(tabs, label, tabs->label_cnt) {
        if(!_ui_cont_hovered(&tabs->widget)) continue;
        zrect tab_rect = _rect_pad(label->bounds, padding);
        if (_ui_clicked(ZM_LEFT_CLICK) && _vec_within(ctx->mouse_pos, tab_rect))
            new_state = i;
    }
    // draw dropdowns if any
    zw_base *tmp = label;
    FOR_N_SIBLINGS(tabs, tmp, tabs->label_cnt) {
        if(tmp->id == ZW_DROPDOWN) {
            if(i == new_state) {
                u8 *dropstate = ((zw_dropdown*)tmp)->state;
                *dropstate |= 1;
                //_ui_schedule_focus(tmp);
            }
            _ui_draw(tmp);
        } else if(i == new_state)
            *tabs->state = i;
    }
    label = _ui_get_child(&tabs->widget);
    _push_rect_cmd((zrect) { bounds.x, bounds.y, bounds.w, tabs->tabheight }, unfocused, zindex);
    FOR_N_SIBLINGS(tabs, label, tabs->label_cnt) {
        zrect tab_rect = _rect_pad(label->bounds, padding);
        if(i == *tabs->state)
            _push_rect_cmd(tab_rect, background, zindex);
        else if(_vec_within(ctx->mouse_pos, tab_rect) && _ui_cont_hovered(&tabs->widget))
            _push_rect_cmd(tab_rect, hovered, zindex);
        _ui_draw(label);
    }
    // skip to selected sibling
    FOR_N_SIBLINGS(tabs, label, *tabs->state);
    _ui_draw(label);
}

void zui_tick(bool blocking) {
    zcmd_any start = { .base = { blocking ? ZCMD_TICK_BLOCKING : ZCMD_TICK, sizeof(zcmd) } };
    ctx->renderer(&start, ctx->user_data);
}

void zui_redraw() {
    zcmd_any start = { .base = { ZCMD_REDRAW, sizeof(zcmd) } };
    ctx->renderer(&start, ctx->user_data);
}

void zui_init(zui_render_fn fn, zui_log_fn logger, void *user_data) {
    static zui_ctx global_ctx = { 0 };
    global_ctx.renderer = fn;
    global_ctx.log = logger;
    global_ctx.user_data = user_data;
    zbuf_init(&global_ctx.draw, 256, 8);
    zbuf_init(&global_ctx.ui, 256, 8);
    zbuf_init(&global_ctx.registry, 256, sizeof(void*));
    zbuf_init(&global_ctx.cont_stack, 256, sizeof(i32));
    zbuf_init(&global_ctx.zdeque, 256, sizeof(u64));
    zbuf_init(&global_ctx.text, 256, sizeof(char));
    zmap_init(&global_ctx.glyphs);
    zmap_init(&global_ctx.style);
    global_ctx.padding = (zvec2) { 15, 15 };
    global_ctx.latest = 0;
    ctx = &global_ctx;
    zui_register(ZW_BLANK, "blank", _zui_blank_size, 0, _zui_blank_draw);

    zui_register(ZW_WINDOW, "window", _zui_window_size, _zui_window_pos, _zui_box_draw);
    zui_default_style(ZW_WINDOW,
        ZSC_BACKGROUND, (zcolor) { 50, 50, 50, 255 },
        ZSV_PADDING, (zvec2) { 15, 15 },
        ZS_DONE);
    zui_register(ZW_POPUP, "popup", _zui_popup_size, _zui_popup_pos, _zui_popup_draw);
    zui_default_style(ZW_POPUP,
        ZSC_BACKGROUND, (zcolor) { 50, 50, 50, 255 },
        ZSV_PADDING, (zvec2) { 15, 15 },
        ZS_DONE);

    zui_register(ZW_BOX, "box", _zui_box_size, _zui_box_pos, _zui_box_draw);
    zui_default_style(ZW_BOX,
        ZSC_BACKGROUND, (zcolor) { 50, 50, 50, 255 },
        ZSV_PADDING, (zvec2) { 15, 15 },
        ZS_DONE);

    zui_register(ZW_LABEL, "label", _zui_label_size, 0, _zui_label_draw);
    zui_register(ZW_LABELF, "labelf", _zui_labelf_size, 0, _zui_labelf_draw);
    zui_default_style(ZW_LABEL, ZSC_FOREGROUND, (zcolor) { 250, 250, 250, 255 }, ZS_DONE);

    zui_register(ZW_COL, "column", _zui_layout_size, _zui_layout_pos, _zui_layout_draw);
    zui_default_style(ZW_COL,
        ZSV_SPACING, (zvec2) { 15, 15 },
        ZSV_PADDING, (zvec2) { 0, 0 },
        ZS_DONE);
    zui_register(ZW_ROW, "row", _zui_layout_size, _zui_layout_pos, _zui_layout_draw);
    zui_default_style(ZW_ROW,
        ZSV_SPACING, (zvec2) { 15, 15 },
        ZSV_PADDING, (zvec2) { 0, 0 },
        ZS_DONE);
    zui_register(ZW_BTN, "btn", _zui_box_size, _zui_box_pos, _zui_button_draw);
    zui_default_style(ZW_BTN, ZSV_PADDING, (zvec2) { 10, 5 }, ZS_DONE);

    zui_register(ZW_SCROLL, "scroll", _zui_scroll_size, _zui_scroll_pos, _zui_scroll_draw);

    zui_register(ZW_CHECK, "check", _zui_check_size, 0, _zui_check_draw);
    zui_register(ZW_TEXT, "text", _zui_text_size, 0, _zui_text_draw);
    zui_default_style(ZW_TEXT, ZSC_BACKGROUND, (zcolor) { 30, 30, 30, 255 }, ZS_DONE);

    zui_register(ZW_COMBO, "combo", _zui_combo_size, _zui_combo_pos, _zui_combo_draw);
    zui_default_style(ZW_COMBO,
        ZSV_PADDING, (zvec2) { 10, 5 },
        ZSC_BACKGROUND, (zcolor) { 70, 70, 70, 255 },
        ZS_DONE);
    //zui_register(ZW_COMBO_DROPDOWN, "combo dropdown", _zui_combo_dd_size, _zui_combo_dd_pos, _zui_combo_dd_draw);
    //zui_default_style(ZW_COMBO_DROPDOWN, ZSV_PADDING, (zvec2) { 10, 5 }, ZS_DONE);

    zui_register(ZW_DROPDOWN, "dropdown", _zui_dropdown_size, _zui_dropdown_pos, _zui_dropdown_draw);
    zui_default_style(ZW_DROPDOWN,
        ZSC_BACKGROUND, (zcolor) { 90, 90, 90, 255 }, ZS_DONE);

    zui_register(ZW_GRID, "grid", _zui_grid_size, _zui_grid_pos, _zui_grid_draw);
    zui_default_style(ZW_GRID,
        ZSC_BACKGROUND, (zcolor) { 30, 30, 30, 255 },
        ZSC_FOREGROUND, (zcolor) { 150, 150, 150, 255 }, // border color
        ZSV_PADDING,    (zvec2)  { 15, 15 },
        ZSV_SPACING,    (zvec2)  { 15, 15 },
        ZSV_BORDER,     (zvec2)  { 0, 0 },
        ZS_DONE);

    zui_register(ZW_TABSET, "tabset", _zui_tabset_size, _zui_tabset_pos, _zui_tabset_draw);
    zui_default_style(ZW_TABSET,
        ZSC_UNFOCUSED,  (zcolor) { 30, 30, 30, 255 },
        ZSC_BACKGROUND, (zcolor) { 50, 50, 50, 255 },
        ZSC_HOVERED,    (zcolor) { 40, 40, 40, 255 },
        ZSV_PADDING,    (zvec2)  { 15, 5 },
        ZS_DONE);

    zui_register(ZW_SURROGATE, "surrogate", _zui_surrogate_size, 0, _zui_surrogate_draw);

    ctx->next_wid = ZW_LAST;
    ctx->next_sid = ZS_LAST;

    zcmd_any start = { .base = { ZCMD_INIT, sizeof(zcmd) } };
    fn(&start, user_data);
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
