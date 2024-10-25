//------------------------------------------------------------------------------
//  sgl-microui.c
//
//  https://github.com/rxi/microui sample using sokol_gl.h, sokol_gfx.h
//  and sokol_app.h
//
//  NOTE: for the debugging UI, cimgui is used via sokol_gfx_cimgui.h
//  (C bindings to Dear ImGui instead of the ImGui C++ API)
//------------------------------------------------------------------------------
#include <IOKit/IOTypes.h>
#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS (1)
#endif
#include <stdlib.h>
#include "sokol_gfx.h"
#include "sokol_app.h"
#include "sokol_log.h"
#include "sokol_glue.h"
#include "font.h"
#include "zui.h"
#include "sokol_gl.h"
#include "sokol_time.h"
#include <stdio.h> // sprintf

static sg_image atlas_img;
static sg_sampler atlas_smp;
static sgl_pipeline pip;

static void z_push_quad(zrect dst, zrect src, i32 zindex, zcolor color) {
    float u0 = (float) src.x / (float) ATLAS_WIDTH;
    float v0 = (float) src.y / (float) ATLAS_HEIGHT;
    float u1 = (float) (src.x + src.w) / (float) ATLAS_WIDTH;
    float v1 = (float) (src.y + src.h) / (float) ATLAS_HEIGHT;

    float x0 = (float) dst.x;
    float y0 = (float) dst.y;
    float x1 = (float) (dst.x + dst.w);
    float y1 = (float) (dst.y + dst.h);

    float z = zindex * 0.0001f - 1.0f; // - 1.0f; // -zindex * 0.0001f;
    //printf("%d\n", zindex);

    sgl_c4b(color.r, color.g, color.b, color.a);
    sgl_v3f_t2f(x0, y0, z, u0, v0);
    sgl_v3f_t2f(x1, y0, z, u1, v0);
    sgl_v3f_t2f(x1, y1, z, u1, v1);
    sgl_v3f_t2f(x0, y1, z, u0, v1);
}

static void z_draw_rect(zrect rect, zcolor color, i32 z) {
    atlasrect box;
    u8 ofs;
    atlas_chardata(0, &box, &ofs);
    z_push_quad(rect, (zrect) { box.x0, box.y0, box.x1 - box.x0, box.y1 - box.y0 }, z, color);
}

static void z_draw_text(const char* text, i32 len, zvec2 pos, zcolor color, i32 z) {
    atlasrect box;
    u8 ofs;
    for(i32 i = 0; i < len; i++) {
        atlas_chardata(text[i], &box, &ofs);
        zrect src = { box.x0, box.y0, box.x1 - box.x0, box.y1 - box.y0 };
        zrect dst = { pos.x + (FONT_WIDTH - src.w) / 2, pos.y + ofs, src.w, src.h };
        z_push_quad(dst, src, z, color);
        pos.x += FONT_WIDTH;
    }
}

static void z_set_clip_rect(zrect rect) {
    sgl_end();
    sgl_scissor_rect(rect.x, rect.y, rect.w, rect.h, true);
    sgl_begin_quads();
}

// initialization
void init(void) {
    // setup sokol-gfx
    sg_setup(&(sg_desc){
        .environment = sglue_environment(),
        .logger.func = slog_func,
    });

    // setup sokol-gl
    sgl_setup(&(sgl_desc_t){
        .logger.func = slog_func,
    });

    // atlas image data is in atlas.inl file, this only contains alpha
    // values, need to expand this to RGBA8
    uint32_t rgba8_size = ATLAS_WIDTH * ATLAS_HEIGHT * 4;
    uint32_t* rgba8_pixels = (uint32_t*) malloc(rgba8_size);
    for (int y = 0; y < ATLAS_HEIGHT; y++) {
        for (int x = 0; x < ATLAS_WIDTH; x++) {
            int index = y*ATLAS_WIDTH + x;
            rgba8_pixels[index] = 0x00FFFFFF | ((uint32_t)atlas_bitmap[index]<<24);
        }
    }
    atlas_img = sg_make_image(&(sg_image_desc){
        .width = ATLAS_WIDTH,
        .height = ATLAS_HEIGHT,
        .data = {
            .subimage[0][0] = {
                .ptr = rgba8_pixels,
                .size = rgba8_size
            }
        }
    });
    atlas_smp = sg_make_sampler(&(sg_sampler_desc){
        // LINEAR would be better for text quality in HighDPI, but the
        // atlas texture is "leaking" from neighbouring pixels unfortunately
        .min_filter = SG_FILTER_LINEAR,
        .mag_filter = SG_FILTER_LINEAR,
    });
    pip = sgl_make_pipeline(&(sg_pipeline_desc){
        .colors[0].blend = {
            .enabled = true,
            .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
            .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA
        },
        .depth = {
            .write_enabled = true,
            .compare = SG_COMPAREFUNC_LESS_EQUAL,
        },
    });

    free(rgba8_pixels);

    stm_setup();

    zui_init();
    zui_font(&atlasfont);
}
static i32 zui_convert_code(i32 sokol_code) {
    switch(sokol_code) {
        case SAPP_KEYCODE_LEFT_SHIFT: return ZK_L_SHIFT;
        case SAPP_KEYCODE_RIGHT_SHIFT: return ZK_R_SHIFT;
        case SAPP_KEYCODE_LEFT_CONTROL: return ZK_L_CTRL;
        case SAPP_KEYCODE_RIGHT_CONTROL: return ZK_L_CTRL;
        case SAPP_KEYCODE_LEFT_ALT: return ZK_L_ALT;
        case SAPP_KEYCODE_RIGHT_ALT: return ZK_R_ALT;
        case SAPP_KEYCODE_LEFT_SUPER: return ZK_SUPER;
    }
    return 0;
}

