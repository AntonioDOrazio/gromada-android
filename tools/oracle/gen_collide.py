"""Collision queries of issue #12 (docs/re/engine.md §6):
  - AskPlace(x, y, z) 0x40a430 (thiscall, ret 0xc) and AskPlaceVid(x, y, z, nvid) 0x40a2d0
    (cdecl) on a synthetic grid, with the real grid query 0x40d6d0/0x40d7a0 (family ask_place);
  - FirstObstacleOnLine(x2, y2, z2) 0x40a770 (thiscall, ret 0xc) with AskPlace replaced by a
    recorder that blocks at a chosen probe (family first_obstacle: the probe sequence);
  - the same with the real AskPlace on a grid with objects near the line (first_obstacle_grid).
"""
from common import family, rng
from structs import Grid, World

ASK_PLACE, ASK_PLACE_VID, FIRST_OBSTACLE = 0x40a430, 0x40a2d0, 0x40a770
SHIFT, NOX, NOY = 5, 64, 64

OBJ_COLS = ["x", "y", "z", "action", "category", "flags", "collisionMask", "sizeX", "sizeY", "sizeZ", "cellX", "cellY"]


def rand_size(r):
    return r.choices([r.randrange(8, 64), r.randrange(0, 8), r.randrange(-20, 0), 24, 32, 48], [6, 1, 0.5, 1, 1, 1])[0]


def self_vid(r):
    return {"category": r.choices([4, 8, 16, 2, 1, 0x0c], [6, 2, 2, 0.5, 0.3, 0.4])[0],
            "flags": r.choices([0, 0x40, 0x20], [3, 5, 1])[0],
            "collisionMask": r.choice([0, 1, 2, 3, 4, 0xff, 0x0f, 0x10]),
            "sizeX": rand_size(r), "sizeY": rand_size(r),
            "sizeZ": r.choices([0, 10, 20, 30, -20, r.randrange(-50, 80)], [0.5, 2, 2, 2, 0.5, 1])[0]}


def add_objects(r, w, grid, nobj, around, az, sprites, objs):
    """Objects (vids 2..) filed in the grid near the points of `around`."""
    for i in range(nobj):
        terrain = r.random() < 0.3
        oc = {"category": 1 if terrain else r.choice([4, 8, 16, 2, 0x0c]),
              "flags": r.choices([0x40, 0x20, 0x4040, 0x60], [6, 1, 2.5, 0.3])[0],
              "collisionMask": r.choice([0, 1, 2, 3, 4, 0xff, 0x0f, 0x10]),
              "sizeX": 24 if terrain else rand_size(r), "sizeY": 16 if terrain else rand_size(r),
              "sizeZ": 0 if terrain else r.choices([0, 10, 20, 30, -20, r.randrange(-50, 80)], [0.5, 2, 2, 2, 0.5, 1])[0]}
        ax, ay = r.choice(around)
        near = r.random() < 0.85
        ox = ax + (r.randrange(-20, 21) if near else r.randrange(-300, 301))
        oy = ay + (r.randrange(-20, 21) if near else r.randrange(-300, 301))
        ox, oy = max(0, min(NOX * 32 - 1, ox)), max(0, min(NOY * 32 - 1, oy))
        oz = 0 if terrain else r.choices([0, az, az + r.randrange(-40, 41), r.randrange(0, 120)], [3, 4, 3, 1])[0]
        action = r.choices([0, 2, 15, 8], [5, 3, 1, 1])[0]
        cx, cy = ox >> SHIFT, oy >> SHIFT
        if r.random() < 0.05:
            cx, cy = r.randrange(NOX), r.randrange(NOY)
        w.set_vid(2 + i, oc)
        s = w.sprite({"x": ox, "y": oy, "z": oz, "nvid": 2 + i, "action": action})
        sprites[s] = i
        objs.append([ox, oy, oz, action, oc["category"], oc["flags"], oc["collisionMask"], oc["sizeX"], oc["sizeY"],
                     oc["sizeZ"], cx, cy])


def commit_objects(grid, sprites, objs):
    for s, i in sorted(sprites.items(), key=lambda kv: kv[1]):
        grid.add(objs[i][10], objs[i][11], s)
    grid.commit()


