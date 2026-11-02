#!/bin/sh
# Controls on the real gameplay (issue #16), end to end through the core and the headless
# frontend (tools/harness.cpp) on the original data. Two runs:
#  pad:   the menus with the pad only (as core_e2e.sh) up to level 1, then the direct scheme:
#         the left stick drives the Kassandra right and then up to a point and stops it there,
#         the right stick aims at the turret ahead (aim assist) and fires until it is hit;
#         the camera follows; L1/R1 with a single weapon change nothing.
#  touch: the menus with the touchscreen only (taps, a long press for "new player") up to
#         level 1, then a tap on the ground drives the Kassandra there and the camera follows.
# Checks come from the core's trace (GROMADA_TRACE): flagman, camera, weapons, targets' hp.
# Frames are given at 60 fps and doubled at 120 fps; touch x is shifted by 107 px in 16:9
# (the 640x480 menus are centred in the 854x480 picture).
# usage: controls_e2e.sh <harness> <core.so> <out_dir> <pad|touch> [harness options, e.g. gromada_fps=120]
# Screenshots (PPM, copyrighted graphics: never commit them) go to <out_dir>. Exit 77 without GROMADA_DATA.
set -u
if [ -z "${GROMADA_DATA:-}" ] || [ ! -f "$GROMADA_DATA/fw.res" ]; then
    echo "SKIP (set GROMADA_DATA to the game folder)"; exit 77
fi
harness=$1; core=$2; out=$3; mode=$4; shift 4
rm -rf "$out"; mkdir -p "$out"
scale=1; wide=0
case " $* " in *gromada_fps=120*) scale=2 ;; esac
case " $* " in *gromada_aspect=16:9*) wide=1 ;; esac
f() { echo $(( $1 * scale )); }
r() { echo "$(f $1)-$(f $2)"; }
# touch at menu coordinates (640x480 screen), for a few frames
tap() { echo "touch=$(( $1 + wide * 107 )),$2@$(r $3 $(( $3 + 2 )))"; }
fail() { echo "FAILED ($mode): $*"; tail -5 "$out/log.txt"; exit 1; }

if [ "$mode" = pad ]; then
    set -- "$@" gromada_input_scheme=Diretto \
        btn=a@$(r 10 12) btn=a@$(r 40 42) btn=x@$(r 80 82) btn=a@$(r 160 162) \
        btn=x@$(r 420 422) btn=a@$(r 460 462) btn=down@$(r 480 482) btn=a@$(r 500 502) \
        btn=start@$(r 540 545) btn=x@$(r 600 602) btn=x@$(r 800 803) btn=x@$(r 1300 1303) \
        btn=r@$(r 1350 1352) btn=l@$(r 1370 1372) \
        lstick=1,0@$(r 1400 1459) lstick=0,-1@$(r 1460 1609) rstick=0,-1@$(r 1740 1900) \
        shots=$(f 1399),$(f 1610),$(f 1730),$(f 1900)
    frames=$(f 1920)
else
    set -- "$@" $(tap 320 240 10) $(tap 320 240 40) $(tap 320 240 80) $(tap 320 240 160) \
        "touch=$(( 350 + wide * 107 )),256@$(r 300 350)" $(tap 350 256 420) $(tap 420 240 500) \
        $(tap 135 357 800) $(tap 135 357 1300) \
        shots=$(f 340),$(f 460),$(f 900),$(f 1399),$(f 1700)
    frames=$(f 1720)
fi
GROMADA_TRACE=1 "$harness" "$core" "$GROMADA_DATA/gromada.exe" "$frames" "$out/shot" savedir="$out/save" "$@" \
    2> "$out/log.txt" || fail "harness exit $?"
grep -q 'load maps\\levels\\01.map' "$out/log.txt" || fail "level 1 not reached"
if grep -q 'script .*: .*error\|\[Gromada\] script maps' "$out/log.txt"; then fail "script errors"; fi

