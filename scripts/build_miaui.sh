#!/bin/bash
# Build Mega Man for MiaUI (Anbernic RG35XX, ARMv7 hard-float), inside the
# linksawakening-miaui Docker image (Debian armhf cross compilers + SDL2):
#
#   docker run --rm -v <repo>:/src linksawakening-miaui bash /src/scripts/build_miaui.sh
#
# Output: build-miaui/Mega Man.pak/ (copy into Ports/ on the SD card).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build-miaui"
PAK="$OUT/Mega Man.pak"
CC=arm-linux-gnueabihf-gcc
ARCH="-marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard"
mkdir -p "$PAK" "$OUT/obj"

echo '[1/2] fceumm core'
if [ ! -f "$OUT/fceumm_libretro.so" ] || [ "$ROOT/libretro-fceumm/.git/HEAD" -nt "$OUT/fceumm_libretro.so" ]; then
    rm -rf "$OUT/fceumm-src"
    cp -r "$ROOT/libretro-fceumm" "$OUT/fceumm-src"
    make -C "$OUT/fceumm-src" -f Makefile.libretro clean >/dev/null 2>&1 || true
    make -C "$OUT/fceumm-src" -f Makefile.libretro platform=unix CC="$CC $ARCH" CXX="arm-linux-gnueabihf-g++ $ARCH" \
        -j"$(nproc)" >/dev/null
    cp "$OUT/fceumm-src/fceumm_libretro.so" "$OUT/"
    arm-linux-gnueabihf-strip "$OUT/fceumm_libretro.so"
fi

echo '[2/2] host'
$CC $ARCH -O2 -Wall -Wextra -DDISPLAY_HZ=60.46 -I"$ROOT/host" -I/usr/include/SDL2 -I/usr/include/arm-linux-gnueabihf/SDL2 \
    "$ROOT/host/mm_host.c" "$ROOT/host/mm_game.c" "$ROOT/host/mm_ui.c" -lSDL2 -ldl -lm -o "$PAK/megaman.elf"

cp "$OUT/fceumm_libretro.so" "$PAK/"
cp "$ROOT/miaui/launch.sh" "$PAK/"
chmod +x "$PAK/launch.sh" "$PAK/megaman.elf"
ls -lh "$PAK"
