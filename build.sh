if [ "$1" = "sokol" ]; then
    clang tests/sokol-test.c src/zui.c backends/sokol/sokol.m -Ibackends/sokol -Itests -Isrc -o bin/sokol-test -fobjc-arc -framework Metal -framework Cocoa -framework MetalKit -framework Quartz
    if [ "$2" = "run" ]; then
        ./bin/sokol-test
    fi
elif [ "$1" = "bake" ]; then
    # bake single header library
    cat src/zui.h > zui-sh.h
    echo "#ifdef ZUI_IMPLEMENTATION" >> zui-sh.h
    tail -n +2 src/zui.c >> zui-sh.h
    echo "#endif" >> zui-sh.h
else
    echo "unrecognized option $1"
fi
