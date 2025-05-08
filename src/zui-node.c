#define ZUI_BUF
#define ZUI_UTF8
#define ZUI_DEV
#include "zui.h"
#include "zui-node.h"
#include <string.h>
#include <stdlib.h>

i32 ZW_NODE_EDITOR;

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

zd_node *znode_get(zd_node_editor *state, void *uud) {
    FOR_NODES(state)
        if(node->uud == uud)
           return node;
    return 0;
}

zd_node *znode_add(zd_node_editor *state, void *uud, zvec2 pos, i32 flags) {
    zd_node *new = malloc(sizeof(zd_node));
    *new = (zd_node) {
        .rect.pos = pos,
        .uud = uud,
        .flags = flags,
        .next = state->head_node,
    };
    state->head_node = new;
    return new;
}

i32 znode_cnt_links(zd_node_editor *state, zd_node *node, i32 flags) { 
    i32 cnt = 0;
    for(zd_node_link **p = &state->head_link, *l = *p; l; p = &l->next, l = *p) {
        if((l->A == node && (flags & ZF_NODE_IN)) ||
            (l->B == node && (flags & ZF_NODE_OUT))) {
            cnt++;
        }
    }
    return cnt;
}

i32 znode_del_links(zd_node_editor *state, zd_node *node, i32 flags) { 
    i32 cnt = 0;
    for(zd_node_link **p = &state->head_link, *l = *p; l; l = *p) {
        if((l->A == node && (flags & ZF_NODE_IN)) ||
            (l->B == node && (flags & ZF_NODE_OUT))) {
            *p = l->next;
            free(l);
            cnt++;
        } else p = &l->next;
    }
    return cnt;
}

bool znode_del(zd_node_editor *state, void *uud) { 
    for(zd_node **p = &state->head_node, *n = *p; n; p = &n->next, n = *p) {
        if(n->uud != uud) continue;
        *p = n->next;
        znode_del_links(state, n, ZF_NODE_ALL);
        free(n);
        return true;
    }
    return false;
}

bool znode_has_link(zd_node_editor *state, zd_node *input, zd_node *output) {
    for(zd_node_link *l = state->head_link; l; l = l->next)
        if(l->A == input && l->B == output)
            return true;
    return false;
}

bool znode_link(zd_node_editor *state, zd_node *input, zd_node *output) {
    if(~input->flags & ZF_NODE_IN || ~output->flags & ZF_NODE_OUT) return false;
    for(zd_node_link **p = &state->head_link, *l = *p; l; p = &l->next, l = *p) {
        if((l->A == input) && (input->flags & ZF_NODE_1IN)) return false;
        if((l->B == output) && (output->flags & ZF_NODE_1OUT)) return false;
        if((l->A == input) && (l->B == output)) return false;
    }

    zd_node_link *new = malloc(sizeof(zd_node_link));
    *new = (zd_node_link) {
        .A = input, .B = output,
        .next = state->head_link,
    };
    state->head_link = new;
    return true;
}

bool znode_del_link(zd_node_editor *state, zd_node *input, zd_node *output) {
    for(zd_node_link **p = &state->head_link, *l = *p; l; p = &l->next, l = *p) {
        if(l->A != input || l->B != output) continue;
        *p = l->next;
        free(l);
        return true;
    }
    return false;
}

//void znode_del(zd_node_editor *state

void zui_node_editor(zd_node_editor *state) {
    zw_node_editor *editor = _cont_alloc(ZW_NODE_EDITOR, sizeof(zw_node_editor));
    editor->state = state;
}

static void _miscount_error() {
    zui_log("Number of children does not match number of nodes\n");
    exit(0);
}
static u16 _znode_editor_size(zw_node_editor *w, bool axis, i16 bound) {
    zd_node *n = w->state->head_node;
    FOR_CHILDREN(w) {
        if(!n) _miscount_error();  
        n->rect.e[2 + axis] = _ui_sz(child, axis, Z_AUTO);
        n = n->next;
    }
    return bound;
}
static void _znode_editor_pos(zw_node_editor *w, zvec2 pos, i32 zindex) {
    zd_node *n = w->state->head_node;
    zvec2 origin = _vec_add(pos, w->state->offset);
    FOR_CHILDREN(w) { 
        if(!n) _miscount_error();
        zvec2 p = _vec_add(origin, n->rect.pos);
        _ui_pos(child, p, zindex);
        n = n->next; 
    }
}
static void _draw_connection(zvec2 start, zvec2 end, bool direction, i32 zindex) {
    if(direction) {
        zvec2 points[4] = {
            { start.x, start.y },
            { end.x, start.y },
            { start.x, end.y },
            { end.x, end.y } };
        _push_bezier_cmd(4, points, 2, (zcolor) { 255, 255, 255, 255 }, zindex);
    } else {
        zvec2 mid = { (end.x + start.x) / 2, (end.y + start.y) / 2 };
        i32 c = (end.x - start.x) / 4;
        zvec2 points[7] = {
            { start.x, start.y },
            { start.x - c, start.y },
            { start.x - c, mid.y },
            { mid.x, mid.y },
            { end.x + c, mid.y },
            { end.x + c, end.y },
            { end.x, end.y },
        };
        _push_bezier_cmd(7, points, 2, (zcolor) { 255, 255, 255, 255 }, zindex);
    }

}

