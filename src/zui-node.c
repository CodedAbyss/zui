#include "stdbool.h"
#define ZUI_BUF
#define ZUI_UTF8
#define ZUI_DEV
#include "zui.h"
#include "zui-node.h"
#include <stdlib.h>

#define ID ZW_NODE_EDITOR

i32 ZW_NODE_EDITOR;
i32 ZSI_NODE_CONN_SPACING;
i32 ZSC_NODE_OUTPUT;
i32 ZSC_NODE_FOUTPUT;
i32 ZSC_NODE_INPUT;
i32 ZSC_NODE_FINPUT;

zd_node *znode_get(zd_node_editor *state, void *uud) {
    FOR_NODES(state)
        if(node->uud == uud)
           return node;
    return 0;
}

zd_node *znode_add(zd_node_editor *state, void *uud, zvec2 pos, i32 inputs, i32 outputs, i32 flags) {
    zd_node *new = malloc(sizeof(zd_node));
    *new = (zd_node) {
        .rect.pos = pos,
        .uud = uud,
        .flags = flags,
        .inputs = inputs,
        .outputs = outputs,
        .next = state->head_node,
    };
    state->node_cnt++;
    state->head_node = new;
    return new;
}

i32 znode_del_links(zd_node_editor *state, zd_node *node, i32 index) { 
    i32 cnt = 0;
    for(zd_node_link **p = &state->head_link, *l = *p; l; l = *p) {
        if((l->A == node && (!index || (l->input == -index - 1))) ||
           (l->B == node && (!index || (l->output == index - 1)))) {
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
        znode_del_links(state, n, 0);
        free(n);
        state->node_cnt--;
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

bool znode_link(zd_node_editor *state, zd_node *input, i32 in_index, zd_node *output, i32 out_index) {
    if(!input || !output
        || in_index < 0 || in_index >= input->inputs
        || out_index < 0 || out_index >= output->outputs) return false;
    for(zd_node_link **p = &state->head_link, *l = *p; l; p = &l->next, l = *p) {
        if((l->A == input) && (l->input == in_index)
            && (input->flags & ZF_NODE_1IN)) return false;
        if((l->B == output) && (l->output == out_index)
            && (output->flags & ZF_NODE_1OUT)) return false;
        if((l->A == input) && (l->B == output)
            && (l->input == in_index) && (l->output == out_index)) return false;
    }
    zd_node_link *new = malloc(sizeof(zd_node_link));
    *new = (zd_node_link) {
        .A = input, .B = output,
        .input = in_index, .output = out_index,
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

void zui_node_editor(zd_node_editor *state) {
    zw_node_editor *editor = _cont_alloc(ZW_NODE_EDITOR, sizeof(zw_node_editor));
    editor->state = state;
}

static void _error_if_miscount(zw_node_editor *w) {
    if(w->cont.children == w->state->node_cnt) return;
    zui_log("Number of children does not match number of nodes. %d vs %d\n", w->cont.children, w->state->node_cnt);
    exit(0);
}
static u16 _znode_editor_size(zw_node_editor *w, bool axis, i16 bound) {
    zd_node *n = w->state->head_node;
    _error_if_miscount(w);
    FOR_CHILDREN(w) {
        n->rect.e[2 + axis] = _ui_sz(child, axis, Z_AUTO);
        n = n->next;
    }
    return bound;
}
static void _znode_editor_pos(zw_node_editor *w, zvec2 pos, i32 zindex) {
    zd_node *n = w->state->head_node;
    zvec2 origin = _vec_add(pos, w->state->offset);
    _error_if_miscount(w);
    FOR_CHILDREN(w) { 
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

static i32 _draw_connection_points(zd_node *n, zrect r, bool input, i32 zindex, zcolor colors[2]) {
    i32 cnt = input ? n->inputs : n->outputs;
    if(!cnt) return 0;
    zvec2 draw_offset = { input ? -3 : -5, -9 };
    if(!input) r.pos.x += r.w;
    i32 index_hovered = 0;
    for(i32 i = 0; i < cnt; i++) {
        zvec2 pos = { r.pos.x, r.pos.y + r.h / (cnt * 2) * (2 * i + 1) };
        bool is_hovered = _vec_distsq(_ui_mpos(), pos) < 100;
        _push_text_cmd(0, _vec_add(pos, draw_offset), colors[is_hovered], "\xE2\x97\x8F", 3, zindex);
        if(is_hovered) index_hovered = i + 1;
    }
    return input ? -index_hovered : index_hovered;
}

static zrect _znode_padded_rect(zd_node *n, zvec2 origin, zvec2 padding, i32 min_conn_space) {
    i32 maxcnt = n->inputs > n->outputs ? n->inputs : n->outputs;
    i32 ypad = (min_conn_space * maxcnt - n->rect.h) / 2;
    if(ypad < padding.y) ypad = padding.y;
    zrect rect = _rect_pad(n->rect, (zvec2) { padding.x, ypad });
    rect.pos = _vec_add(rect.pos, origin);
    return rect;
}


static void _znode_editor_draw(zw_node_editor *w) {
    _error_if_miscount(w);

    zcolor background = zui_stylec(ID, ZSC_BACKGROUND);
    zcolor foreground = zui_stylec(ID, ZSC_FOREGROUND);

    zvec2 origin = _vec_add(w->widget.used.pos, w->state->offset);

    zvec2 padding = zui_stylev(ID, ZSV_PADDING);
    i32 min_conn_space = zui_stylei(ID, ZSI_NODE_CONN_SPACING);

    zcolor in_colors[2] = {
        zui_stylec(ID, ZSC_NODE_INPUT),
        zui_stylec(ID, ZSC_NODE_FINPUT),
    };
    zcolor out_colors[2] = {
        zui_stylec(ID, ZSC_NODE_OUTPUT),
        zui_stylec(ID, ZSC_NODE_FOUTPUT),
    };

    _push_rect_cmd(w->widget.used, background, w->widget.zindex);
    for(zd_node_link *c = w->state->head_link; c; c = c->next) {
        zrect start = _znode_padded_rect(c->A, origin, padding, min_conn_space);
        zrect end   = _znode_padded_rect(c->B, origin, padding, min_conn_space);
        start.y += start.h / (2 * c->A->inputs) * (2 * c->input + 1);
        end.y += end.h / (2 * c->B->outputs) * (2 * c->output + 1);
        end.x += end.w;
        _draw_connection(start.pos, end.pos, start.x > end.x, w->widget.zindex);
    }
    zrect selected_rect;
    zd_node *n = w->state->head_node;
    FOR_CHILDREN(w) { 
        zrect rect = _znode_padded_rect(n, origin, padding, min_conn_space);
        if(_ui_clicked(ZM_LEFT_CLICK) && _vec_within(_ui_mpos(), rect))
            w->state->dragged = n;
        _push_rect_cmd(rect, foreground, child->zindex); 
        // 0 => nothing hovered, index<=-1 => input(1-index), index>=1 => output(index-1)
        i32 index = _draw_connection_points(n, rect, true, w->widget.zindex, in_colors)
            + _draw_connection_points(n, rect, false, w->widget.zindex, out_colors);
        if(index != 0) { // a connection point is hovered
            if(_ui_clicked(ZM_LEFT_CLICK)) {
                w->state->drag_state = index; 
                w->state->dragged = n;
            } else if(_ui_clicked(ZM_RIGHT_CLICK)) {
                znode_del_links(w->state, n, index);
            } else if(_ui_released(ZM_LEFT_CLICK) && (index ^ w->state->drag_state) < 0) { 
                if(index > 0) {
                    znode_link(w->state, w->state->dragged, -w->state->drag_state - 1, n, index - 1);
                } else {
                    znode_link(w->state, n, -index - 1, w->state->dragged, w->state->drag_state - 1); 
                }
            }
        }
        if(w->state->dragged == n) selected_rect = rect;
        _ui_draw(child);
        n = n->next;
    }

    n = w->state->dragged;
    if(!n) {
        if(_ui_cont_focused(&w->widget) && _ui_dragged(ZM_LEFT_CLICK)) {
            w->state->offset = _vec_add(w->state->offset, _ui_mdelta());
        }
        return;
    } 
    i32 index = w->state->drag_state;
    if(_ui_pressed(ZM_LEFT_CLICK)) {
        if(w->state->drag_state == 0)
            n->rect.pos = _vec_add(n->rect.pos, _ui_mdelta());
        if(w->state->drag_state != 0) {
            zvec2 start = _ui_mpos();
            zvec2 end = selected_rect.pos;
            i32 cnt = index < 0 ? n->inputs : n->outputs;
            if(index > 0) end.x += selected_rect.w;
            end.y += selected_rect.h / (2 * cnt) * (2 * abs(index) - 1);
            _draw_connection(start, end, (start.x > end.x) ^ (index < 0), w->widget.zindex);
        }
    } else {
        w->state->dragged = 0;
        w->state->drag_state = 0;
    }
}

void zui_node_register() {
    ZW_NODE_EDITOR        = zui_new_wid();
    ZSI_NODE_CONN_SPACING = zui_new_sid();
    ZSC_NODE_OUTPUT       = zui_new_sid();
    ZSC_NODE_FOUTPUT      = zui_new_sid();
    ZSC_NODE_INPUT        = zui_new_sid();
    ZSC_NODE_FINPUT       = zui_new_sid();
    zui_register(ZW_NODE_EDITOR, "node editor", _znode_editor_size, _znode_editor_pos, _znode_editor_draw);
    zui_default_style(ZW_NODE_EDITOR,
        ZSV_PADDING, (zvec2) { 10, 10 },
        ZSI_NODE_CONN_SPACING, 20,
        ZSC_BACKGROUND, (zcolor) { 30, 30, 30, 255 },
        ZSC_FOREGROUND, (zcolor) { 70, 70, 70, 255 },
        ZSV_PADDING, (zvec2) { 10, 10 },
        ZSC_NODE_INPUT, (zcolor) { 50, 255, 50, 255 },
        ZSC_NODE_FINPUT, (zcolor) { 150, 255, 150, 255 },
        ZSC_NODE_OUTPUT, (zcolor) { 200, 200, 50, 255 },
        ZSC_NODE_FOUTPUT, (zcolor) { 255, 255, 150, 255 },
        ZS_DONE);
}
