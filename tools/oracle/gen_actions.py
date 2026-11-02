"""Per-class Action functions of issue #12 (docs/re/engine.md §2.3, §5, §7, §8): the real
Action of Projectile 0x40f710, Vehicle 0x40ea20, Building 0x410e10 and Kassandra 0x411a60 runs
on a synthetic unit in a synthetic world, with every collaborator real (ChangeAction, TurnTo,
SetDestination, Stop, Approach, FindTarget, Fire/Aim, FirstObstacleOnLine/AskPlace, the material
base 0x412580, rand). The sprites have the real class vtables, so virtual calls run the original
code too. Only CreateUnit 0x40e1b0 is replaced (returns 0: no unit is created).

World: map 2048 x 2048 (tiles 24 x 16), grid with 32 px cells (64 x 64), empty but for the
optional target unit; four weapons. Sprites: the unit (vid 1), an optional target (vid 2, a
Static of category Vehicle, also in the grid and in the unit list), an optional turret child
(vid 3, Superstructure). One family per class: the case's fixed columns describe the vids, the
weapon, the sprites before the call and the call; the outputs are the fields afterwards.
"""
from common import family, rng
from structs import CURCLOCK, G_DIST, MAP_H, MAP_W, RAND_STATE, SCALE_X, SCALE_Y, Grid, World
from units import ACTION, INIT_TABLES, VTABLE, get, put, set_unit_list, set_vid_ext, set_weapons

CREATE_UNIT = 0x40e1b0
LOCAL_INPUT = 0x43d045     # 12-byte input record of the local player (bits at +8)
NET_GAME = 0x44e694
SHIFT, NOX, NOY = 5, 64, 64

VID_COLS = ["cls", "category", "vidFlags", "dirs", "speedX", "speedY", "acceleration", "maxHP", "sizeX", "sizeY",
            "sizeZ", "visibilityRadius", "vidArmy", "rotationPeriod", "woundChild"] + [f"len{k}" for k in range(16)]
WPN_COLS = ["weaponIndex", "wTargetCategory", "wFlags", "wRange", "wScatter", "wCooldown"]
SPRITE_COLS = ["x", "y", "z", "direction", "action", "frame", "firstFrame", "lastFrame", "state", "speed", "vz",
               "fracX", "fracY", "fracZ", "goX", "goY", "goZ", "timer", "attackX", "attackY", "attackZ", "hp"]
UNIT_COLS = ["target", "army", "behave", "wpX", "wpY", "buildTime", "turn"]
AMMO_COLS = [f"ammo{k}" for k in range(8)]
WORLD_COLS = ["seed", "curclock", "gDist", "inputBits", "hasTarget", "targetX", "targetY", "targetZ", "targetSizeZ",
              "targetHP", "targetMaxHP", "targetArmy", "hasChild", "childNvid", "childShots", "childDirs", "childFightChild", "childDirection",
              "childAction"]
CALL_COLS = ["act", "v1", "v1ref", "v2", "v2ref"]
OUT_WORLD = ["crash", "ret", "seed'", "gDist'", "createUnitCalls", "target.x'", "target.y'", "target.hp'", "target.action'",
             "child.direction'", "child.action'", "child.attackX'", "child.attackY'", "child.attackZ'"]


def pick(r, values, weights=None):
    return r.choices(values, weights)[0]


def rand_vid(r, cls):
    v = {"cls": cls, "category": pick(r, [4, 8, 16, 2], [6, 1, 1, 0.5]) if cls != 5 else pick(r, [16, 2, 4], [3, 1, 1]),
         "vidFlags": pick(r, [0x40, 0x42, 0x40 | 0x1000, 0], [5, 1, 0.5, 1]),
         "dirs": pick(r, [1, 8, 16, 32, 32, 64]),
         "speedX": pick(r, [0, r.randrange(1, 200), 9999], [1, 6, 0.3]),
         "speedY": pick(r, [0, r.randrange(1, 100)], [2, 3]),
         "acceleration": pick(r, [9999, r.randrange(0, 600)], [1, 2]),
         "maxHP": pick(r, [0, r.randrange(1, 256), 100], [0.5, 4, 1]),
         "sizeX": r.randrange(8, 64), "sizeY": r.randrange(8, 64), "sizeZ": pick(r, [0, 10, 30, r.randrange(-20, 90)]),
         "visibilityRadius": pick(r, [0, r.randrange(50, 500)], [1, 4]),
         "vidArmy": r.randrange(4), "rotationPeriod": r.randrange(0, 20),
         "woundChild": pick(r, [0, 3], [3, 1])}
    for k in range(16):
        v[f"len{k}"] = pick(r, [0, 1, 2, 4, 8], [3, 3, 1, 1, 0.5])
    return v