def gen_ask_place(o, n=1200):
    r = rng("ask_place")
    w = World(o, nvids=16)
    cases = []
    for _ in range(n):
        w.reset()
        o.calls.clear()
        grid = Grid(o, SHIFT, SHIFT, NOX, NOY)
        fn = r.choice([0, 0, 1])
        sc = self_vid(r)
        sx, sy = r.randrange(40, NOX * 32 - 40), r.randrange(40, NOY * 32 - 40)
        ax, ay = sx + r.randrange(-8, 9), sy + r.randrange(-8, 9)
        az = r.choices([0, r.randrange(1, 100), r.randrange(-5, 0), 80], [6, 2, 0.3, 1])[0]
        w.set_vid(1, sc)
        me = w.sprite({"x": sx, "y": sy, "z": az, "nvid": 1})
        in_grid = 1 if r.random() < 0.7 else 0
        if in_grid:
            grid.add(sx >> SHIFT, sy >> SHIFT, me)
        sprites, objs = {}, []
        add_objects(r, w, grid, r.choices(range(7), [1, 3, 3, 2, 1, 1, 1])[0], [(ax, ay)], az, sprites, objs)
        commit_objects(grid, sprites, objs)
        if fn == 0:
            res = o.call(ASK_PLACE, [ax, ay, az], conv="thiscall", this=me) & 0xFFFFFFFF
        else:
            res = o.call(ASK_PLACE_VID, [ax, ay, az, 1], conv="cdecl") & 0xFFFFFFFF
        assert not o.calls
        blocker = -1 if res == 0 else (-2 if res == me else sprites[res])
        case = [fn] + [sc[k] for k in ("category", "flags", "collisionMask", "sizeX", "sizeY", "sizeZ")] + \
            [sx, sy, in_grid, ax, ay, az, blocker, len(objs)]
        for ob in objs:
            case += ob
        cases.append(case)
    rec = family("ask_place", "AskPlace 0x40a430 (thiscall, x, y, z; ret 0xc) / AskPlaceVid 0x40a2d0 (cdecl, x, y, z, nvid)",
                 ["fn", "category", "flags", "collisionMask", "sizeX", "sizeY", "sizeZ", "selfX", "selfY", "selfInGrid",
                  "argX", "argY", "argZ", "blocker", "nObjects"], [],
                 notes=f"fn 0 = AskPlace on the sprite (vid 1), 1 = AskPlaceVid for nvid 1 (no sprite: the sprite in "
                       f"the grid is an ordinary object for it). Grid as check_move (cell shift {SHIFT}, {NOX}x{NOY}); "
                       "blocker: object index, -1 none, -2 = the sprite itself (AskPlace with z < 0, or AskPlaceVid "
                       "meeting it in the grid).")
    rec["objectColumns"] = OBJ_COLS
    rec["cases"] = cases
    return rec


def line_case(r):
    sx, sy = r.randrange(0, 2048), r.randrange(0, 2048)
    kind = r.random()
    if kind < 0.55:
        x2, y2 = sx + r.randrange(-600, 601), sy + r.randrange(-600, 601)
    elif kind < 0.7:
        x2, y2 = sx + r.randrange(-40, 41), sy + r.randrange(-40, 41)
    elif kind < 0.8:
        x2, y2 = sx + r.choice([0, 16, -16, 17, 32, 33]), sy + r.randrange(-3000, 3000)
    elif kind < 0.9:
        x2, y2 = r.randrange(-500, 4000), r.randrange(-500, 4000)
    elif kind < 0.97:
        sx, sy = r.randrange(-300, 100), r.randrange(-300, 100)   # negative positions (packed result)
        x2, y2 = r.randrange(-400, 400), r.randrange(-400, 400)
    else:
        x2, y2 = sx + r.randrange(-70000, 70000), sy + r.randrange(-70000, 70000)   # int32 beyond s16
    sz = r.choices([0, r.randrange(0, 120), r.randrange(-20, 0)], [4, 3, 0.5])[0]
    z2 = r.choices([sz, 0, sz + r.randrange(-50, 200), r.randrange(-100, 400)], [3, 2, 3, 1])[0]
    return sx, sy, sz, x2, y2, z2


