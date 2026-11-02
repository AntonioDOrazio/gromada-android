#!/bin/sh
# Rendering pipeline (issue #20, docs/rendering.md): with internal resolution 1x, zoom 1 and smooth
# motion off the core's output is the one it had before the pipeline (main at bedb79d), frame by
# frame (picture and sound): the flow of core_e2e.sh (logo, movies, menus, new player, mission
# map, briefing, level 1) played with the virtual cursor and with the direct scheme, at 4:3/60 fps
# and 16:9/120 fps. The reference is the MD5 of the harness's per-frame hash list ("hashes="),
# recorded with the core of bedb79d. A change of the game's behaviour merged later (another
# issue) changes these too: then regenerate them with the core of the new main and say so.
# usage: render_e2e.sh <harness> <core.so> <out_dir>
# Exit code 77 (skipped) without GROMADA_DATA.
set -u
if [ -z "${GROMADA_DATA:-}" ] || [ ! -f "$GROMADA_DATA/fw.res" ]; then
    echo "SKIP (set GROMADA_DATA to the game folder)"; exit 77
fi
harness=$1; core=$2; out=$3
rm -rf "$out"; mkdir -p "$out"
fail=0
run() { # name scheme fps aspect expected-md5
    name=$1; scheme=$2; fps=$3; aspect=$4; expected=$5
    scale=1; [ "$fps" = 120 ] && scale=2
    f() { echo $(( $1 * scale )); }
    r() { echo "$(f $1)-$(f $2)"; }
    base="btn=a@$(r 10 12) btn=a@$(r 40 42) btn=x@$(r 80 82) btn=a@$(r 160 162) btn=x@$(r 420 422) btn=a@$(r 460 462)
          btn=down@$(r 480 482) btn=a@$(r 500 502) btn=start@$(r 540 545) btn=x@$(r 600 602) btn=x@$(r 800 803)
          btn=x@$(r 1300 1303)"
    if [ "$scheme" = cursor ]; then
        input="$base btn=a@$(r 1320 1330) lstick=0.5,0.2@$(r 1310 1340) btn=b@$(r 1350 1380) btn=r@$(r 1390 1391)
               lstick=-1,0@$(r 1400 1500) lstick=1,1@$(r 1500 1600)"
        extra=""
    else
        input="$base btn=r@$(r 1350 1352) btn=l@$(r 1370 1372) lstick=1,0@$(r 1400 1459) lstick=0,-1@$(r 1460 1609)
               rstick=0,-1@$(r 1740 1900)"
        extra="gromada_input_scheme=Diretto"
    fi
    mkdir -p "$out/save_$name/gromada"
    printf '1\r\n1\r\n50\r\n80\r\n1\r\n' > "$out/save_$name/gromada/fw.ini"
    # shellcheck disable=SC2086
    "$harness" "$core" "$GROMADA_DATA/gromada.exe" "$(f 1950)" "$out/$name" savedir="$out/save_$name" $input $extra \
        gromada_fps=$fps gromada_aspect=$aspect gromada_render_scale=1x gromada_zoom=1x gromada_smooth=disattivato \
        hashes="$out/$name.txt" 2> "$out/$name.log" || { echo "FAILED: $name (harness)"; tail -5 "$out/$name.log"; fail=1; return; }
    got=$(md5sum < "$out/$name.txt" | cut -d' ' -f1)
    if [ "$got" = "$expected" ]; then
        echo "$name: $(wc -l < "$out/$name.txt") frames identical to the reference"
    else
        echo "FAILED: $name: frame hashes differ from the reference ($got)"; fail=1
    fi
}
run cursor_43_60 cursor 60 4:3 3a03454777b23954cc1fb5ea70fdd7c0
run direct_43_60 direct 60 4:3 3f34a8da00c74f4f419297335fffce2f
run cursor_169_120 cursor 120 16:9 a2cf47e8b41cb62af96d54685a98b663
run direct_169_120 direct 120 16:9 158475bd97230e12db78d3c234b3fc1c
if [ $fail -ne 0 ]; then echo "FAILED"; exit 1; fi
echo "OK"
