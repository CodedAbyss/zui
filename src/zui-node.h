#ifndef ZNODE_INCLUDED
#define ZNODE_INCLUDED
#include "zui.h"

#define FOR_NODES(editor) for(zd_node *node = (editor)->head_node; node; node = node->next)

typedef struct zd_node {
    zrect rect;
    void *uud;
    i32 flags;
    struct zd_node *next;
} zd_node;

typedef struct zd_node_link {
    zd_node *A, *B; 
    struct zd_node_link *next;
} zd_node_link;

typedef struct zd_node_editor {
    zvec2 offset;
    i32 drag_state;
    zd_node *dragged;
    zd_node *head_node;
    zd_node_link *head_link;
} zd_node_editor;

typedef struct zw_node_editor { Z_CONT; zd_node_editor *state; } zw_node_editor;

enum {
    ZF_NODE_OUT = 1,
    ZF_NODE_IN = 2,
    ZF_NODE_ALL = 3,
    ZF_NODE_1IN = 4,
    ZF_NODE_1OUT = 8,
};

i32 znode_del_links(zd_node_editor *state, zd_node *node, i32 flags);
i32 znode_cnt_links(zd_node_editor *state, zd_node *node, i32 flags);
zd_node *znode_get(zd_node_editor *state, void *uud);
zd_node *znode_add(zd_node_editor *state, void *uud, zvec2 pos, i32 flags);
bool znode_del(zd_node_editor *state, void *uud);

bool znode_has_link(zd_node_editor *state, zd_node *input, zd_node *output);
bool znode_link(zd_node_editor *state, zd_node *input, zd_node *output);
bool znode_del_link(zd_node_editor *state, zd_node *input, zd_node *output);
void zui_node_editor(zd_node_editor *state);
void zui_node_register();

#endif
