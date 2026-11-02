#!/bin/sh
# Console controls of issue #19, end to end through the core and the headless frontend
# (tools/harness.cpp) on the original data, in level 1 (the STAND arena). The test writes its
# own profile "AB" in slot 1 with the HAIL ballistic cannon in weapon slot 2 (10 shells), so
# that the pad menus (as in controls_e2e.sh) select that player and start the level. In every
# run the Kassandra drives north with the d-pad (the original's arrows, same in both schemes)
# and stops 234 px south of a static enemy T: beyond the point where the metric-range aim of
# the old aim point fell (280 / 1.5 = 187 px straight up), within the laser's 280 px range,
# and off screen. The camera follows the player in every run.
#  lock:      A) twin stick, reticle hidden: the right stick straight up fires the laser; the
#                invisible lock takes T (off screen) and T loses hp;
#             B) cursor scheme: the cursor is scrolled to T (edge scroll, then the mouse on T)
#                and R2 fires: T loses exactly the same hp, frame by frame, as in A (the same
#                original shot: a right click on the unit);
#             C) twin stick with the lock off: no lock, the shot goes to full range along the
#                stick (the laser rays may still stop on T on their way: reported, not checked).
#             Also: R2 = right button (RDown) and L2 = left click (the Kassandra drives) in the
#             cursor scheme, A/B held = zoom in/out only in the level, A clicks in the menus;
#             L2/R2 analog = zoom rate in the twin-stick scheme.
#  ballistic: the right stick 15 degrees off T (T 40..50 px from the aim line): with HAIL
#             (ballistic, wide corridor) T is locked and a shell lands on it; with the lock off
#             the shell goes to full range along the stick and misses T; with the laser
#             (narrow corridor) T is not locked (aim point at full range).
# Checks come from the core's trace (GROMADA_TRACE). Frames are given at 60 fps and doubled
# at 120 fps.
# usage: controls_lock_e2e.sh <harness> <core.so> <out_dir> <lock|ballistic> [harness options]
# Screenshots (PPM, copyrighted graphics: never commit them) go to <out_dir>. Exit 77 without GROMADA_DATA.
set -u
if [ -z "${GROMADA_DATA:-}" ] || [ ! -f "$GROMADA_DATA/fw.res" ]; then
    echo "SKIP (set GROMADA_DATA to the game folder)"; exit 77
fi
harness=$1; core=$2; out=$3; mode=$4; shift 4
extra="$*" # harness options of the configuration (gromada_fps, gromada_aspect)
rm -rf "$out"; mkdir -p "$out"
scale=1
case " $* " in *gromada_fps=120*) scale=2 ;; esac
f() { echo $(( $1 * scale )); }
r() { echo "$(f $1)-$(f $2)"; }
fail() { echo "FAILED ($mode): $*"; exit 1; }

# The player's profile (the game's own text format, one value per line): name, map, nweapon,
# hp, noammo[8], weapon[4] (88 HEPHEST, 91 HAIL), opt_player, kassandra, upgrade.
profile() {
    mkdir -p "$1/gromada"
    printf 'AB\r\n0\r\n0\r\n40\r\n20\r\n0\r\n0\r\n10\r\n0\r\n0\r\n0\r\n0\r\n88\r\n91\r\n0\r\n0\r\n4\r\n132\r\n16385\r\n' \
        > "$1/gromada/save1"
}
# Menus by pad (select player 1, start level 1), then 1.5 s north with the d-pad.
common="btn=a@$(r 10 12) btn=a@$(r 40 42) btn=x@$(r 80 82) btn=a@$(r 160 162) \
btn=x@$(r 420 422) btn=x@$(r 520 522) btn=x@$(r 1020 1023) btn=up@$(r 1060 1150)"
# run <name> <frames at 60 fps> <options...>
run() {
    name=$1; frames=$(f $2); shift 2
    profile "$out/$name"
    # shellcheck disable=SC2086
    GROMADA_TRACE=1 "$harness" "$core" "$GROMADA_DATA/gromada.exe" "$frames" "$out/$name/shot" savedir="$out/$name" \
        "gromada_camera=Segue sempre il giocatore" $extra $common "$@" 2> "$out/$name.log" || fail "$name: harness exit $?"
    grep -q 'load maps\\levels\\01.map' "$out/$name.log" || fail "$name: level 1 not reached"
}
# trace field of frame n (60 fps numbering) in run $1
field() { grep "^\[f$(f $2)\] \[Gromada\] trace" "$out/$1.log" | sed -n "s/.* $3=\([^ ]*\).*/\1/p"; }
xy() { echo "$1" | cut -d, -f$2; }
# hp of T (nearest target) from frame a to frame b: "frame:hp" at every change
hpseries() {
    grep "\] trace" "$out/$1.log" | sed -n "$(( $(f $2) + 1 )),$(( $(f $3) + 1 ))p" |
        sed -n 's/^\[f\([0-9]*\)\].* nhp=\([-0-9]*\).*/\1 \2/p' | awk 'p != $2 { printf "%s:%s ", $1, $2; p = $2 }'
}
hp() { field "$1" "$2" nhp; }

