#!/bin/sh
# End-to-end test of the libretro core with the headless frontend (tools/harness.cpp) on the
# original data: boot -> logo -> main menu -> new player -> mission map -> level 1 briefing ->
# level 1 -> Esc (game menu) -> logoff -> mission map, with the controller only (A = click,
# X = Enter, Start = Esc / Enter while typing the name). Screenshots of every step are written
# in <out_dir> (PPM; they contain copyrighted graphics: never commit them).
# usage: core_e2e.sh <harness> <core.so> <out_dir> [extra harness options, e.g. gromada_fps=120]
# Exit code 77 (skipped) without GROMADA_DATA.
set -u
if [ -z "${GROMADA_DATA:-}" ] || [ ! -f "$GROMADA_DATA/fw.res" ]; then
    echo "SKIP (set GROMADA_DATA to the game folder)"; exit 77
fi
harness=$1; core=$2; out=$3; shift 3
rm -rf "$out"; mkdir -p "$out"
# Frames at 60 fps (doubled below for 120 fps).
scale=1
case " $* " in *gromada_fps=120*) scale=2 ;; esac
f() { echo $(( $1 * scale )); }
r() { echo "$(f $1)-$(f $2)"; }
"$harness" "$core" "$GROMADA_DATA/gromada.exe" $(f 1800) "$out/shot" savedir="$out/save" \
    btn=a@$(r 10 12) btn=a@$(r 40 42) btn=x@$(r 80 82) btn=a@$(r 160 162) \
    btn=x@$(r 420 422) btn=a@$(r 460 462) btn=down@$(r 480 482) btn=a@$(r 500 502) \
    btn=start@$(r 540 545) btn=x@$(r 600 602) btn=x@$(r 800 803) btn=x@$(r 1300 1303) \
    btn=start@$(r 1500 1504) btn=up@$(r 1600 1602) btn=x@$(r 1640 1643) \
    shots=$(f 5),$(f 100),$(f 399),$(f 440),$(f 520),$(f 580),$(f 759),$(f 1100),$(f 1400),$(f 1560),$(f 1620),$(f 1760) \
    "$@" 2> "$out/log.txt"
status=$?
grep -o 'load maps[^ ]*' "$out/log.txt" | sed 's/^load //' > "$out/maps.txt"
expected='maps\logo.map
maps\mainmenu.map
maps\player.map
maps\maps.map
maps\levels\01.map
maps\maps.map'
if [ $status -ne 0 ] || [ "$(cat "$out/maps.txt")" != "$expected" ]; then
    echo "FAILED (harness exit $status); maps loaded:"; cat "$out/maps.txt"; tail -20 "$out/log.txt"; exit 1
fi
if ! head -1 "$out/save/gromada/save4" | grep -q '^AB'; then
    echo "FAILED: the new profile was not saved"; exit 1
fi
for m in 'movie avi\\0.avi' 'movie avi\\1.avi' 'movie avi\\2.avi'; do
    grep -q "$m" "$out/log.txt" || { echo "FAILED: no '$m'"; exit 1; }
done
if grep -q 'script .*: .*error\|\[Gromada\] script maps' "$out/log.txt"; then
    echo "FAILED: script errors"; grep 'script' "$out/log.txt" | head; exit 1
fi
echo "OK: $(tr '\n' ' ' < "$out/maps.txt")"