def rand_sprite(r, vid):
    hx, hy = vid["sizeX"] // 2, vid["sizeY"] // 2
    s = {"x": r.randrange(hx, 2048 - hx), "y": r.randrange(hy, 2048 - hy),
         "z": pick(r, [0, r.randrange(1, 120), r.randrange(-10, 0)], [4, 2, 0.5]),
         "direction": r.randrange(256), "action": pick(r, [0, 2, 3, 4, 5, 8, 15, 13, 12, 10, 6, 14], [3, 3, 1, 1, 1, 1, 1, 1, 0.5, 0.5, 0.5, 0.5]),
         "frame": r.randrange(0, 40), "firstFrame": r.randrange(0, 40), "lastFrame": r.randrange(0, 40),
         "state": pick(r, [0, 0x10, 0x110, 0x113, 0x13, 0x3, 0x20, 0x30, 0x120, 0x80, 0x800, 0x40]) | pick(r, [0, 0x200], [3, 1]),
         "speed": pick(r, [0, vid["speedX"], r.randrange(-60, 200)], [2, 2, 2]),
         "vz": pick(r, [0, r.randrange(-100, 100)], [2, 1]),
         "fracX": r.randrange(0, 128), "fracY": r.randrange(0, 128), "fracZ": r.randrange(0, 128),
         "timer": pick(r, [0, r.randrange(1, 3000)], [3, 1]),
         "attackX": r.randrange(0, 2048), "attackY": r.randrange(0, 2048), "attackZ": pick(r, [0, 19, 80]),
         "hp": pick(r, [vid["maxHP"], r.randrange(0, 256), vid["maxHP"] // 2], [2, 2, 1]) & 0xFF}
    s["goX"] = s["x"] + pick(r, [0, r.randrange(-10, 11), r.randrange(-400, 400)], [1, 1, 2])
    s["goY"] = s["y"] + pick(r, [0, r.randrange(-10, 11), r.randrange(-400, 400)], [1, 1, 2])
    s["goZ"] = s["z"] + pick(r, [0, r.randrange(-30, 80)], [2, 1])
    return s


def rand_unit(r, has_target):
    wp = pick(r, [-1, r.randrange(0, 2048)], [1, 1])
    return {"target": 1 if has_target and r.random() < 0.5 else 0, "army": r.randrange(4),
            "behave": pick(r, [0, 1, 5, 2, 4, 3, 7, 0x80, 0x81], [1, 3, 2, 1, 1, 1, 1, 0.5, 0.5]),
            "wpX": wp, "wpY": wp if wp < 0 else r.randrange(0, 2048), "buildTime": pick(r, [20, r.randrange(256)]),
            "turn": pick(r, [0, 0, 30, -30, r.randrange(-30, 31)])}


def rand_call(r, acts):
    act = pick(r, [a for a, _, _ in acts], [wgt for _, wgt, _ in acts])
    gen = dict((a, g) for a, _, g in acts)[act]
    v1, v1ref, v2 = gen(r)
    return {"act": act, "v1": v1, "v1ref": v1ref, "v2": v2, "v2ref": 0}


def lit(lo, hi, lo2=None, hi2=None):
    return lambda r: (r.randrange(lo, hi), 0, r.randrange(lo2, hi2) if lo2 is not None else 0)


def ref(r):
    k = pick(r, [0, 1, 2], [1, 4, 1])  # 0 = null pointer, 1 = the target, 2 = the unit itself
    return 0, k, 0


NONE = lambda r: (0, 0, 0)  # noqa: E731
MOVE = lit(-100, 2200, -100, 2200)
STEP = lit(-12, 13, -12, 13)

PROJECTILE_ACTS = [(31, 2, STEP), (38, 2, STEP), (39, 2, ref), (40, 2, MOVE), (45, 6, NONE), (46, 1, NONE),
                   (57, 0.5, NONE), (43, 1, lit(-20, 300)), (15, 0.5, NONE), (65, 0.3, NONE)]
VEHICLE_ACTS = [(0, 0.3, NONE), (2, 0.3, NONE), (44, 0.3, NONE), (75, 0.5, lit(0, 256)), (76, 0.5, NONE),
                (77, 0.5, lit(0, 300)), (78, 0.5, NONE), (67, 0.5, lit(0, 256)), (81, 0.5, NONE), (60, 0.5, NONE),
                (56, 1, MOVE), (34, 0.3, lit(0, 20)), (36, 0.3, NONE), (15, 0.5, NONE), (48, 1, ref),
                (45, 10, NONE), (43, 2, lit(-20, 300)), (39, 1.5, ref), (40, 1.5, MOVE), (74, 1, MOVE),
                (70, 1, MOVE), (31, 2, STEP), (38, 1, STEP), (65, 0.3, NONE)]
KASSANDRA_ACTS = [(56, 1, MOVE), (40, 1, MOVE), (63, 1, lambda r: (r.randrange(-50, 300), 0, r.randrange(-1, 9))),
                  (64, 1, lit(-1, 9)), (45, 10, NONE), (43, 2, lit(-20, 300)), (31, 3, STEP), (38, 0.5, STEP),
                  (75, 0.3, lit(0, 4)), (76, 0.3, NONE), (39, 0.5, ref), (74, 0.5, MOVE), (48, 0.3, ref)]
BUILDING_ACTS = [(45, 10, NONE), (43, 2, lit(-20, 300)), (49, 2, NONE), (41, 1, lit(-1, 6)), (75, 0.3, lit(0, 4)),
                 (78, 0.3, NONE), (77, 0.3, lit(0, 60)), (31, 0.5, STEP), (40, 0.5, MOVE)]


MALLOC, REALLOC, FREE = 0x41ca48, 0x41c928, 0x41c881


def install_heap(o):
    """The CRT's malloc/realloc/free (command stacks, item lists), on the oracle heap."""
    sizes = {}

    def malloc(o, args):
        p = o.alloc(max(args(0), 1))
        sizes[p] = args(0)
        return p

    def realloc(o, args):
        old, size = args(0) & 0xFFFFFFFF, args(1)
        p = o.alloc(max(size, 1))
        if old in sizes:
            o.write(p, o.read(old, min(sizes[old], size)))
        sizes[p] = size
        return p

    o.stub(MALLOC, malloc, popped=0)
    o.stub(REALLOC, realloc, popped=0)
    o.stub(FREE, lambda o, args: 0, popped=0)


def run_family(o, name, cls_name, cls, acts, n, kass=False, building=False):
    r = rng(name)
    w = World(o, nvids=96)
    st = {"cu": 0}

    def create_unit(o, args):
        st["cu"] += 1
        return 0

    o.stub(CREATE_UNIT, create_unit, popped=0)
    install_heap(o)
    in_cols = VID_COLS + WPN_COLS + SPRITE_COLS + UNIT_COLS + (AMMO_COLS if kass else []) + \
        (["buildNvid"] if building else []) + WORLD_COLS + CALL_COLS
    out_cols = [c + "'" for c in SPRITE_COLS + UNIT_COLS + (AMMO_COLS if kass else []) + (["buildNvid"] if building else [])] + OUT_WORLD
    cases = []
    for _ in range(n):
        w.reset()
        o.calls.clear()
        st["cu"] = 0
        grid = Grid(o, SHIFT, SHIFT, NOX, NOY)
        o.w32(MAP_W, 2048)
        o.w32(MAP_H, 2048)
        o.w32(SCALE_X, 24)
        o.w32(SCALE_Y, 16)
        o.w32(NET_GAME, 0)
        c = rand_vid(r, cls)
        wpn = {"wTargetCategory": pick(r, [4, 8, 12, 0]), "wFlags": pick(r, [0, 1, 2, 4, 8, 9]),
               "wRange": pick(r, [r.randrange(0, 400), 0]), "wScatter": r.randrange(0, 10),
               "wCooldown": r.randrange(0, 200)}
        c.update(wpn)
        c["weaponIndex"] = pick(r, [0, 1, 2, 3], [1, 2, 2, 2])   # vid.weapon (0 = unarmed for the units)
        has_target = r.random() < 0.6
        c.update(rand_sprite(r, c))
        c.update(rand_unit(r, has_target))
        if kass:
            for k in range(8):
                c[f"ammo{k}"] = pick(r, [0, r.randrange(1, 50), r.randrange(-5, 600)], [2, 3, 1])
        if building:
            c["buildNvid"] = pick(r, [0, 2, 4, 5])
        tx = max(0, min(2047, c["x"] + pick(r, [r.randrange(-60, 61), r.randrange(-500, 500)], [1, 2])))
        ty = max(0, min(2047, c["y"] + pick(r, [r.randrange(-60, 61), r.randrange(-500, 500)], [1, 2])))
        c.update({"seed": r.randrange(1 << 32), "curclock": r.randrange(1 << 31),
                  "gDist": pick(r, [0, r.randrange(0, 600)]), "inputBits": pick(r, [0, 0x40, 0x80, 4, 8, 0x44, 0x88, 0x48, 0xc0], [3, 2, 1, 1, 1, 1, 1, 1, 0.3]),
                  "hasTarget": int(has_target), "targetX": tx, "targetY": ty, "targetZ": pick(r, [0, 0, 50, r.randrange(-5, 100)]),
                  "targetSizeZ": pick(r, [0, 20, -10, 40]), "targetHP": r.randrange(0, 200), "targetMaxHP": pick(r, [0, 100, 200]),
                  "targetArmy": r.randrange(4), "hasChild": int(r.random() < 0.25), "childDirs": pick(r, [1, 8, 32]),
                  "childFightChild": pick(r, [0, 5]), "childDirection": r.randrange(256), "childAction": pick(r, [0, 4, 5, 8])})
        c.update(rand_call(r, acts))
        # the Kassandra's head is its child: vids 0x58..0x5c select the ammunition slot (0x411b84)
        c["childNvid"] = 0x58 + r.randrange(5) if kass else 3
        c["childShots"] = pick(r, [0, 1, 4])   # childrenCount[act_fight] of the child's vid
        if c["act"] == 56:
            c["hasChild"] = 1   # act_coor_attack reads the head without a null check
        widx = c["weaponIndex"]
        wl = [{"targetCategory": 4, "flags": 0, "range": 100, "scatter": 0, "cooldown": 10} for _ in range(4)]
        wl[widx] = {"targetCategory": wpn["wTargetCategory"], "flags": wpn["wFlags"], "range": wpn["wRange"],
                    "scatter": wpn["wScatter"], "cooldown": wpn["wCooldown"]}
        set_weapons(o, wl)
        anim = [c[f"len{k}"] for k in range(16)]
        wound = [0] * 16
        wound[13] = c["woundChild"]
        set_vid_ext(w, 1, {"cls": cls, "category": c["category"], "flags": c["vidFlags"], "dirs": c["dirs"],
                           "speedX": c["speedX"], "speedY": c["speedY"], "acceleration": c["acceleration"],
                           "maxHP": c["maxHP"], "sizeX": c["sizeX"], "sizeY": c["sizeY"], "sizeZ": c["sizeZ"],
                           "visibilityRadius": c["visibilityRadius"], "army": c["vidArmy"],
                           "rotationPeriod": c["rotationPeriod"], "weapon": widx,
                           "linkedObjectVid": c["childNvid"] if c["hasChild"] else 0}, anim_lengths=anim, child_nvid=wound)
        set_vid_ext(w, 2, {"cls": 1, "category": 4, "flags": 0x40, "sizeX": 20, "sizeY": 20, "sizeZ": c["targetSizeZ"],
                           "maxHP": c["targetMaxHP"], "army": c["targetArmy"], "dirs": 1}, anim_lengths=[1] * 16)
        cf = [0] * 16
        cf[8] = c["childFightChild"]
        set_vid_ext(w, c["childNvid"], {"cls": 9, "dirs": c["childDirs"]}, anim_lengths=[1, 0, 0, 0, 1, 1, 0, 0, 2],
                    child_nvid=cf, child_count=[0] * 8 + [c["childShots"]])
        for k in (4, 5):   # vids a building can produce (CreateUnit is stubbed)
            set_vid_ext(w, k, {"cls": 2, "category": 4, "sizeX": 20, "sizeY": 20})
        me = w.sprite({"nvid": 1})
        for k in SPRITE_COLS:
            put(o, me, k, c[k])
        put(o, me, "vtable", VTABLE[cls_name])
        target = 0
        units = []
        if has_target:
            target = w.sprite({"x": tx, "y": ty, "z": c["targetZ"], "nvid": 2, "hp": c["targetHP"]})
            put(o, target, "vtable", VTABLE["Static"])
            grid.add(tx >> SHIFT, ty >> SHIFT, target)
            units.append(target)
        grid.commit()
        set_unit_list(o, units)
        for k in UNIT_COLS:
            put(o, me, k, c[k] if k != "target" else (target if c[k] else 0))
        if kass:
            for k in AMMO_COLS:
                put(o, me, k, c[k])
        if building:
            put(o, me, "buildNvid", c["buildNvid"])
        child = 0
        if c["hasChild"]:
            child = w.sprite({"nvid": c["childNvid"], "direction": c["childDirection"], "action": c["childAction"]})
            put(o, child, "vtable", VTABLE["Superstructure"])
            put(o, me, "child", child)
        o.w32(RAND_STATE, c["seed"])
        o.w32(CURCLOCK, c["curclock"])
        o.w32(G_DIST, c["gDist"])
        o.write(LOCAL_INPUT, bytes(8) + c["inputBits"].to_bytes(2, "little") + bytes(2))
        v1 = c["v1"] if c["v1ref"] == 0 else (0, target, me)[c["v1ref"]]
        if c["v1ref"] == 1 and not target:
            c["v1ref"], v1 = 0, 0
        v2 = c["v2"]
        if c["act"] == 43:   # act_damage: v2 = the sprite that fired (Kassandra reads it on a lethal hit)
            c["v2ref"] = 1 if target else 2
            v2 = target or me
        try:
            ret = o.call(ACTION[cls_name], [c["act"], v1, v2], conv="thiscall", this=me)
            crash = 0
        except RuntimeError as e:   # 0x40c0e0 walks the sub-object chain past its end (D10)
            assert "invalid memory access at 0x14 (eip 0x40c0f0)" in str(e), e
            ret, crash = 0, 1
        out = [get(o, me, k) for k in SPRITE_COLS]
        for k in UNIT_COLS:
            v = get(o, me, k)
            out.append(v if k != "target" else (0 if v == 0 else 1 if v == target else 2 if v == me else 3))
        if kass:
            out += [get(o, me, k) for k in AMMO_COLS]
        if building:
            out.append(get(o, me, "buildNvid"))
        if ret and (ret & 0xFFFFFFFF) in (target, me) and target:
            ret = -1000 - (1 if (ret & 0xFFFFFFFF) == target else 2)   # a pointer: encoded
        out += [crash, ret, o.u32(RAND_STATE), o.s32(G_DIST), st["cu"]]
        out += [get(o, target, k) for k in ("x", "y", "hp", "action")] if target else [0, 0, 0, 0]
        out += [get(o, child, k) for k in ("direction", "action", "attackX", "attackY", "attackZ")] if child else [0] * 5
        cases.append([c[k] for k in in_cols] + out)
    o.unstub(CREATE_UNIT)
    return family(name, f"{cls_name} Action {ACTION[cls_name]:#x} (thiscall this=unit, act, v1, v2; ret 0xc)",
                  in_cols + out_cols, cases,
                  notes="see tools/oracle/gen_actions.py: vid 1 = the unit (len* = animation lengths, woundChild = "
                        "childNvid[13]), vid 2 = target (Static, category 4, 20x20, in the grid and the unit list), vid "
                        "childNvid = turret child (linkedObjectVid = childNvid when hasChild; 0x58.. for the Kassandra, whose "
                        "act_coor_attack uses the head's childrenCount[8] = childShots); weapon w*; target fields: 0 none, 1 the "
                        "target sprite, 2 the unit, 3 another pointer; v1ref: 0 = v1 literal, 1 = v1 = the target, 2 = "
                        "the unit; v2ref likewise for v2 (act_damage: the source); ret -1001/-1002 = returned the target/the unit; createUnitCalls = calls to the "
                        "stubbed CreateUnit (returns 0); crash = the original read past the end of the sub-object chain in "
                        "0x40c0e0 (wound effect missing: outputs after the fault are meaningless); inputBits = bits of the local input record 0x43d045")


def generate(o):
    o.call(INIT_TABLES, [])
    return [run_family(o, "action_projectile", "Projectile", 5, PROJECTILE_ACTS, 600),
            run_family(o, "action_vehicle", "Vehicle", 2, VEHICLE_ACTS, 1400),
            run_family(o, "action_kassandra", "Kassandra", 17, KASSANDRA_ACTS, 1000, kass=True),
            run_family(o, "action_building", "Building", 3, BUILDING_ACTS, 800, building=True)]
