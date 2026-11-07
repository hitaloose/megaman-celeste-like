#!/bin/sh
# MiaUI launcher for Mega Man (NES) with checkpoints and a death counter:
# ROMS/Ports/Mega Man.pak/. Uses your "Mega Man (USA)" ROM from the NES
# folder (unzipped into this folder the first time). Saves go to Saves/MegaMan/.
PAK_PATH="$(cd "$(dirname "$0")" && pwd)"
cd "$PAK_PATH"

SAVE_DIR="${SAVES_PATH:-/userdata/Saves}/MegaMan"
mkdir -p "$SAVE_DIR"

ROM="$PAK_PATH/megaman.nes"
if [ ! -f "$ROM" ]; then
    for z in "${ROMS_PATH:-/userdata/Roms}"/*NES*/"Mega Man (USA)".zip; do
        [ -f "$z" ] || continue
        unzip -p "$z" "*.nes" > "$ROM.tmp" 2>/dev/null && mv "$ROM.tmp" "$ROM"
        break
    done
fi
[ -f "$ROM" ] || { echo "Mega Man (USA) ROM not found" > "$SAVE_DIR/log.txt"; exit 1; }

# 720 MHz: the heaviest screen measured (Cut Man's Bladers, firing, NES overclock
# on) takes at most 9.7 ms of the 16.7 ms frame (bench.sh). MiaUI's CPU_SPEED_PERF
# would be 1488 MHz. overclock.elf takes 720 MHz only by the name "low".
overclock.elf low >/dev/null 2>&1

[ -f "$SAVE_DIR/log.txt" ] && mv "$SAVE_DIR/log.txt" "$SAVE_DIR/log.prev.txt"
./megaman.elf --core ./fceumm_libretro.so --save-dir "$SAVE_DIR" "$ROM" > "$SAVE_DIR/log.txt" 2>&1
