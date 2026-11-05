# Mega Man (NES), Celeste style, for handhelds

A small libretro frontend (`host/mm_host.c`) that runs the original
*Mega Man (USA)* in [fceumm](https://github.com/libretro/libretro-fceumm) and
changes how it plays from outside the console (RAM reads/writes and
snapshots, no ROM patch), in `host/mm_game.c`:

- checkpoints at every screen transition going up, and behind the first boss door
- a death counter instead of lives (lives never run out): finish with as few deaths as you can
- L / R switch weapons, Y rapid fire
- three save files (START on the title screen)
- MENU: palette, aspect (pixel perfect / NTSC 8:7 / full 4:3), filter
  (smooth / crisp), refill weapons on respawn, main menu, quit

The details (RAM addresses, how each part works) are in the comment at the
top of `host/mm_game.c`.

## Build for MiaUI (Anbernic RG35XX)

Needs Docker and the `linksawakening-miaui` image (armhf cross compiler + SDL2):

    git submodule update --init
    docker run --rm -v "$PWD":/src linksawakening-miaui bash /src/scripts/build_miaui.sh

Copy `build-miaui/Mega Man.pak` into `Ports/` on the SD card. The ROM is not
included: the launcher takes `Mega Man (USA).zip` from your NES ROM folder.
Saves and the log go to `Saves/MegaMan/`.

## Run on a computer

    cc -O2 -o mm host/*.c $(sdl2-config --cflags --libs)
    ./mm --core path/to/fceumm_libretro.dylib "Mega Man (USA).nes"

`--headless --frames N --input script.txt --shot prefix --shot-at F --watch ADDRS --poke ADDR=VAL@F`
run it without a window, for tests.
