"""Physics step of SPRITE::Tact: code range 0x409cae..0x40a116 (docs/re/engine.md §4.2).

The range is executed on a synthetic sprite/vid with esi = sprite, ecx = vid, ebp = 0 (the
register state of 0x409a20 at that point). Collaborators are replaced by recorders:
  - virtual Action(act, v1, v2) (path_block 31, path_limit 38): recorded;
  - CheckMove 0x40a5c0: recorded, returns "blocked" or 0 as chosen by the case;
  - FirstObstacleOnLine 0x40a770 (speed 9999): recorded, returns the chosen packed point;
  - grid cell remove 0x40d320 / add 0x40d290: recorded as cell indices.
The local `step` ([esp+0x24] of SpriteTact, used by the command logic) is read back.
"""
from common import family, rng
from structs import (CURCLOCK, GRID, MAP_H, MAP_W, WIND_DIR, WIND_STRENGTH, World)

BEGIN, END, END_PASSIVE = 0x409cae, 0x40a116, 0x40a22c
CHECK_MOVE, FIRST_OBSTACLE, CELL_REMOVE, CELL_ADD = 0x40a5c0, 0x40a770, 0x40d320, 0x40d290
GRID_SHIFT_X, GRID_SHIFT_Y, GRID_NOX, GRID_NOY, GRID_ROW_SHIFT = 5, 4, 100, 120, 7
STEP_LOCAL = 0x24

COLUMNS_IN = ["flags", "speedX", "acceleration", "rotationPeriod",
              "x", "y", "z", "direction", "state", "lastTact", "speed", "vz", "fracX", "fracY", "fracZ",
              "stepAccum", "goX", "goY", "goZ",
              "curclock", "mapW", "mapH", "windStrength", "windDir", "blocked", "obstacle"]
COLUMNS_OUT = ["x'", "y'", "z'", "state'", "speed'", "vz'", "fracX'", "fracY'", "fracZ'", "stepAccum'",
               "stepSet", "step", "nActions", "act1", "act1v1", "act1v2", "act2", "act2v1", "act2v2",
               "checkMoveCalls", "cmX", "cmY", "cmZ", "obstacleCalls", "cellRemoved", "cellAdded"]


def pick(r, choices, weights=None):
    return r.choices(choices, weights=weights)[0]


