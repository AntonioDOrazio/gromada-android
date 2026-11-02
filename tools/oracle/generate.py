#!/usr/bin/env python3
"""Regenerate the oracle test vectors in tests/data/oracle/ from the original gromada.exe.

Usage (needs the game: $GROMADA_DATA/gromada.exe or $GROMADA_EXE):
    /root/tools/oracle-venv/bin/python -I tools/oracle/generate.py [family-module ...]

Without arguments every gen_*.py module is run. Each module exposes generate(oracle) and
returns a list of records (common.family); each record is written to
tests/data/oracle/<family>.json. Inputs come from deterministic per-family generators, so
running the script twice produces identical files.
"""
import importlib
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from common import dump, install_error_traps  # noqa: E402
from oracle import Oracle  # noqa: E402

MODULES = ["gen_rand", "gen_direction", "gen_tables", "gen_distance", "gen_physics",
           "gen_setdest", "gen_turnto", "gen_anim", "gen_checkmove", "gen_damage",
           "gen_vm", "gen_timing", "gen_commands",
           # issue #12
           "gen_collide", "gen_weapons", "gen_actions",
           # issue #18
           "gen_sound"]


def main():
    out_dir = os.path.join(HERE, "..", "..", "tests", "data", "oracle")
    os.makedirs(out_dir, exist_ok=True)
    mods = sys.argv[1:] or MODULES
    o = Oracle()
    total = 0
    for name in mods:
        t = time.time()
        o.reset_data()
        o.heap_reset()
        o.calls.clear()
        o.clear_internal_stubs()
        install_error_traps(o)
        mod = importlib.import_module(name)
        for rec in mod.generate(o):
            path = os.path.join(out_dir, rec["family"] + ".json")
            dump(rec, path)
            size = os.path.getsize(path)
            total += size
            print(f"{rec['family']:20} {len(rec['cases']):6} cases {size / 1024:8.1f} KiB  ({name}, {time.time() - t:.1f}s)")
    print(f"total written: {total / 1024:.1f} KiB")


if __name__ == "__main__":
    main()
