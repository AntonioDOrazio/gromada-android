"""GetDirection (0x40b030, cdecl, 4 x int32): CORDIC atan2 + euclidean distance in g_dist."""
import math

from common import INT_MAX, INT_MIN, family, rng
from structs import G_DIST

GET_DIRECTION = 0x40b030


def inputs():
    r = rng("direction")
    pts = []
    # every direction step at several radii around assorted origins (wraparound of 0/255)
    for radius in (1, 2, 3, 5, 8, 16, 24, 50, 100, 200, 500, 1000, 2047, 2048, 3000):
        for k in range(0, 256, 4 if radius > 3 else 16):
            a = 2 * math.pi * (k + 0.5 * (radius % 2)) / 256
            dx, dy = round(radius * math.sin(a)), round(-radius * math.cos(a))
            ox, oy = r.randrange(-500, 2500), r.randrange(-500, 2500)
            pts.append((ox, oy, ox + dx, oy + dy))
    # axes, diagonals and the zero vector
    for d in (0, 1, -1, 7, -7, 64, -64, 1000, -1000, 32767, -32768):
        for e in (0, 1, -1, d, -d):
            pts.append((0, 0, d, e))
            pts.append((d, e, 0, 0))
    # random map-sized coordinates (s16 sprite fields)
    for _ in range(1500):
        pts.append(tuple(r.randrange(0, 4096) for _ in range(4)))
    for _ in range(800):
        pts.append(tuple(r.randrange(-32768, 32768) for _ in range(4)))
    # full int32 range: CORDIC and the distance multiply wrap around
    for _ in range(300):
        pts.append(tuple(r.randrange(INT_MIN, INT_MAX + 1) for _ in range(4)))
    for v in (INT_MIN, INT_MAX, INT_MIN + 1, INT_MAX - 1):
        pts += [(0, 0, v, 0), (0, 0, 0, v), (v, v, 0, 0), (0, 0, v, v), (v, 0, -v, 0)]
    return pts


def generate(o):
    cases = []
    for x1, y1, x2, y2 in inputs():
        o.w32(G_DIST, 0x5A5A5A5A)
        d = o.call(GET_DIRECTION, [x1, y1, x2, y2])
        cases.append([x1, y1, x2, y2, d, o.s32(G_DIST)])
    return [family("direction", "GetDirection 0x40b030 (cdecl, args x1,y1,x2,y2; returns angle>>1 unmasked, distance in 0x439d4c)",
                   ["x1", "y1", "x2", "y2", "direction", "distance"], cases)]
