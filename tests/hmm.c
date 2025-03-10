#include "../src/zui.h"
static void *ZS;
#define ZS_zui_button(...) u8
#define ZS(e) (({ static ZS_##e _; ZS = &_; }), e)
int main() {
    zs_text txt;
    char buffer[512];



    if ZS(zui_button("", ZS)) {

    }
    //ZS(zui_text(buffer, 512, &_));
}
