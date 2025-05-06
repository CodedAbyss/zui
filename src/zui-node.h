#include <string.h>
#define ZUI_BUF
#define ZUI_DEV
#include "zui.h"
#include <stdlib.h>

static i32 ZW_NODE_EDITOR;

typedef struct zui_pool {
    zui_buf buf;
    i32 head;
    i32 element_size;
} zui_pool;

void zpool_init(zui_pool *p, i32 cap, i32 element_size) {
    zbuf_init(&p->buf, cap, sizeof(void*));
    p->head = 0;
    p->element_size = zbuf_align(&p->buf, element_size);
    for(i32 i = 0; i < cap; i += p->element_size)
        *(i32*)(p->buf.data + i) = i + p->element_size;
}

void *zpool_alloc(zui_pool *p) {
    p->buf.used += p->element_size;
    zbuf_resize(&p->buf);
    p->head = *(i32*)(p->buf.data + p->head);
    return p->buf.data + p->head;
}

void zpool_free(zui_pool *p, void *ptr) {
    p->buf.used -= p->element_size;
    *(i32*)ptr = p->head;
    p->head = (u8*)ptr - p->buf.data;
}


typedef struct zd_node {
    zvec2 pos;
    u16 inputs, outputs;
    zcolor color;
    char title[32];
    struct zd_node *next;
} zd_node;

typedef struct zd_node_conn {
    zd_node *A, *B; 
    u16 input, output;
    struct zd_node_conn *next;
} zd_node_conn;

typedef struct zd_node_editor {
    zvec2 offset;
    zd_node *head_node;
    zd_node_conn *head_conn;
} zd_node_editor;

typedef struct zw_node_editor { Z_WIDGET; zd_node_editor *state; } zw_node_editor;

void znode_add(zd_node_editor *state, i32 inputs, i32 outputs, char *title, zcolor color) {
    zd_node *new = malloc(sizeof(zd_node));
    *new = (zd_node) {
        .pos = { 0, 0 },
        .inputs = inputs,
        .outputs = outputs,
        .color = color,
        .next = state->head_node,
    };
    memcpy(&new->title, title, 31);
    new->title[31] = 0; 
    state->head_node = new;
}

void znode_connect(zd_node_editor *state, zd_node *A, u16 input, zd_node *B, u16 output) {
    zd_node_conn *new = malloc(sizeof(zd_node_conn));
    *new = (zd_node_conn) {
        .A = A, .B = B,
        .input = input,
        .output = output,
        .next = state->head_conn,
    };
    state->head_conn = new;
}

void zui_node_editor(zd_node_editor *state) {
    zw_node_editor *editor = _ui_alloc(ZW_NODE_EDITOR, sizeof(zw_node_editor));
    editor->state = state;
}

static u16 _znode_editor_size(zw_node_editor *w, bool axis, u32 bound) { return bound; }
static void _znode_editor_draw(zw_node_editor *w) {

}

void zui_node_register() {
    ZW_NODE_EDITOR = zui_new_id();
    zui_register(ZW_NODE_EDITOR, _znode_editor_size, 0, _znode_editor_draw);
}