static void event(const sapp_event* ev) {
    i32 code = 0;
    switch (ev->type) {
        case SAPP_EVENTTYPE_MOUSE_DOWN:
            zui_input_mousemove((zvec2) { ev->mouse_x, ev->mouse_y });
            zui_input_mousedown(1 << ev->mouse_button);
            break;
        case SAPP_EVENTTYPE_MOUSE_UP:
            zui_input_mousemove((zvec2) { ev->mouse_x, ev->mouse_y });
            zui_input_mouseup(1 << ev->mouse_button);
            break;
        case SAPP_EVENTTYPE_MOUSE_MOVE:
            zui_input_mousemove((zvec2) { ev->mouse_x, ev->mouse_y });
            break;
        case SAPP_EVENTTYPE_MOUSE_SCROLL:
            break;
        case SAPP_EVENTTYPE_KEY_DOWN:
            switch(ev->key_code) {
                case SAPP_KEYCODE_LEFT: zui_input_char(17); break;
                case SAPP_KEYCODE_RIGHT: zui_input_char(18); break;
                case SAPP_KEYCODE_UP: zui_input_char(19); break;
                case SAPP_KEYCODE_DOWN: zui_input_char(20); break;
                default: break;
            }
            if((code = zui_convert_code(ev->key_code)))
                zui_input_keydown(code);
            break;
        case SAPP_EVENTTYPE_KEY_UP:
            if((code = zui_convert_code(ev->key_code)))
                zui_input_keyup(code);
            break;
        case SAPP_EVENTTYPE_CHAR: {
            // on macos, delete has backspace behavior
            #ifdef __MACH__
            if (ev->char_code == 127)
                ev->char_code = '\b';
            #endif
            zui_input_char(ev->char_code & 255);
        } break;
        case SAPP_EVENTTYPE_CLIPBOARD_PASTED: {
            const char *str = sapp_get_clipboard_string();
            while(*str) zui_input_char(*str++);
        } break;
        default:
            break;
    }
}

static void cube(void) {
    sgl_matrix_mode_modelview();
    sgl_translate(0.0f, 0.0f, -5.f);
    sgl_begin_quads();
    sgl_c3f(1.0f, 0.0f, 0.0f);
        sgl_v3f(-1.0f, -1.0f,  1.0f);
        sgl_v3f( 1.0f, -1.0f,  1.0f);
        sgl_v3f( 1.0f,  1.0f,  1.0f);
        sgl_v3f(-1.0f,  1.0f,  1.0f);
    sgl_c3f(0.0f, 1.0f, 0.0f);
        sgl_v3f(-2.0f, -2.0f,  0.0f);
        sgl_v3f( 0.0f, -2.0f,  0.0f);
        sgl_v3f( 0.0f,  0.0f,  0.0f);
        sgl_v3f(-2.0f,  0.0f,  0.0f);
    sgl_end();
}

