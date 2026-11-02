#!/usr/bin/env python3
"""Compare tools/re/engine_reference.py (static-analysis reference, issue #2) with the oracle
vectors of tests/data/oracle (emulated original). Prints mismatch counts and examples.
No game data needed.

    python3 -I tools/oracle/check_engine_reference.py
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "re"))
import engine_reference as er  # noqa: E402

DATA = os.path.join(HERE, "..", "..", "tests", "data", "oracle")


def load(name):
    with open(os.path.join(DATA, name + ".json")) as f:
        d = json.load(f)
    cols = {c: i for i, c in enumerate(d["columns"])}
    return d, [dict((c, row[i]) for c, i in cols.items()) for row in d["cases"]]


def report(name, bad, total, examples):
    print(f"{name:40} {total - bad:6}/{total} match")
    for e in examples[:3]:
        print("    e.g.", e)


def main():
    d, rows = load("tables")
    arr = d["arrays"]
    ok = arr["sin"] == er.SIN and arr["negCos"] == er.NEG_COS and arr["cordicAngles"] == er.CORDIC_ANGLES
    print(f"{'SIN / NEG_COS / CORDIC_ANGLES':40} {'identical' if ok else 'DIFFERENT'}")

    _, rows = load("direction")
    bad, ex = 0, []
    bad_unmasked = 0
    for r in rows:
        dd, dist = er.get_direction(r["x1"], r["y1"], r["x2"], r["y2"])
        if dd != r["direction"] & 0xFF or dist != r["distance"]:
            bad += 1
            ex.append((r, dd, dist))
        if dd != r["direction"]:
            bad_unmasked += 1
    report("get_direction (direction & 0xff, distance)", bad, len(rows), ex)
    print(f"    raw return value differs (not masked by the original) in {bad_unmasked} cases")

    _, rows = load("distance_sprite")
    bad = sum(er.chebyshev_distance(r["x"], r["y"], r["otherX"], r["otherY"]) != r["result"] for r in rows)
    report("chebyshev_distance (0x40aff0)", bad, len(rows), [])

    _, rows = load("clock")
    bad = sum(er.clock_advance(r["curclock"], r["lastReal"], r["timeGetTime"]) & 0xFFFFFFFF != r["curclock'"] for r in rows)
    report("clock_advance", bad, len(rows), [])

    _, rows = load("physics")
    bad_speed, bad_move, n_speed, n_move, ex_s, ex_m = 0, 0, 0, 0, [], []
    for r in rows:
        if r["state"] & 0x40:
            continue
        dt = (r["curclock"] - r["lastTact"]) & 0xFFFFFFFF
        speed = er.integrate_speed(r["speed"], r["acceleration"], r["speedX"], dt, bool(r["state"] & 0x100))
        n_speed += 1
        if speed != r["speed'"]:
            bad_speed += 1
            ex_s.append(dict(speed=r["speed"], accel=r["acceleration"], speedX=r["speedX"], dt=dt,
                             accelerating=bool(r["state"] & 0x100), ref=speed, original=r["speed'"]))
        # plain horizontal move: no wind, not blocked, inside the map, not a beam, moving
        if (r["flags"] & 0x1000) or r["speed'"] in (0, 9999) or r["nActions"] or r["obstacleCalls"]:
            continue
        n_move += 1
        x, y, fx, fy = er.move_step(r["x"], r["y"], r["fracX"], r["fracY"], r["direction"], r["speed'"], dt)
        if (er.to_s32(x) & 0xFFFF, y & 0xFFFF, fx, fy) != (r["x'"] & 0xFFFF, r["y'"] & 0xFFFF, r["fracX'"], r["fracY'"]):
            bad_move += 1
            ex_m.append((r["x"], r["y"], r["speed'"], dt, (x, y, fx, fy), (r["x'"], r["y'"], r["fracX'"], r["fracY'"])))
    report("integrate_speed", bad_speed, n_speed, ex_s)
    report("move_step (no wind, free move)", bad_move, n_move, ex_m)

    _, rows = load("tick_schedule")
    bad = 0
    for r in rows:
        if r["flags"] & 8:
            continue
        state = r["seed"]
        rand15 = None
        if r["flags"] & 0x100:
            state = (state * 214013 + 2531011) & 0xFFFFFFFF
            rand15 = (state >> 16) & 0x7FFF
        p = er.tick_period(r["frameDuration"], bool(r["flags"] & 0x100), rand15)
        due = ((r["curclock"] - r["lastTact"]) & 0xFFFFFFFF) >= p
        bad += due != bool(r["ticked"])
    report("tick_period + due check", bad, len(rows), [])


if __name__ == "__main__":
    main()
