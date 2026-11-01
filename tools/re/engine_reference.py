#!/usr/bin/env python3
"""Executable reference of the integer formulas used by gromada.exe for the simulation.

Every function cites the address of the original code (see docs/re/engine.md).
Run `python3 -I tools/re/engine_reference.py` to execute the self tests; with the game data
available ($GROMADA_DATA or /root/gromada-data/game) it also applies the movement formula to
real vids (Gun tank, Fighter, Kassandra) and checks the facts quoted in the document.
"""
import math
import os
import sys

# 0x428e14 / 0x429214: 256-entry direction tables in .data (dir 0 = north, 64 = east, clockwise)
# The exe tables are exactly int() (truncation toward zero) of 1024*sin / -1024*cos (verified: 0 mismatches)
SIN = [int(1024 * math.sin(2 * math.pi * i / 256)) for i in range(256)]  # dx per unit, 0x428e14
NEG_COS = [-int(1024 * math.cos(2 * math.pi * i / 256)) for i in range(256)]  # dy, 0x429214
CORDIC_ANGLES = [64, 37, 19, 10, 5, 2, 1]  # 0x429e18, 1/512 of a turn
GRAVITY = 174  # 0x409ccb: vz -= 174*dt/1024
CLOCK_STEP_CAP_MS = 71  # 0x401fcd: the game clock advances at most 71 ms per frame


def asr(v, n):
    """Arithmetic shift right like x86 `sar` on a 32 bit int."""
    return v >> n


def to_s32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def get_direction(x1, y1, x2, y2):
    """0x40b030: CORDIC atan2 -> (direction 0..255, distance). Distance goes to global 0x439d4c."""
    x, y = x2 - x1, y2 - y1
    if x < 0:
        x, y, angle = -x, -y, 384
    else:
        angle = 128
    for i in range(7):
        ox = x
        if y >= 0:
            x += asr(y, i)
            y -= asr(ox, i)
            angle += CORDIC_ANGLES[i]
        else:
            x -= asr(y, i)
            y += asr(ox, i)
            angle -= CORDIC_ANGLES[i]
    distance = asr(to_s32(x * 636750), 20)
    return (angle >> 1) & 0xFF, distance


def chebyshev_distance(ax, ay, bx, by):
    """0x40aff0 / 0x40afc0: max(|dx|, 3*|dy|/2) used for weapon range and visibility."""
    return max(abs(ax - bx), (abs(ay - by) * 3) >> 1)


def clock_advance(clock, last_real, now_real):
    """0x401f70: per frame the game clock 0x42bf94 advances by min(real dt, 71) ms."""
    dt = (now_real - last_real) & 0xFFFFFFFF
    return clock + (CLOCK_STEP_CAP_MS if dt > CLOCK_STEP_CAP_MS else dt)


def tick_period(frame_duration, randomized, rand15=None):
    """0x40bc55: period of a sprite = vid.frameDuration (+ jitter if flag Randomized 0x100)."""
    p = frame_duration
    if randomized:
        p += (p >> 3) - rand15 % ((p >> 2) + 1)
    return p


def integrate_speed(speed, accel, max_speed, dt, accelerating):
    """0x409d02..0x409d7c: speed (+0x22) update. 9999 means 'instantaneous'."""
    if accelerating:
        if speed < max_speed:
            speed = max_speed if accel == 9999 else speed + ((accel * dt) >> 10)
    elif speed > 0:
        speed = 0 if accel == 9999 else speed - ((accel * dt) >> 9)
        speed = max(speed, 0)
    return speed


def move_step(x, y, fx, fy, direction, speed, dt):
    """0x409e7c..0x40a00e: horizontal move with 7-bit fractional accumulators (+0x26/+0x27)."""
    d = to_s32(speed * dt)
    ax = asr(to_s32(d * SIN[direction]), 13) + fx
    ay = asr(to_s32(d * NEG_COS[direction]), 13) + fy
    fx = (ax & 0x7F)
    fy = (ay & 0x7F)
    # the fraction bytes are signed chars in the sprite; & 0x7f keeps them in 0..127
    return x + asr(ax, 7), y + asr(ay, 7), fx, fy


def vertical_step(z, vz, fz, dt, gravity):
    """0x409cc5 + 0x409d7c: gravity then z integration (+0x24 vz, +0x28 fraction)."""
    if gravity and z:
        vz -= (dt * GRAVITY) >> 10
    a = asr(to_s32(vz * dt), 3) + fz
    return z + asr(a, 7), vz, a & 0x7F


def selftest():
    assert get_direction(0, 0, 0, -100)[0] in (0, 255, 1)
    assert abs(get_direction(0, 0, 100, 0)[0] - 64) <= 1
    assert abs(get_direction(0, 0, 0, 100)[0] - 128) <= 1
    assert abs(get_direction(0, 0, -100, 0)[0] - 192) <= 1
    d = get_direction(0, 0, 300, 400)[1]
    assert abs(d - 500) <= 3, d
    assert clock_advance(1000, 0, 500) == 1071
    assert clock_advance(1000, 0, 40) == 1040
    # speed is in pixels per 1024 ms: 80 px/1.024 s moving east for 1024 ms -> +80 px
    x, y, fx, fy = 0, 0, 0, 0
    for _ in range(1024 // 64):
        x, y, fx, fy = move_step(x, y, fx, fy, 64, 80, 64)
    assert x in (79, 80) and y == 0, (x, y)
    print("formula self tests: ok")


def data_checks():
    root = os.environ.get("GROMADA_DATA", "/root/gromada-data/game")
    res = os.path.join(root, "fw.res")
    if not os.path.exists(res):
        print("fw.res not found: data checks skipped")
        return
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import vid_stats  # noqa: E402
    exe = os.path.join(root, "gromada.exe")
    if os.path.exists(exe):
        import struct
        raw = open(exe, "rb").read()  # .data is mapped 1:1 (raw 0x28000 == va 0x428000)
        assert list(struct.unpack_from("<256i", raw, 0x28e14)) == SIN
        assert list(struct.unpack_from("<256i", raw, 0x29214)) == NEG_COS
        assert list(raw[0x29e18:0x29e1f]) == CORDIC_ANGLES
        print("direction tables and CORDIC angles match gromada.exe")
    data = open(res, "rb").read()
    vids = [vid_stats.parse_vid(data, b) for t, n, b, e in vid_stats.sections(data) if t == ord("!")]
    for v in vids:
        if v["frameDuration"] is None:
            v["frameDuration"] = vids[v["graphicsFrom"]]["frameDuration"]
    durations = [v["frameDuration"] for v in vids]
    most = max(set(durations), key=durations.count)
    print(f"most common frameDuration: {most} ms ({durations.count(most)}/{len(vids)} vids)")
    for nvid in (19, 46, 81):
        v = vids[nvid]
        period = v["frameDuration"]
        x, y, fx, fy = 0, 0, 0, 0
        speed, t = 0, 0
        while t < 3000:
            speed = integrate_speed(speed, v["accel"], v["speedX"], period, True)
            x, y, fx, fy = move_step(x, y, fx, fy, 64, speed, period)
            t += period
        print(f"vid {nvid:3} {v['name'][:24]:24} tick {period} ms, speedX {v['speedX']}, "
              f"accel {v['accel']} -> {x} px east after {t} ms (~{v['speedX'] * 1000 // 1024} px/s at top speed)")


if __name__ == "__main__":
    selftest()
    data_checks()
