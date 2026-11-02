#!/bin/sh
# Game-level oracle (issue #13, docs/trace-oracle.md): replays the three demos with the original
# gromada.exe in emulation (tools/oracle/trace_game.py) and with our session + engine
# (game_trace), then compares the traces frame by frame (world, rand, camera, builtin calls,
# sound requests and channel operations). Any difference fails the test.
# usage: trace_oracle.sh <game_trace> <out_dir> [frames per demo, 0 = whole demo] [game_trace options]
# (e.g. "--present 2,2": the core draws every frame at internal resolution 2, zoom 2, smooth
# motion, issue #20; the trace must stay identical)
# Needs GROMADA_DATA and ORACLE_PYTHON (a python with unicorn and pefile, e.g.
# /root/tools/oracle-venv/bin/python); exit code 77 (skipped) otherwise. The traces contain
# game data: they stay in <out_dir> (build tree).
set -u
if [ -z "${GROMADA_DATA:-}" ] || [ ! -f "$GROMADA_DATA/gromada.exe" ]; then
    echo "SKIP (set GROMADA_DATA to the game folder)"; exit 77
fi
if [ -z "${ORACLE_PYTHON:-}" ] || ! "$ORACLE_PYTHON" -I -c "import unicorn, pefile" 2>/dev/null; then
    echo "SKIP (set ORACLE_PYTHON to a python with unicorn and pefile)"; exit 77
fi
trace=$1; out=$2; frames=${3:-1500}
shift 2; [ $# -gt 0 ] && shift
here=$(dirname "$0")
mkdir -p "$out"
status=0
for n in 1 2 3; do
    "$ORACLE_PYTHON" -I "$here/../tools/oracle/trace_game.py" $n --frames "$frames" --data "$GROMADA_DATA" \
        -o "$out/demo$n.orig.jsonl.gz" || exit 1
    "$trace" --data "$GROMADA_DATA" --demo $n --frames "$frames" -o "$out/demo$n.core.jsonl.gz" "$@" || exit 1
    "$ORACLE_PYTHON" -I "$here/../tools/oracle/compare_traces.py" "$out/demo$n.orig.jsonl.gz" \
        "$out/demo$n.core.jsonl.gz" --json "$out/demo$n.cmp.json" || status=1
done
exit $status