# The scene: T straight up (within 16 px), 200..276 px away (beyond 187, within the range).
check_scene() {
    n=$(field "$1" 1320 nearest); range=$(field "$1" 1320 range)
    [ -n "$n" ] || fail "$1: no trace at frame 1320"
    nx=$(xy "$n" 1); ny=$(xy "$n" 2)
    [ "$range" = 280 ] || fail "$1: weapon range $range (expected the laser's 280)"
    [ $nx -ge -16 ] && [ $nx -le 16 ] && [ $ny -le -200 ] && [ $ny -ge -270 ] || fail "$1: T at $n"
    [ $(( -ny * 3 / 2 )) -gt "$range" ] || fail "$1: T within the old metric aim point"
}

if [ "$mode" = lock ]; then
    # A: twin stick, no reticle; fire 2.5 s; L2/R2 analog zoom afterwards.
    run A 1600 gromada_input_scheme=Diretto gromada_reticle=Nascosto rstick=0,-1@$(r 1330 1480) \
        r2a=0.54@$(r 1540 1545) l2a=1@$(r 1550 1555) btn=r2@$(r 1560 1565) shots=$(f 1329),$(f 1500)
    check_scene A
    [ "$(field A 1400 locked)" = 1 ] && [ "$(field A 1400 hidden)" = 1 ] || fail "A: not locked or reticle shown"
    [ "$(field A 1400 mouse)" = "$(field A 1320 mouse)" ] || fail "A: the aim point moved"
    cur=$(field A 1400 cursor) # the reticle (hidden) is on the border when T is off screen
    offscreen=no; [ "$(xy "$cur" 2)" -le 3 ] && offscreen=yes
    [ "$(field A 1542 zoom)" = 50 ] && [ "$(field A 1552 zoom)" = -100 ] && [ "$(field A 1562 zoom)" = 100 ] ||
        fail "A: analog zoom $(field A 1542 zoom) $(field A 1552 zoom) $(field A 1562 zoom)"
    [ "$(field A 1562 bits)" = 0 ] || fail "A: R2 clicked in the twin-stick scheme"
    hpA=$(hpseries A 1320 1599); hp0=$(hp A 1320); hp1=$(hp A 1599)
    [ $(( hp0 - hp1 )) -ge 12 ] || fail "A: T not hit ($hp0 -> $hp1)"

    # B: cursor scheme. Same menus (the cursor starts where the twin stick put it), then the
    # cursor at the top edge scrolls the view to T and the mouse puts it on T. Two passes:
    # the first gives the camera and cursor, the second moves the mouse onto T and fires.
    bopts="mouse=0,-160@$(f 5) mouse=0,-1000@$(f 1220) mouse=0,100@$(f 1250)"
    # shellcheck disable=SC2086
    run B1 1310 $bopts
    cam=$(field B1 1299 cam); cur=$(field B1 1299 cursor); mouse=$(field B1 1299 mouse); t=$(field A 1320 mouse)
    viewx=$(( $(xy "$cur" 1) + $(xy "$cam" 1) - $(xy "$mouse" 1) ))
    dx=$(( $(xy "$t" 1) - $(xy "$cam" 1) + viewx - $(xy "$cur" 1) )); dy=$(( $(xy "$t" 2) - $(xy "$cam" 2) - $(xy "$cur" 2) ))
    # shellcheck disable=SC2086
    run B 1600 $bopts mouse=$dx,$dy@$(f 1300) btn=r2@$(r 1330 1480) btn=l2@$(r 1500 1501) \
        btn=a@$(r 1530 1545) btn=b@$(r 1560 1575) shots=$(f 1400)
    check_scene B
    [ "$(field B 1400 mouse)" = "$t" ] || fail "B: the cursor is not on T ($(field B 1400 mouse) vs $t)"
    [ "$(field B 1400 bits)" = 32 ] || fail "B: R2 is not the right button (bits $(field B 1400 bits))"
    # A in the main menu (ctx 1, no zoom) clicks: the player menu loads right after the press
    [ "$(field B 160 ctx)" = 1 ] && [ "$(field B 160 zoom)" = 0 ] || fail "B: A in the menu"
    lf=$(grep 'load maps\\player.map' "$out/B.log" | head -1 | sed 's/^\[f\([0-9]*\)\].*/\1/')
    [ -n "$lf" ] && [ "$lf" -ge "$(f 160)" ] && [ "$lf" -le "$(f 163)" ] || fail "B: A does not click in the menus (player menu at $lf)"
    hpB=$(hpseries B 1320 1599)
    [ "$hpA" = "$hpB" ] || fail "different damage: twin stick $hpA, cursor $hpB"
    # L2 = left click: the Kassandra drives to the cursor (towards T, north)
    [ $(( $(field B 1500 bits) & 1 )) = 1 ] || fail "B: L2 is not the left click"
    pB0=$(field B 1499 flagman); pB1=$(field B 1599 flagman)
    [ $(( $(xy "$pB0" 2) - $(xy "$pB1" 2) )) -ge 10 ] || fail "B: L2 did not drive ($pB0 -> $pB1)"
    # A/B held = zoom in/out, no clicks
    [ "$(field B 1535 zoom)" = 100 ] && [ "$(field B 1565 zoom)" = -100 ] && [ "$(field B 1585 zoom)" = 0 ] ||
        fail "B: A/B zoom $(field B 1535 zoom) $(field B 1565 zoom)"
    [ $(( $(field B 1535 bits) & 0x33 )) = 0 ] && [ $(( $(field B 1565 bits) & 0x33 )) = 0 ] || fail "B: A/B clicked in the level"

    # C: the lock off: full range straight up, past T.
    run C 1600 gromada_input_scheme=Diretto gromada_aim_assist=Disattivata rstick=0,-1@$(r 1330 1480)
    check_scene C
    [ "$(field C 1400 locked)" = 0 ] || fail "C: locked with the lock off"
    m=$(field C 1400 mouse); p=$(field C 1400 flagman)
    [ $(( $(xy "$p" 2) - $(xy "$m" 2) )) = 280 ] || fail "C: aim point $m not at full range from $p"
    hpC=$(hpseries C 1320 1599)
    echo "OK lock: T $(field A 1320 nearest) (off screen: $offscreen); twin stick and cursor+R2 hp $hpA; lock off (rays stopped on the way) $hpC;" \
         "L2 drove $pB0 -> $pB1; zoom A/B and L2/R2 analog"
    exit 0
