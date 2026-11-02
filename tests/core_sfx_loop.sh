#!/bin/sh
# Looping engine sounds through the core (issue #17): reaches level 1 like core_e2e.sh, then
# drives the player's vehicle (ELEPHANT, tracks: looping effect 43) with move orders for ~15 s.
# The engine requests its sound once per animation cycle (~every 50 ms): the effect must start
# once and be kept alive, not restarted (the session logs "sfx <n> loop start" for each start
# of a looping effect). The captured audio is <out_dir>/run.wav.
# usage: core_sfx_loop.sh <harness> <core.so> <out_dir> [extra harness options, e.g. gromada_fps=120]
# Exit code 77 (skipped) without GROMADA_DATA.
set -u
if [ -z "${GROMADA_DATA:-}" ] || [ ! -f "$GROMADA_DATA/fw.res" ]; then
    echo "SKIP (set GROMADA_DATA to the game folder)"; exit 77
fi
harness=$1; core=$2; out=$3; shift 3
rm -rf "$out"; mkdir -p "$out"
scale=1
case " $* " in *gromada_fps=120*) scale=2 ;; esac
f() { echo $(( $1 * scale )); }
r() { echo "$(f $1)-$(f $2)"; }
# Every 120 frames from the start of the level: cursor to one side for 20 frames, then A
# (left click = move order).
moves=""; i=0
for start in 1320 1440 1560 1680 1800 1920 2040; do
    case $((i % 4)) in 0) st="1,0" ;; 1) st="0,1" ;; 2) st="-1,0" ;; 3) st="0,-1" ;; esac
    moves="$moves lstick=$st@$(r $start $((start + 20))) btn=a@$(r $((start + 24)) $((start + 26)))"
    i=$((i + 1))
done
"$harness" "$core" "$GROMADA_DATA/gromada.exe" $(f 2200) "$out/run" savedir="$out/save" \
    btn=a@$(r 10 12) btn=a@$(r 40 42) btn=x@$(r 80 82) btn=a@$(r 160 162) \
    btn=x@$(r 420 422) btn=a@$(r 460 462) btn=down@$(r 480 482) btn=a@$(r 500 502) \
    btn=start@$(r 540 545) btn=x@$(r 600 602) btn=x@$(r 800 803) btn=x@$(r 1300 1303) \
    $moves "$@" 2> "$out/log.txt"
status=$?
if [ $status -ne 0 ] || ! grep -q 'load maps\\levels\\01.map' "$out/log.txt"; then
    echo "FAILED (harness exit $status): level 1 not reached"; tail -20 "$out/log.txt"; exit 1
fi
starts=$(grep -c 'sfx 43 loop start' "$out/log.txt")
if [ "$starts" -ne 1 ]; then
    echo "FAILED: the engine loop (sfx 43) started $starts time(s), expected 1"
    grep 'loop start' "$out/log.txt"; exit 1
fi
echo "OK: looping starts: $(grep -o 'sfx [0-9]* loop start' "$out/log.txt" | sort | uniq -c | tr -s ' ' | tr '\n' ';')"