def gen_first_obstacle(o, n=2000):
    r = rng("first_obstacle")
    w = World(o)
    st = {}

    def probe(o, args):
        k = st["n"]
        st["n"] += 1
        x, y, z = args(0), args(1), args(2)
        st["sum"][0] += x
        st["sum"][1] += y
        st["sum"][2] += z
        st["last"] = [x, y, z]
        return st["blocker"] if k == st["k"] else 0

    o.stub(ASK_PLACE, probe, popped=12)
    cases = []
    for _ in range(n):
        w.reset()
        cat = r.choices([4, 8, 16, 2, 0x0c], [6, 2, 2, 0.5, 0.5])[0]
        flags = r.choices([0, 0x40, 0x20], [3, 5, 0.5])[0]
        sx, sy, sz, x2, y2, z2 = line_case(r)
        w.set_vid(1, {"category": cat, "flags": flags})
        me = w.sprite({"x": sx, "y": sy, "z": sz, "nvid": 1})
        k = r.choices([-1, r.randrange(0, 4), r.randrange(0, 60), 0], [4, 2, 3, 1])[0]
        st.update(n=0, k=k, blocker=me + 0x1000, sum=[0, 0, 0], last=[0, 0, 0])
        ret = o.call(FIRST_OBSTACLE, [x2, y2, z2], conv="thiscall", this=me)
        cases.append([cat, flags, sx, sy, sz, x2, y2, z2, k, ret, st["n"]] + st["sum"] + st["last"])
    o.unstub(ASK_PLACE)
    return family("first_obstacle",
                  "FirstObstacleOnLine 0x40a770 (thiscall, x2, y2, z2; ret 0xc), AskPlace 0x40a430 recorded",
                  ["category", "flags", "x", "y", "z", "x2", "y2", "z2", "blockAt", "ret", "nProbes", "sumX", "sumY",
                   "sumZ", "lastX", "lastY", "lastZ"], cases,
                  notes="AskPlace(x, y, z) is replaced by a recorder that returns a sprite at the probe number blockAt "
                        "(0-based, -1 = never); nProbes/sum*/last* describe the probes made; ret = (y << 16) + x of the "
                        "blocked probe (32-bit sum, x sign extended), 0 = free line")


def gen_first_obstacle_grid(o, n=800):
    r = rng("first_obstacle_grid")
    w = World(o, nvids=16)
    cases = []
    for _ in range(n):
        w.reset()
        o.calls.clear()
        grid = Grid(o, SHIFT, SHIFT, NOX, NOY)
        sc = self_vid(r)
        sc["category"] = r.choices([4, 8, 16], [6, 2, 2])[0]
        sx, sy = r.randrange(40, NOX * 32 - 40), r.randrange(40, NOY * 32 - 40)
        x2 = max(0, min(NOX * 32 - 1, sx + r.randrange(-400, 401)))
        y2 = max(0, min(NOY * 32 - 1, sy + r.randrange(-400, 401)))
        sz = r.choices([0, r.randrange(1, 100)], [3, 1])[0]
        z2 = r.choices([sz, 0, sz + r.randrange(-30, 150)], [3, 2, 2])[0]
        w.set_vid(1, sc)
        me = w.sprite({"x": sx, "y": sy, "z": sz, "nvid": 1})
        in_grid = 1 if r.random() < 0.8 else 0
        if in_grid:
            grid.add(sx >> SHIFT, sy >> SHIFT, me)
        sprites, objs = {}, []
        on_line = [(sx + (x2 - sx) * t // 8, sy + (y2 - sy) * t // 8) for t in range(9)]
        add_objects(r, w, grid, r.choices(range(9), [1, 2, 3, 3, 2, 2, 1, 1, 1])[0], on_line, sz, sprites, objs)
        commit_objects(grid, sprites, objs)
        ret = o.call(FIRST_OBSTACLE, [x2, y2, z2], conv="thiscall", this=me)
        assert not o.calls
        case = [sc[k] for k in ("category", "flags", "collisionMask", "sizeX", "sizeY", "sizeZ")] + \
            [sx, sy, sz, in_grid, x2, y2, z2, ret, len(objs)]
        for ob in objs:
            case += ob
        cases.append(case)
    rec = family("first_obstacle_grid", "FirstObstacleOnLine 0x40a770 with the real AskPlace 0x40a430 and grid 0x431ab0",
                 ["category", "flags", "collisionMask", "sizeX", "sizeY", "sizeZ", "x", "y", "z", "selfInGrid",
                  "x2", "y2", "z2", "ret", "nObjects"], [],
                 notes=f"grid as check_move (cell shift {SHIFT}, {NOX}x{NOY}, the sprite first in its cell when "
                       "selfInGrid); objects near the line; ret = (y << 16) + x of the first blocked probe, 0 = free")
    rec["objectColumns"] = OBJ_COLS
    rec["cases"] = cases
    return rec


def generate(o):
    return [gen_ask_place(o), gen_first_obstacle(o), gen_first_obstacle_grid(o)]