# trace field of frame n (60 fps numbering)
field() { grep "^\[f$(f $1)\] \[Gromada\] trace" "$out/log.txt" | sed -n "s/.* $2=\([^ ]*\).*/\1/p"; }
xy() { echo "$1" | cut -d, -f$2; }

if [ "$mode" = touch ]; then
    # the player has no pad here: the touch tap drives (left click on the ground)
    p0=$(field 1399 flagman); [ -n "$p0" ] && [ "$(xy "$p0" 1)" != -1 ] || fail "no flagman at frame 1399"
    # tap 180 px right of the player's screen position (from the trace at 1399), replaying
    # the same run (deterministic) with that tap added
    cam=$(field 1399 cam)
    sx=$(( $(xy "$p0" 1) - $(xy "$cam" 1) + 180 )); sy=$(( $(xy "$p0" 2) - $(xy "$cam" 2) ))
    GROMADA_TRACE=1 "$harness" "$core" "$GROMADA_DATA/gromada.exe" "$frames" "$out/shot" savedir="$out/save2" "$@" \
        "touch=$sx,$sy@$(r 1420 1422)" 2> "$out/log2.txt" || fail "harness exit $?"
    mv "$out/log2.txt" "$out/log.txt"
    p1=$(field 1700 flagman); cam1=$(field 1700 cam)
    dx=$(( $(xy "$p1" 1) - $(xy "$p0" 1) )); dy=$(( $(xy "$p1" 2) - $(xy "$p0" 2) ))
    [ $dx -ge 130 ] && [ $dx -le 200 ] && [ $dy -ge -30 ] && [ $dy -le 30 ] || fail "tap did not drive there (moved $dx,$dy)"
    [ "$(xy "$cam1" 1)" -gt "$(xy "$cam" 1)" ] || fail "camera did not follow ($cam -> $cam1)"
    [ "$(field 1700 ctx)" = 0 ] || fail "not in the gameplay context"
    head -1 "$out/save/gromada/save5" | grep -q '^.\?$' || fail "the new (unnamed) profile was not saved in slot 5"
    echo "OK touch: menus by touch, tap drove $dx,$dy, camera $cam -> $cam1"
    exit 0
fi

p0=$(field 1399 flagman); p1=$(field 1700 flagman); p2=$(field 1730 flagman)
[ -n "$p0" ] && [ "$(xy "$p0" 1)" != -1 ] || fail "no flagman at frame 1399"
dx=$(( $(xy "$p1" 1) - $(xy "$p0" 1) )); dy=$(( $(xy "$p1" 2) - $(xy "$p0" 2) ))
# 1 s right (turning first) then 2.5 s up
[ $dx -ge 25 ] && [ $dx -le 120 ] && [ $dy -ge -260 ] && [ $dy -le -120 ] || fail "drove to $dx,$dy (expected ~+60,-180)"
[ "$p1" = "$p2" ] || fail "did not stop after the stick was released ($p1 -> $p2)"
cam0=$(field 1399 cam); cam1=$(field 1700 cam)
[ $(( $(xy "$cam0" 2) - $(xy "$cam1" 2) )) -ge 60 ] || fail "camera did not follow ($cam0 -> $cam1)"
[ "$(field 1380 weapon)" = 0 ] && [ "$(field 1380 mask)" = 1 ] && [ "$(field 1380 pending)" = -1 ] || fail "weapons"
hp0=$(field 1739 hp); hp1=$(field 1900 hp)
locked=$(grep '\] trace' "$out/log.txt" | sed -n "$(f 1745),$(f 1900)p" | grep -c 'locked=1')
[ "$locked" -ge "$(f 100)" ] || fail "aim assist did not lock the turret ($locked frames)"
[ $(( hp0 - hp1 )) -ge 4 ] || fail "the target was not hit (hp $hp0 -> $hp1)"
echo "OK pad: drove $dx,$dy and stopped, camera $cam0 -> $cam1, locked $locked frames, targets' hp $hp0 -> $hp1"
