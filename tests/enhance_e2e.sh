#!/bin/sh
# Graphics enhancements through the core (issue #21): demo 1 played three times for 1500
# frames with the frame hashes of the harness (video and audio FNV per frame):
#  - default options and the enhancement options explicitly "disattivate": identical video and
#    audio, frame by frame (effects off = bit-identical output);
#  - lights and contact shadows on: the video changes, the audio stays identical in every frame
#    (the demo drives the same simulation: presentation only).
# usage: enhance_e2e.sh <harness> <core.so> <out_dir> [extra harness options, e.g. gromada_fps=120]
# Exit code 77 (skipped) without GROMADA_DATA.
set -u
if [ -z "${GROMADA_DATA:-}" ] || [ ! -f "$GROMADA_DATA/fw.res" ]; then
    echo "SKIP (set GROMADA_DATA to the game folder)"; exit 77
fi
harness=$1; core=$2; out=$3; shift 3
rm -rf "$out"; mkdir -p "$out"
run() {
    name=$1; shift
    if ! "$harness" "$core" "$GROMADA_DATA/gromada.exe" 1500 "$out/$name" savedir=none gromada_demo=1 \
            hashes="$out/$name.txt" "$@" 2> "$out/$name.log"; then
        echo "FAILED: harness run $name"; tail -20 "$out/$name.log"; exit 1
    fi
}
run default "$@"
run off gromada_fx_lights=disattivate gromada_fx_contact_shadows=disattivate "$@"
run on gromada_fx_lights=normali gromada_fx_contact_shadows=normali "$@"
if ! cmp -s "$out/default.txt" "$out/off.txt"; then
    echo "FAILED: effects off differ from the default output"; diff "$out/default.txt" "$out/off.txt" | head; exit 1
fi
counts=$(paste -d' ' "$out/default.txt" "$out/on.txt" | awk '
    $1 != $4 { bad++ } $2 != $5 { video++ } $3 != $6 { audio++ }
    END { printf "%d %d %d %d", NR, video + 0, audio + 0, bad + 0 }')
set -- $counts
echo "frames $1: video changed in $2, audio changed in $3"
if [ "$1" -lt 1500 ] || [ "$4" -ne 0 ]; then echo "FAILED: frame lists differ"; exit 1; fi
if [ "$3" -ne 0 ]; then echo "FAILED: the effects changed the audio (the simulation)"; exit 1; fi
if [ "$2" -lt 500 ]; then echo "FAILED: the effects did not change the picture"; exit 1; fi
echo OK