fi

# ballistic: R1 selects slot 2 (HAIL) after the drive; the stick 15 degrees right of north.
stick="rstick=0.259,-0.966@$(r 1330 1420)"
run H 1600 gromada_input_scheme=Diretto gromada_reticle=Nascosto btn=r@$(r 1200 1202) "$stick" shots=$(f 1420)
run N 1600 gromada_input_scheme=Diretto gromada_aim_assist=Disattivata btn=r@$(r 1200 1202) "$stick"
run L 1600 gromada_input_scheme=Diretto gromada_reticle=Nascosto "$stick"
for x in H N L; do check_scene $x; done
for x in H N; do
    [ "$(field $x 1340 weapon)" = 1 ] && [ "$(field $x 1340 kind)" = 2 ] ||
        fail "$x: HAIL not selected (weapon $(field $x 1340 weapon), kind $(field $x 1340 kind))"
done
[ "$(field L 1340 kind)" = 1 ] || fail "L: laser kind $(field L 1340 kind)"
t=$(field L 1320 mouse) # T's point (locked with the stick idle, straight up)
# T is 40..50 px from the aim line: inside the wide corridor only
[ "$(field H 1340 locked)" = 1 ] && [ "$(field H 1340 mouse)" = "$t" ] || fail "H: T not locked in the wide corridor"
[ "$(field L 1340 locked)" = 0 ] || fail "L: T locked in the narrow corridor"
# without a lock the aim point is the full range (280) along the stick
for x in N L; do
    [ "$(field $x 1340 locked)" = 0 ] || fail "$x: locked"
    m=$(field $x 1340 mouse); p=$(field $x 1340 flagman)
    dx=$(( $(xy "$m" 1) - $(xy "$p" 1) )); dy=$(( $(xy "$p" 2) - $(xy "$m" 2) ))
    [ $dx -ge 70 ] && [ $dx -le 75 ] && [ $dy -ge 268 ] && [ $dy -le 272 ] || fail "$x: aim point $m from $p"
done
hH=$(hpseries H 1320 1599); hN=$(hpseries N 1320 1599); hL=$(hpseries L 1320 1599)
[ $(( $(hp H 1320) - $(hp H 1599) )) -ge 45 ] || fail "H: the shell did not land on T ($hH)"
[ "$(hp N 1320)" = "$(hp N 1599)" ] || fail "N: the unlocked shell hit T ($hN)"
echo "OK ballistic: HAIL locked T 15 degrees off the stick, hp $hH; HAIL unlocked (full range) $hN; laser not locked $hL"