def make_case(r, i):
    c = {}
    c["flags"] = r.choice([0, 2, 0x40, 0x42, 0x1000, 0x1002, 0x1040, 0x1042]) | pick(r, [0, 1, 0x20, 0x100], [6, 1, 1, 1])
    c["speedX"] = pick(r, [r.randrange(0, 200), r.randrange(-200, 200), 9999, 0, 32767, -32768], [6, 1, 1, 1, 0.3, 0.3])
    c["acceleration"] = pick(r, [r.randrange(0, 1000), 9999, 0, 65535, r.randrange(0, 65536)], [6, 1, 1, 0.5, 0.5])
    c["rotationPeriod"] = pick(r, [r.randrange(0, 30), r.randrange(-128, 128)], [5, 1])
    mw, mh = pick(r, [(2400, 1920), (r.randrange(100, 3200), r.randrange(100, 1920)), (32767, 32767)], [3, 3, 1])
    c["mapW"], c["mapH"] = mw, mh
    near_edge = r.random() < 0.25
    if near_edge:
        c["x"] = r.choice([r.randrange(0, 8), r.randrange(mw - 8, mw + 2)])
        c["y"] = r.choice([r.randrange(0, 8), r.randrange(mh - 8, mh + 2), r.randrange(0, mh)])
    else:
        c["x"], c["y"] = r.randrange(0, mw + 1), r.randrange(0, mh + 1)
    if r.random() < 0.03:
        c["x"], c["y"] = r.randrange(-32768, 32768), r.randrange(-32768, 32768)
    c["z"] = pick(r, [0, r.randrange(0, 120), r.randrange(-20, 0), r.randrange(-32768, 32768)], [4, 4, 1, 0.3])
    c["direction"] = r.randrange(256)
    st = pick(r, [0, 0x100, 0x110, 0x10, 0x111, 0x112, 0x113, 0x13], [2, 3, 4, 2, 1, 1, 1, 1])
    if r.random() < 0.03:
        st |= 0x40
    c["state"] = st | (r.choice([0, 0x20, 0x200, 0x800]) if r.random() < 0.2 else 0)
    dt = pick(r, [r.randrange(20, 80), 0, 1, 71, r.randrange(0, 300), r.randrange(300, 70000),
                  r.randrange(1 << 20, 1 << 32), 0xFFFFFFFF], [10, 1, 1, 2, 2, 1, 0.5, 0.2])
    c["curclock"] = r.randrange(0, 1 << 32)
    c["lastTact"] = (c["curclock"] - dt) & 0xFFFFFFFF
    c["speed"] = pick(r, [0, r.randrange(-60, 200), c["speedX"], 9999, r.randrange(-32768, 32768), -32768, 32767],
                      [3, 6, 2, 1, 0.5, 0.2, 0.2])
    c["vz"] = pick(r, [0, r.randrange(-200, 200), r.randrange(-32768, 32768)], [5, 4, 0.5])
    c["fracX"], c["fracY"], c["fracZ"] = (pick(r, [r.randrange(0, 128), r.randrange(-128, 128)], [6, 1]) for _ in range(3))
    c["stepAccum"] = pick(r, [r.randrange(0, 30), r.randrange(0, 256)], [5, 1])
    # destinations close to the position exercise the crossing bits and the z snapping
    c["goX"] = c["x"] + pick(r, [r.randrange(-10, 11), r.randrange(-400, 400), 0], [4, 3, 1])
    c["goY"] = c["y"] + pick(r, [r.randrange(-10, 11), r.randrange(-400, 400), 0], [4, 3, 1])
    c["goZ"] = c["z"] + pick(r, [r.randrange(-6, 7), r.randrange(-100, 100), 0], [4, 2, 2])
    for k in ("goX", "goY", "goZ", "x", "y", "z"):
        c[k] = ((c[k] + 0x8000) & 0xFFFF) - 0x8000
    c["windStrength"] = pick(r, [0, r.randrange(-100, 100), r.randrange(-32768, 32768)], [5, 3, 0.3])
    c["windDir"] = pick(r, [r.randrange(256), r.randrange(1 << 32)], [5, 1])
    if r.random() < 0.08:
        # land exactly on goZ (the snapping is asymmetric: see docs/oracle.md)
        c["flags"] &= ~2
        c["vz"] = r.choice([-1, 1]) * r.randrange(20, 400)
        c["z"] = r.randrange(0, 150)
        dt = (c["curclock"] - c["lastTact"]) & 0xFFFFFFFF
        a = (((dt * c["vz"]) & 0xFFFFFFFF) ^ 0x80000000) - 0x80000000
        dz = ((a >> 3) + c["fracZ"]) >> 7
        c["goZ"] = ((c["z"] + dz + r.choice([0, 0, 1, -1]) + 0x8000) & 0xFFFF) - 0x8000
    c["blocked"] = 1 if r.random() < 0.25 else 0
    c["obstacle"] = 0 if r.random() < 0.5 else (r.randrange(0, 4000) | (r.randrange(0, 4000) << 16))
    return c


