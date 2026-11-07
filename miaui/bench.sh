#!/bin/sh
# CPU clock benchmark for Mega Man on the RG35XX. Needs Saves/MegaMan/bench.state:
# in the game, at a heavy screen, open MENU and press SELECT.
#
#   sh bench.sh [FRAMES] [CLOCKS...]      (from the Mega Man.pak folder, e.g. over adb)
#   BENCH_INPUT=FILE sh bench.sh ...      pad script for the runs (default: Y held, rapid fire)
#
# For each CPU clock (default: every one the kernel offers, 504 MHz and up) runs
# the same 30 s from bench.state with the NES overclock on and off, and appends
# one line per run to Saves/MegaMan/bench.txt. Close the game before running it.
PAK_PATH="$(cd "$(dirname "$0")" && pwd)"
cd "$PAK_PATH"
SAVE_DIR="${SAVES_PATH:-/userdata/Saves}/MegaMan"
STATE="$SAVE_DIR/bench.state"
OUT="$SAVE_DIR/bench.txt"
[ -f "$STATE" ] || { echo "no $STATE: open MENU in the game and press SELECT" >&2; exit 1; }

FRAMES="${1:-1800}"
[ $# -gt 0 ] && shift
CLOCKS="$*"
if [ -z "$CLOCKS" ]; then
    for f in $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_frequencies 2>/dev/null); do
        [ "$f" -ge 504000 ] && CLOCKS="$CLOCKS $f"
    done
fi
[ -n "$CLOCKS" ] || CLOCKS="504000 720000 840000 1008000 1104000 1200000 1296000"

INPUT="${BENCH_INPUT:-/tmp/bench_input.txt}"
[ -n "$BENCH_INPUT" ] || echo "0 Y" > "$INPUT"

# overclock.elf only takes a few values (others become 1.2 GHz): set the cap as it does
set_clock() { for c in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_max_freq; do echo "$1" > "$c"; done; }

ORIG=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq)
echo "# $(date) frames=$FRAMES input=$(tr '\n' ' ' < "$INPUT")" >> "$OUT"
for clk in $CLOCKS; do
    set_clock "$clk"
    sleep 1
    cur=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null)
    for oc in 2x-Postrender disabled; do
        line=$(./megaman.elf --core ./fceumm_libretro.so --save-dir "$SAVE_DIR" --bench "$STATE" --frames "$FRAMES" \
               --poke 6A=1C --input "$INPUT" --opt fceumm_overclocking=$oc ./megaman.nes 2>/dev/null | grep '^bench')
        echo "clock=$clk actual=$cur $line" | tee -a "$OUT"
    done
done
set_clock "$ORIG"
