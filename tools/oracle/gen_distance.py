"""Game distance max(|dx|, 3|dy|>>1): Distance(sprite) 0x40aff0, Distance(x, y) 0x40afc0, and the
script builtin SizeTo(unit, x, y) (id 65, 0x412bcc) through the VM."""
from common import INT_MAX, INT_MIN, edge_ints, family, pushi, rng, vm_program
from structs import World

DIST_SPRITE, DIST_POINT = 0x40aff0, 0x40afc0


def coords16(r, n):
    out = []
    for _ in range(n):
        kind = r.random()
        if kind < 0.6:
            out.append(tuple(r.randrange(0, 4096) for _ in range(4)))
        elif kind < 0.9:
            out.append(tuple(r.randrange(-32768, 32768) for _ in range(4)))
        else:
            out.append(tuple(r.choice([-32768, 32767, 0, -1, 1, 21845, -21846]) for _ in range(4)))
    return out


def generate(o):
    r = rng("distance")
    w = World(o)
    cases = []
    for ax, ay, bx, by in coords16(r, 2000):
        w.reset()
        a = w.sprite({"x": ax, "y": ay})
        b = w.sprite({"x": bx, "y": by})
        cases.append([ax, ay, bx, by, o.call(DIST_SPRITE, [b], conv="thiscall", this=a)])
    sprite_rec = family("distance_sprite", "Distance 0x40aff0 (thiscall this=sprite, arg=other sprite; s16 x/y at +0xc/+0xe)",
                        ["x", "y", "otherX", "otherY", "result"], cases)

    cases = []
    pts = [(ax, ay, bx, by) for ax, ay, bx, by in coords16(r, 1200)]
    pts += [(r.randrange(-32768, 32768), r.randrange(-32768, 32768), r.randrange(INT_MIN, INT_MAX + 1),
             r.randrange(INT_MIN, INT_MAX + 1)) for _ in range(300)]
    pts += [(0, 0, x, y) for x in (INT_MIN, INT_MAX, 0) for y in (INT_MIN, INT_MAX, 0, 1431655765, 1431655766)]
    for ax, ay, x, y in pts:
        w.reset()
        a = w.sprite({"x": ax, "y": ay})
        cases.append([ax, ay, x, y, o.call(DIST_POINT, [x, y], conv="thiscall", this=a)])
    point_rec = family("distance_point", "Distance 0x40afc0 (thiscall this=sprite, args int32 x, y)",
                       ["x", "y", "px", "py", "result"], cases)

    cases = []
    vals = edge_ints() + [r.randrange(-5000, 5000) for _ in range(40)]
    for i in range(800):
        ax, ay = r.randrange(-32768, 32768), r.randrange(-32768, 32768)
        x, y = r.choice(vals), r.choice(vals)
        null_unit = 1 if i % 50 == 0 else 0
        w.reset()
        a = 0 if null_unit else w.sprite({"x": ax, "y": ay})
        _, res = vm_program(o, pushi(a) + pushi(x) + pushi(y) + bytes([65]))
        cases.append([null_unit, ax, ay, x, y, res])
    sizeto_rec = family("sizeto", "script builtin SizeTo(unit, x, y) id 65 (0x412bcc) through the VM",
                        ["nullUnit", "unitX", "unitY", "x", "y", "result"], cases,
                        notes="x and y are truncated to s16; a null unit returns 60000")
    return [sprite_rec, point_rec, sizeto_rec]
