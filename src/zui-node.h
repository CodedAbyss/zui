#ifndef ZNODE_INCLUDED
#define ZNODE_INCLUDED
#include "zui.h"

#define FOR_NODES(editor) for(zd_node *node = (editor)->head_node; node; node = node->next)

typedef struct zd_node {
    zrect rect;
    void *uud;
    i32 flags;
    i16 inputs, outputs;
    struct zd_node *next;
} zd_node;

typedef struct zd_node_link {
    zd_node *A, *B; 
    i16 input, output;
    struct zd_node_link *next;
} zd_node_link;

typedef struct zd_node_editor {
    zvec2 offset;
    i32 drag_state;
    i32 node_cnt;
    zd_node *dragged;
    zd_node *head_node;
    zd_node_link *head_link;
} zd_node_editor;

typedef struct zw_node_editor { Z_CONT; zd_node_editor *state; } zw_node_editor;

enum {
    ZF_NODE_1IN = 1,
    ZF_NODE_1OUT = 2,
};

extern i32 ZW_NODE_EDITOR;
extern i32 ZSI_NODE_CONN_SPACING;
extern i32 ZSC_NODE_OUTPUT;
extern i32 ZSC_NODE_FOUTPUT;
extern i32 ZSC_NODE_INPUT;
extern i32 ZSC_NODE_FINPUT;

i32 znode_del_links(zd_node_editor *state, zd_node *node, i32 flags);
i32 znode_cnt_links(zd_node_editor *state, zd_node *node, i32 flags);
zd_node *znode_get(zd_node_editor *state, void *uud);
zd_node *znode_add(zd_node_editor *state, void *uud, zvec2 pos, i32 inputs, i32 outputs, i32 flags);
bool znode_del(zd_node_editor *state, void *uud);

bool znode_has_link(zd_node_editor *state, zd_node *input, zd_node *output);
bool znode_link(zd_node_editor *state, zd_node *input, i32 in_index, zd_node *output, i32 out_index);
bool znode_del_link(zd_node_editor *state, zd_node *input, zd_node *output);
void zui_node_editor(zd_node_editor *state);
void zui_node_register();

#endif
