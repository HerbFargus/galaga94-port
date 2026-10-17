#!/bin/sh
# Build galaga94 (Windows: MSYS2 mingw32 gcc + SDL2 VC dev libs; Linux: gcc + sdl2-config).
set -e
cd "$(dirname "$0")"
# src/game.c is checked in; regenerate it (needs Python + capstone) with REGEN=1
if [ -n "$REGEN" ] || [ ! -f src/game.c ]; then
    mkdir -p out
    python tools/recomp.py data/GALAGA.EXE > src/game.c 2> out/recomp.log || { cat out/recomp.log; exit 1; }
fi
mkdir -p build
SRC="src/game.c src/runtime.c src/vga.c src/sound.c src/platform.c"
case "$(uname -s)" in
MINGW*|MSYS*|CYGWIN*)
    SDL=${SDL:-../sdl2/SDL2-2.32.10}
    CC=${CC:-/c/msys64/mingw32/bin/gcc}
    PATH=/c/msys64/mingw32/bin:$PATH
    $CC -O2 $CFLAGS -std=gnu11 -Wall -Wno-unused-label -Wno-unused-value -Wno-unused-but-set-variable \
        -DSDL_MAIN_HANDLED -I"$SDL/include" -o build/galaga94.exe $SRC "$SDL/lib/x86/SDL2.lib" \
        -static-libgcc $([ -n "$CONSOLE" ] && echo -mconsole || echo -mwindows)
    cp "$SDL/lib/x86/SDL2.dll" build/
    ;;
*)
    ${CC:-gcc} -O2 -std=gnu11 -Wall -Wno-unused-label -Wno-unused-value -Wno-unused-but-set-variable \
        -DSDL_MAIN_HANDLED $(sdl2-config --cflags) -o build/galaga94 $SRC $(sdl2-config --libs)
    ;;
esac
echo built
