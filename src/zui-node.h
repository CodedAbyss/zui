#ifndef ZNODE_INCLUDED
#define ZNODE_INCLUDED
#include "zui.h"

#define FOR_NODES(editor) for(zd_node *node = (editor)->head_node; node; node = node->next)
#define FOR_INPUTS(node) for(zd_node_link *link = (node)->inputs; link; link = link->next_in)
#define FOR_OUTPUTS(node) for(zd_node_link *link = (node)->outputs; link; link = link->next_out)


typedef struct zd_node_link {
    struct zd_node *output, *input;
    i32 id_in, id_out;
    struct zd_node_link *next;
	struct zd_node_link *next_in;
	struct zd_node_link *next_out;
} zd_node_link;

typedef struct zd_node {
    zrect rect;
    void *uud;
    i32 uud_type;
    i32 flags;
    i32 cnt_in, cnt_out;
	zd_node_link *inputs;
	zd_node_link *outputs;
    struct zd_node *next;
} zd_node;

typedef struct zd_node_editor {
    zvec2 offset;
    i32 drag_state;
    i32 node_cnt;
    i32 link_cnt;
    bool has_updated;
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

i32 znode_toposort(zd_node_editor *state, zd_node **arr);

zd_node *znode_get(zd_node_editor *state, void *uud, i32 uud_type);
zd_node *znode_add(zd_node_editor *state, void *uud, i32 uud_type, zvec2 pos, i32 inputs, i32 outputs, i32 flags);

bool znode_del_out_links(zd_node_editor *state, zd_node *node, i32 id_out);
bool znode_del_in_links(zd_node_editor *state, zd_node *node, i32 id_in);
bool znode_del(zd_node_editor *state, void *uud, i32 uud_type);

bool znode_link(zd_node_editor *state, zd_node *output, i32 out_index, zd_node *input, i32 in_index);
void zui_node_editor(zd_node_editor *state);
bool znode_updated(zd_node_editor *state);
void zui_node_register();

#endif
