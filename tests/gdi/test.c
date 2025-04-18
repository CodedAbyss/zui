#define ZUI_IMPL
#include "../../backends/gdi/zui-gdi.h"
#include <stdio.h>

void init(void *user_data) {
    zui_log("init\n");
    zui_new_font("Consolas", 16);
}

void frame(void *user_data) {
    zui_window(); {
        zui_label("Hello!");
    } zui_end();
    zui_render();
}

void close(void *user_data) {
    printf("close\n");
}

void LOG(char *fmt, va_list args, void *user_data) { vprintf(fmt, args); }

i32 main(i32 argc, char **argv) {
    zui_init(gdi_renderer, LOG, &(zui_gdi_args) {
        .width = 300,
        .height = 200,
        .title = "Zui GDI example",
        .init = init,
        .frame = frame,
        .close = close
    });
}