def generate(o, n=2000):
    r = rng("physics")
    w = World(o)
    state = {}

    def check_move(o, args):
        o.calls.append(["cm", args(0), args(1), args(2)])
        return state["blocker"] if state["blocked"] else 0

    def first_obstacle(o, args):
        o.calls.append(["fo", args(0), args(1), args(2)])
        return state["obstacle"]

    def cell(tag):
        def cb(o, args):
            o.calls.append([tag, (o.reg("ecx") - o.u32(GRID + 0x08)) // 8])
            return 1
        return cb

    o.stub(CHECK_MOVE, check_move, popped=12)
    o.stub(FIRST_OBSTACLE, first_obstacle, popped=12)
    o.stub(CELL_REMOVE, cell("rm"), popped=4)
    o.stub(CELL_ADD, cell("add"), popped=4)
    cases = []
    for i in range(n):
        c = make_case(r, i)
        w.reset()
        o.calls.clear()
        vid = w.set_vid(1, {"flags": c["flags"], "speedX": c["speedX"], "acceleration": c["acceleration"],
                            "rotationPeriod": c["rotationPeriod"], "category": 4, "cls": 2, "dirs": 32})
        spr = w.sprite({k: c[k] for k in ("x", "y", "z", "direction", "state", "lastTact", "speed", "vz", "fracX",
                                           "fracY", "fracZ", "stepAccum", "goX", "goY", "goZ")})
        w.put(spr, "nvid", 1)
        w.put(spr, "vtable", w.vtable("act"))
        state.update(blocked=c["blocked"], blocker=spr + 0x1000, obstacle=c["obstacle"])
        o.w32(CURCLOCK, c["curclock"])
        o.w32(MAP_W, c["mapW"])
        o.w32(MAP_H, c["mapH"])
        o.w16(WIND_STRENGTH, c["windStrength"])
        o.w32(WIND_DIR, c["windDir"])
        cells = o.alloc(8)
        o.w32(GRID + 0x08, cells)  # only used to turn cell pointers back into indices
        o.w8(GRID + 0x0c, GRID_ROW_SHIFT)
        o.w16(GRID + 0x0e, GRID_NOX)
        o.w16(GRID + 0x10, GRID_NOY)
        o.w8(GRID + 0x12, GRID_SHIFT_X)
        o.w8(GRID + 0x14, GRID_SHIFT_Y)
        end = o.run(BEGIN, [END, END_PASSIVE], regs={"esi": spr, "ecx": vid, "ebp": 0}, stack_words=32)
        step_raw = o.u32(o.frame + STEP_LOCAL)
        step_set = 0 if step_raw == 0xCCCCCCCC else 1
        acts = [e[2:] for e in o.calls if e[0] == "act"]
        cms = [e[1:] for e in o.calls if e[0] == "cm"]
        fos = [e for e in o.calls if e[0] == "fo"]
        rm = [e[1] for e in o.calls if e[0] == "rm"]
        add = [e[1] for e in o.calls if e[0] == "add"]
        assert len(acts) <= 2 and len(cms) <= 1 and len(rm) <= 1 and len(add) <= 1, o.calls
        if end == END_PASSIVE:
            assert c["state"] & 0x40
        a = acts + [[-1, 0, 0]] * (2 - len(acts))
        cm = cms[0] if cms else [0, 0, 0]
        out = [w.get(spr, k) for k in ("x", "y", "z", "state", "speed", "vz", "fracX", "fracY", "fracZ", "stepAccum")]
        out += [step_set, (step_raw - (1 << 32) if step_raw & 0x80000000 else step_raw) if step_set else 0,
                len(acts)] + a[0] + a[1] + [len(cms)] + cm + [len(fos), rm[0] if rm else -1, add[0] if add else -1]
        cases.append([c[k] for k in COLUMNS_IN] + out)
    return [family("physics", "SPRITE::Tact physics range 0x409cae..0x40a116 (esi=sprite, ecx=vid, ebp=0)",
                   COLUMNS_IN + COLUMNS_OUT, cases,
                   notes=f"grid: cell shift x {GRID_SHIFT_X}, y {GRID_SHIFT_Y}, nox {GRID_NOX}, noy {GRID_NOY}, "
                         f"row shift {GRID_ROW_SHIFT}; vid.dirs 32; obstacle = packed x | y << 16 returned by 0x40a770; "
                         "blocked = CheckMove returns a sprite; stepSet = 0 when the step local is not written")]
