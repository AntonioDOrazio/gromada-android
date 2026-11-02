#!/bin/sh
# Save states end to end through the core and the headless frontend (issue #14), on the original
# data: the flow of core_e2e.sh (logo, movies, menus, new player, mission map, level 1 played
# with the pad) with
#   1. save in level 1, play on (L2 move, R2 fire, A zoom: issue #19), load, replay the same
#      input: every frame (picture and sound) identical, music and sound effects included;
#   2. the same across a map change (save on the mission map, load after level 1 started);
#   3. the same inside a menu (player choice, name entry);
#   4. a state saved during a movie, loaded by another process at its start: identical frames;
#   5. run-ahead (every frame: run, save, run one more, load) over the whole flow: the real
#      frames equal a plain run, each run-ahead frame equals the plain run's next frame;
#   6. the direct control scheme (camera follow, aim assist): reload and run-ahead;
#   7. corrupted states refused without changing anything (statefuzz).
# usage: savestate_e2e.sh <harness> <core.so> <out_dir> [extra harness options]
# Exit code 77 (skipped) without GROMADA_DATA. Output in <out_dir> (never commit it).
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
input="btn=a@$(r 10 12) btn=a@$(r 40 42) btn=x@$(r 80 82) btn=a@$(r 160 162) btn=x@$(r 420 422) btn=a@$(r 460 462)
       btn=down@$(r 480 482) btn=a@$(r 500 502) btn=start@$(r 540 545) btn=x@$(r 600 602) btn=x@$(r 800 803)
       btn=x@$(r 1300 1303) btn=l2@$(r 1320 1330) lstick=0.5,0.2@$(r 1310 1340) btn=r2@$(r 1350 1380) btn=r@$(r 1390 1391)
       btn=a@$(r 1395 1410)"
fail=0
run() { # name frames options...
    name=$1; frames=$2; shift 2
    # the game's fw.ini has the CD music off (4th line): on here, so the sound checks cover it
    mkdir -p "$out/save_$name/gromada"
    printf '1\r\n1\r\n50\r\n80\r\n1\r\n' > "$out/save_$name/gromada/fw.ini"
    # shellcheck disable=SC2086
    "$harness" "$core" "$GROMADA_DATA/gromada.exe" "$frames" "$out/$name" savedir="$out/save_$name" $input \
        hashes="$out/$name.txt" "$@" 2> "$out/$name.log"
    status=$?
    grep 'savestates:\|statefuzz:' "$out/$name.log"
    if [ $status -ne 0 ]; then echo "FAILED: $name (harness exit $status)"; tail -5 "$out/$name.log"; fail=1; fi
}
# frames replayed after a load: same hashes as the first time
reload() { # name
    awk -v name="$1" '$1 != "ahead" { k = $1; v = $2 " " $3; if (k in seen) { n++; if (seen[k] != v) { d++; if (!first) first = k } } else seen[k] = v }
        END { printf "%s: %d frames replayed, %d differ%s\n", name, n, d, d ? " (first " first ")" : "";
              exit (n == 0 || d > 0) }' "$out/$1.txt" || fail=1
}

run level $(f 1600) savestate=$(f 1310) loadstate=$(f 1450) "$@"; reload level
run mapchange $(f 1100) savestate=$(f 820) loadstate=$(f 900) "$@"; reload mapchange
run menu $(f 600) savestate=$(f 300) loadstate=$(f 420) "$@"; reload menu
# a state saved inside the intro movie (1.avi), loaded by another process during the logo movie
run movie_a $(f 300) savestate=$(f 60) statefile="$out/movie.state" "$@"
run movie_b $(f 240) loadstate=$(f 5) statefile="$out/movie.state" "$@"
cat "$out/movie_a.txt" > "$out/movie.txt"
awk -v from=$(f 5) '$1 != "ahead" && $1 >= from' "$out/movie_b.txt" >> "$out/movie.txt"
reload movie
# run-ahead against a plain run
runahead() { # plain runahead
    awk -v name="$2" 'FNR == NR { ref[$1] = $2 " " $3; vid[$1] = $2; next }
         $1 == "ahead" { if (!($2 in vid)) next; a++; if (vid[$2] != $3) ad++; next }
         { n++; if (ref[$1] != $2 " " $3) d++ }
         END { printf "%s: %d real frames, %d differ; %d run-ahead frames, %d differ\n", name, n, d, a, ad; exit (d > 0 || ad > 0 || n == 0) }' \
        "$out/$1.txt" "$out/$2.txt" || fail=1
}
run plain $(f 1500) "$@"
run runahead $(f 1500) runahead=1 "$@"
runahead plain runahead
# the direct control scheme (issue #16: camera following the player, aim assist, weapon
# requests, move orders) with the input of controls_e2e.sh: drive, turn, aim and fire
input="btn=a@$(r 10 12) btn=a@$(r 40 42) btn=x@$(r 80 82) btn=a@$(r 160 162) btn=x@$(r 420 422) btn=a@$(r 460 462)
       btn=down@$(r 480 482) btn=a@$(r 500 502) btn=start@$(r 540 545) btn=x@$(r 600 602) btn=x@$(r 800 803)
       btn=x@$(r 1300 1303) btn=r@$(r 1350 1352) btn=l@$(r 1370 1372)
       lstick=1,0@$(r 1400 1459) lstick=0,-1@$(r 1460 1609) rstick=0,-1@$(r 1740 1900)"
run direct $(f 2000) gromada_input_scheme=Diretto savestate=$(f 1440) loadstate=$(f 1800) "$@"; reload direct
run direct_plain $(f 1920) gromada_input_scheme=Diretto "$@"
run direct_runahead $(f 1920) gromada_input_scheme=Diretto runahead=1 "$@"
runahead direct_plain direct_runahead
# corrupted states
run fuzz $(f 1400) statefuzz=400 "$@"
if [ $fail -ne 0 ]; then echo "FAILED"; exit 1; fi
echo "OK"
