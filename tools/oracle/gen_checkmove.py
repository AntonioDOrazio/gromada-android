"""CheckMove(x, y, z) 0x40a5c0 (thiscall, ret 0xc) on a synthetic spatial grid, using the real
grid query/next functions 0x40d6d0/0x40d7a0. Crushable objects receive a recorded virtual
Action(act_damage 43, 2, 0)."""
from common import family, rng
from structs import Grid, World

CHECK_MOVE = 0x40a5c0
SHIFT, NOX, NOY = 5, 64, 64

SELF_COLS = ["category", "flags", "collisionMask", "sizeX", "sizeY", "sizeZ", "selfX", "selfY", "selfInGrid",
             "argX", "argY", "argZ"]
OUT_COLS = ["blocker", "crushedMask"]
OBJ_COLS = ["x", "y", "z", "action", "category", "flags", "collisionMask", "sizeX", "sizeY", "sizeZ", "cellX", "cellY"]


def rand_size(r):
    return r.choices([r.randrange(8, 64), r.randrange(0, 8), r.randrange(-20, 0), 24, 32, 48], [6, 1, 0.5, 1, 1, 1])[0]


def generate(o, n=1200):
    r = rng("checkmove")
    w = World(o, nvids=16)
    cases = []
    for _ in range(n):
        w.reset()
        o.calls.clear()
        grid = Grid(o, SHIFT, SHIFT, NOX, NOY)
        sc = {"category": r.choices([4, 8, 16, 2, 1, 0x0c, 0x14], [6, 2, 2, 0.5, 0.3, 0.4, 0.3])[0],
              "flags": r.choices([0, 0x40, 0x20, 0x4000], [3, 5, 1, 0.5])[0],
              "collisionMask": r.choice([0, 1, 2, 3, 4, 0xff, 0x0f, 0x10]),
              "sizeX": rand_size(r), "sizeY": rand_size(r), "sizeZ": r.choices([0, 10, 20, 30, -20, r.randrange(-50, 80)], [0.5, 2, 2, 2, 0.5, 1])[0]}
        sx, sy = r.randrange(40, NOX * 32 - 40), r.randrange(40, NOY * 32 - 40)
        ax, ay = sx + r.randrange(-8, 9), sy + r.randrange(-8, 9)
        az = r.choices([0, r.randrange(1, 100), r.randrange(-5, 0), 80], [6, 2, 0.3, 1])[0]
        if r.random() < 0.02:
            ax, ay = r.randrange(-100, 0), r.randrange(-100, 2100)  # negative cells are clamped to 0
        w.set_vid(1, sc)
        self_spr = w.sprite({"x": sx, "y": sy, "z": az, "nvid": 1, "action": 0})
        w.put(self_spr, "vtable", w.vtable("self"))
        in_grid = 1 if r.random() < 0.7 else 0
        objs = []
        sprites = {}
        nobj = r.choices(range(7), [1, 3, 3, 2, 1, 1, 1])[0]
        order = list(range(nobj))
        for i in range(nobj):
            terrain = r.random() < 0.3
            oc = {"category": 1 if terrain else r.choice([4, 8, 16, 2, 0x0c]),
                  "flags": r.choices([0x40, 0x20, 0x4040, 0x60], [6, 1, 2.5, 0.3])[0],
                  "collisionMask": r.choice([0, 1, 2, 3, 4, 0xff, 0x0f, 0x10]),
                  "sizeX": 24 if terrain else rand_size(r), "sizeY": 16 if terrain else rand_size(r),
                  "sizeZ": 0 if terrain else r.choices([0, 10, 20, 30, -20, r.randrange(-50, 80)], [0.5, 2, 2, 2, 0.5, 1])[0]}
            near = r.random() < 0.85
            ox = ax + (r.randrange(-20, 21) if near else r.randrange(-300, 301))
            oy = ay + (r.randrange(-20, 21) if near else r.randrange(-300, 301))
            ox, oy = max(0, min(NOX * 32 - 1, ox)), max(0, min(NOY * 32 - 1, oy))
            oz = 0 if terrain else r.choices([0, az, az + r.randrange(-40, 41), r.randrange(0, 120)], [3, 4, 3, 1])[0]
            action = r.choices([0, 2, 15, 8], [5, 3, 1, 1])[0]
            cx, cy = ox >> SHIFT, oy >> SHIFT
            if r.random() < 0.05:
                cx, cy = r.randrange(NOX), r.randrange(NOY)  # misfiled: only the cell matters to the query
            w.set_vid(2 + i, oc)
            s = w.sprite({"x": ox, "y": oy, "z": oz, "nvid": 2 + i, "action": action})
            w.put(s, "vtable", w.vtable(("obj", i)))
            sprites[s] = i
            objs.append([ox, oy, oz, action, oc["category"], oc["flags"], oc["collisionMask"], oc["sizeX"], oc["sizeY"],
                         oc["sizeZ"], cx, cy])
        # insertion order inside a cell: self first (if present), then the objects in index order
        if in_grid:
            grid.add(sx >> SHIFT, sy >> SHIFT, self_spr)
        for i in order:
            grid.add(objs[i][10], objs[i][11], [s for s, k in sprites.items() if k == i][0])
        grid.commit()
        res = o.call(CHECK_MOVE, [ax, ay, az], conv="thiscall", this=self_spr) & 0xFFFFFFFF
        blocker = -1 if res == 0 else (-2 if res == self_spr else sprites[res])
        crushed = 0
        for e in o.calls:
            tag = e[0]
            assert isinstance(tag, tuple) and e[2:] == [43, 2, 0], e
            crushed |= 1 << tag[1]
        case = [sc[k] for k in ("category", "flags", "collisionMask", "sizeX", "sizeY", "sizeZ")] + \
            [sx, sy, in_grid, ax, ay, az, blocker, crushed, nobj]
        for ob in objs:
            case += ob
        cases.append(case)
    rec = family("check_move", "CheckMove 0x40a5c0 (thiscall this=sprite, args int x, y, z; ret 0xc) with grid 0x431ab0",
                 SELF_COLS + OUT_COLS + ["nObjects"], [], notes=(
                     f"variable length cases: the fixed columns, then nObjects groups of objectColumns. Grid: cell shift "
                     f"{SHIFT} (x and y), {NOX}x{NOY} cells; each cell lists self first (when selfInGrid, in the cell of "
                     "selfX/selfY) then the objects in index order. blocker: object index, -1 none, -2 = this (z < 0)."))
    rec["objectColumns"] = OBJ_COLS
    rec["cases"] = cases
    return [rec]