static void _znode_editor_draw(zw_node_editor *w) {
    _push_rect_cmd(w->widget.used, (zcolor) { 30, 30, 30, 255 }, w->widget.zindex);
    zd_node_link *c = w->state->head_link;
    zvec2 origin = _vec_add(w->widget.used.pos, w->state->offset);
    while(c) {
        zrect start = c->A->rect;
        zrect end = c->B->rect;
        start.pos = _vec_add(_vec_add(origin, start.pos), (zvec2) { -10, start.h / 2 });
        end.pos = _vec_add(_vec_add(origin, end.pos), (zvec2) { end.w + 10, end.h / 2 });
        _draw_connection(start.pos, end.pos, start.x > end.x, w->widget.zindex);
        c = c->next;
    }

    zd_node *n = w->state->head_node;
    zd_node *scheduled_link_delete = 0;
    i32 scheduled_link_delete_flags = 0;

    FOR_CHILDREN(w) { 
        if(!n) _miscount_error();
        zrect r = { 0 };
        if(!_rect_intersect(_rect_pad(child->used, (zvec2) { 10, 10 }), w->widget.used, &r)) {
            n = n->next;
            continue;
        }
        if(_ui_clicked(ZM_LEFT_CLICK) && _vec_within(_ui_mpos(), r))
            w->state->dragged = n;

        zvec2 input = (zvec2) { r.x, r.y + r.h / 2 };
        zvec2 output = (zvec2) { r.x + r.w, input.y };

        _push_rect_cmd(r, (zcolor) { 70, 70, 70, 255 }, child->zindex); 

        zvec2 p1 = { input.x - 3, input.y - 9 };
        zvec2 p2 = { output.x - 6, output.y - 9 };
        zcolor input_color = { 50, 255, 50, 255 };
        zcolor input_hicolor = { 150, 255, 150, 255 };

        zcolor output_color = { 200, 200, 50, 255 };
        zcolor output_hicolor = { 255, 255, 150, 255 };

        if(n->flags & ZF_NODE_OUT) {
            if(_vec_distsq(_ui_mpos(), output) < 100) {
                _push_text_cmd(0, p2, output_hicolor, "\xE2\x97\x8F", 3, child->zindex);
                if(_ui_clicked(ZM_RIGHT_CLICK)) {
                    w->state->drag_state = 0;
                    w->state->dragged = 0;
                    scheduled_link_delete = n;
                    scheduled_link_delete_flags = ZF_NODE_OUT;
                }
                else if(_ui_clicked(ZM_LEFT_CLICK)) {
                    w->state->drag_state = 2;
                    w->state->dragged = n;
                } else if(_ui_released(ZM_LEFT_CLICK) && w->state->drag_state == 1) {
                    znode_link(w->state, w->state->dragged, n);
                }
            } else
                _push_text_cmd(0, p2, output_color, "\xE2\x97\x8F", 3, child->zindex);
        }

        if(n->flags & ZF_NODE_IN) {
            if(_vec_distsq(_ui_mpos(), input) < 100) {
                _push_text_cmd(0, p1, input_hicolor, "\xE2\x97\x8F", 3, child->zindex);
                if(_ui_clicked(ZM_RIGHT_CLICK)) {
                    w->state->drag_state = 0;
                    w->state->dragged = 0;
                    scheduled_link_delete = n;
                    scheduled_link_delete_flags = ZF_NODE_IN;
                }
                else if(_ui_clicked(ZM_LEFT_CLICK)) {
                    w->state->drag_state = 1;
                    w->state->dragged = n;
                } else if(_ui_released(ZM_LEFT_CLICK) && w->state->drag_state == 2) {
                    znode_link(w->state, n, w->state->dragged);
                }
            } else
                _push_text_cmd(0, p1, input_color, "\xE2\x97\x8F", 3, child->zindex);
        }

        _ui_draw(child);
        n = n->next;
    }

    if(scheduled_link_delete) {
        znode_del_links(w->state, scheduled_link_delete, scheduled_link_delete_flags);
    }

    n = w->state->dragged;
    if(!n) return;
    zvec2 end = _vec_add(origin, n->rect.pos);
    end.y += n->rect.h / 2;
    if(_ui_pressed(ZM_LEFT_CLICK)) {
        if(w->state->drag_state == 0)
            n->rect.pos = _vec_add(n->rect.pos, _ui_mdelta());
        if(w->state->drag_state != 0) {
            bool left = w->state->drag_state == 1;
            zvec2 start = _ui_mpos();
            end.x += (left ? -10 : n->rect.w + 10);
            _draw_connection(start, end, (start.x > end.x) ^ left, w->widget.zindex);
        }
    } else {
        w->state->dragged = 0;
        w->state->drag_state = 0;
    }
}

void zui_node_register() {
    ZW_NODE_EDITOR = zui_new_id();
    zui_register(ZW_NODE_EDITOR, _znode_editor_size, _znode_editor_pos, _znode_editor_draw);
}
