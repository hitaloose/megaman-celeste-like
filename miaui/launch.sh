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

overclock.elf "${CPU_SPEED_PERF:-1296000}" 2>/dev/null

[ -f "$SAVE_DIR/log.txt" ] && mv "$SAVE_DIR/log.txt" "$SAVE_DIR/log.prev.txt"
./megaman.elf --core ./fceumm_libretro.so --save-dir "$SAVE_DIR" "$ROM" > "$SAVE_DIR/log.txt" 2>&1