extern void stm_setup();
// do one frame
void frame(void) {
    //stm_setup();
    sgl_defaults();
    //sgl_push_pipeline();
    sgl_load_pipeline(pip);
    sgl_enable_texture();
    sgl_texture(atlas_img, atlas_smp);
    sgl_matrix_mode_projection();
    //sgl_push_matrix();
    //sgl_ortho(-10.0f, 10, -10, 10.0f, -10000.0f, 10000.0f);
    sgl_ortho(0.0f, (float)sapp_width(), (float)sapp_height(), 0.0f, -1000.0f, 1000.0f);
    sgl_begin_quads();

    static char username[32] = { 0 };
    static char password[32] = { 0 };
    static i32 oops = 0;

    float row_desc[] = { Z_AUTO, 200 };

    zui_window(sapp_width(), sapp_height(), sapp_frame_duration());
        static i32 state = 0;
        zui_col(1, 0);
            zui_combo("test", "a,b,c", &state);
        zui_end();
        // zui_col(3, 0);
        //     zui_row(2, row_desc);
        //         zui_label("Username:");
        //         static i32 user_state;
        //         //zui_size(200, Z_AUTO);
        //         zui_text(username, 32, &user_state);
        //     zui_end();
        //     zui_row(2, row_desc);
        //         zui_label("Password:");
        //         static i32 pass_state;
        //         //zui_size(200, Z_AUTO);
        //         zui_text(password, 32, &pass_state);
        //     zui_end();
        //     static u8 btn;
        //     // zui_just(ZJ_RIGHT);
        //     zui_button("Save", &btn);
        // zui_end();
    zui_end();

    static char shop[32] = { 0 };
    static char ro[32] = { 0 };
    static char comment[32] = { 0 };

    // z_draw_rect((zrect) { 0, 0, 100, 100 }, (zcolor) { 255, 255, 255, 255 }, 15);
    // z_draw_rect((zrect) { 50, 50, 100, 100 }, (zcolor) { 255, 0, 0, 255 }, 10);

    float widths[] = { 100, -1.0f };
    // zui_window(sapp_width(), sapp_height(), sapp_frame_duration());
    //     zui_col(5, 0);
    //         zui_just(ZJ_LEFT);
    //         zui_row(2, widths);
    //             zui_label("Shop: ");
    //             static i32 shop_state;
    //             //zui_size(200, Z_AUTO);
    //             zui_text(shop, sizeof(shop), &shop_state);
    //         zui_end();
    //         zui_row(2, widths);
    //             zui_label("Ro: ");
    //             static i32 ro_state;
    //             //zui_size(200, Z_AUTO);
    //             zui_text(ro, sizeof(ro), &ro_state);
    //         zui_end();
    //         zui_row(2, widths);
    //             zui_label("Scan Type: ");
    //             static i32 ctype_state;
    //             //zui_size(200, Z_AUTO);
    //             zui_combo("type", "Preliminary Estimate,Completed Estimate,Supplement,Invoice,Other Scan", &ctype_state);
    //         zui_end();
    //         zui_row(2, widths);
    //             zui_label("Comment: ");
    //             static i32 comment_state;
    //             //zui_size(200, Z_AUTO);
    //             zui_text(comment, sizeof(comment), &comment_state);
    //         zui_end();
    //         zui_just(ZJ_CENTER);
    //         zui_row(3, (float[]) { 150, -1.0f, 150 });
    //             static u8 btn1, btn2;
    //             zui_button("Cancel", &btn1);
    //             zui_blank();
    //             zui_button("Print", &btn2);
    //         zui_end();
    //     zui_end();
    // zui_end();

    zcmd_draw *draw_cmd;
    i32 i = 0;
    while((draw_cmd = zui_draw_next())) {
        //i++;
        switch(draw_cmd->base.id) {
            case ZCMD_DRAW_RECT: z_draw_rect(draw_cmd->rect.rect, draw_cmd->rect.color, draw_cmd->rect.zindex); break;
            case ZCMD_CLIP: z_set_clip_rect(draw_cmd->clip.cliprect); break;
            case ZCMD_DRAW_TEXT: z_draw_text(draw_cmd->text.text, draw_cmd->text.len, draw_cmd->text.coord, draw_cmd->text.color, draw_cmd->text.zindex); break;
        }
    }

    //printf("cmd count: %d\n", i);

    static float rot[2] = { 0, 0 };
    rot[0] += 1.0f * sapp_frame_duration();
    rot[1] += 2.0f * sapp_frame_duration();
    // sgl_matrix_mode_modelview();
    // //sgl_translate(0.0f, 0.0f, -12.0f);
    // sgl_rotate(sgl_rad(rot[0]), 1.0f, 0.0f, 0.0f);
    // sgl_rotate(sgl_rad(rot[1]), 0.0f, 1.0f, 0.0f);
    // sgl_push_matrix();
    //cube();
    //sgl_pop_matrix();

    sgl_end();
    //sgl_pop_matrix();
    //sgl_pop_pipeline();

    // render the sokol-gfx default pass
    sg_begin_pass(&(sg_pass) {
        .action = {
            .colors[0] = {
                .load_action = SG_LOADACTION_CLEAR,
                .clear_value = { 0.0f, 0.0f, 0.0f, 1.0f }
            }
        },
        .swapchain = sglue_swapchain()
    });
    sgl_draw();
    sg_end_pass();
    sg_commit();
}

static void cleanup(void) {
    sgl_shutdown();
    sg_shutdown();
}

sapp_desc sokol_main(int argc, char* argv[]) {
    (void)argc; (void)argv;
    return (sapp_desc){
        .init_cb = init,
        .frame_cb = frame,
        .cleanup_cb = cleanup,
        .event_cb = event,
        .width = 720,
        .height = 540,
        .window_title = "zui.h",
        .icon.sokol_default = true,
        .enable_clipboard = true,
        .clipboard_size = 256,
        .logger.func = slog_func,
    };
}
