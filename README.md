# Mega Man (NES), Celeste style, for handhelds

A small libretro frontend (`host/mm_host.c`) that runs the original
*Mega Man (USA)* in [fceumm](https://github.com/libretro/libretro-fceumm) and
changes how it plays from outside the console (RAM reads/writes and
snapshots, no ROM patch), in `host/mm_game.c`:

- checkpoints at every screen transition (in Cut Man's stage, not going down), and behind the first boss door
- no slowdown: the emulated NES CPU gets twice the time per frame (fceumm's 2x-Postrender overclock)
- softer sound: fceumm's RF audio filter (the muffled highs of a TV over RF)
- a death counter instead of lives (lives never run out): finish with as few deaths as you can
- L / R switch weapons, Y rapid fire
- three save files (START on the title screen); a new game picks its mode:
  weapon energy refilled on respawn or not
- Hall of Fame (last item of SELECT FILE), one per mode: every finished run
  with date, deaths and score, ranked by fewest deaths (score breaks ties);
  beating Wily goes there with the run marked, and frees its file
- MENU: palette, aspect (pixel perfect / NTSC 8:7 / full 4:3), filter
  (smooth / crisp), overlay (CRT scanlines, with or without the screen frame,
  from ourigen's [perfect_overlays](https://github.com/ourigen/perfect_overlays);
  credits in `assets/overlays/CREDITS.txt`), main menu, quit

The overlays are embedded in the program: after changing a PNG in
`assets/overlays/`, run `sh scripts/gen_overlays.sh`.

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

Ready builds for Windows, macOS (Apple Silicon) and Linux, and the MiaUI
package, come from GitHub Actions (`.github/workflows/build.yml`): the
artifacts of each run, and the release of each `v*` tag. Put your ROM next
to the program as `megaman.nes` (details in `desktop/README.txt`).

To build it yourself:

    make -C libretro-fceumm -f Makefile.libretro
    cc -O2 -o megaman host/*.c $(sdl2-config --cflags --libs)
    ./megaman --core libretro-fceumm/fceumm_libretro.dylib "Mega Man (USA).nes"

Without `--core` / ROM / `--save-dir` it looks next to the program for
`fceumm_libretro.{so,dylib,dll}`, `megaman.nes` (or `Mega Man (USA).nes`) and
`saves/`. Keys: arrows, Z = B, X = A, A = rapid fire, Enter = Start,
Right Shift = Select, Q / W = L / R, Esc = MENU; or a gamepad.

`--headless --frames N --input script.txt --shot prefix --shot-at F --watch ADDRS --poke ADDR=VAL@F`
run it without a window, for tests.

## Benchmark (CPU clock)

In the game, at a heavy screen, open MENU and press SELECT (F5 on a computer):
that moment goes to `Saves/MegaMan/bench.state`. Then `--bench bench.state`
runs 30 s from it and prints the time each frame's work took and the game's
own lag frames. On the handheld, `sh bench.sh` in `Mega Man.pak` (over adb, with
the game closed) does that at every CPU clock, NES overclock on and off, into
`Saves/MegaMan/bench.txt`.
