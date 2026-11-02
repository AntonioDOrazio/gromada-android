"""Damage in the "material" base Action 0x412580 (thiscall, args act, var1, var2; ret 0xc):
  - act_damage (43): hp bookkeeping, death/wound transitions (ChangeAction 0x40b3e0 recorded)
    and removal of the wound child on repair (0x40c0e0 recorded);
  - act_death (15) at the end of the death animation: area damage through the grid
    (targets receive a recorded virtual Action(43, deathDamage, this))."""
from common import family, rng
from structs import Grid, World

MATERIAL_ACTION, CHANGE_ACTION, REMOVE_CHILD = 0x412580, 0x40b3e0, 0x40c0e0
SHIFT, NOX, NOY = 5, 64, 64
WOUND_CHILD_NVID = 77


def gen_direct(o, n=2000):
    r = rng("damage")
    w = World(o)
    o.stub(CHANGE_ACTION, o.recorder("ca", 2), popped=8)
    o.stub(REMOVE_CHILD, o.recorder("rc", 1), popped=4)
    cases = []
    for _ in range(n):
        max_hp = r.choices([0, r.randrange(1, 256), 100, 255, 1, 2, 3], [1, 6, 2, 1, 0.5, 0.5, 0.5])[0]
        hp = r.choices([max_hp, r.randrange(0, 256), max_hp // 2, max_hp // 2 + 1, 0], [2, 4, 1, 1, 0.5])[0] & 0xFF
        dmg = r.choices([r.randrange(0, 50), r.randrange(0, 300), 2, hp, hp - 1, -r.randrange(1, 300),
                         r.randrange(-(1 << 31), 1 << 31)], [5, 2, 1, 1, 1, 1, 0.5])[0]
        w.reset()
        o.calls.clear()
        child_nvid = [0] * 16
        child_nvid[13] = WOUND_CHILD_NVID
        w.set_vid(1, {"maxHP": max_hp}, child_nvid=child_nvid)
        spr = w.sprite({"hp": hp, "nvid": 1})
        ret = o.call(MATERIAL_ACTION, [43, dmg, 0], conv="thiscall", this=spr)
        assert ret == 0
        ca = [e for e in o.calls if e[0] == "ca"]
        rc = [e for e in o.calls if e[0] == "rc"]
        assert len(ca) <= 1 and len(rc) <= 1 and all(e[3] == -1 for e in ca)
        assert all(e[2] == WOUND_CHILD_NVID for e in rc)
        cases.append([max_hp, hp, dmg, w.get(spr, "hp"), ca[0][2] if ca else -1, len(rc)])
    return family("damage", "material Action 0x412580 case act_damage 43 (thiscall this=sprite, args 43, n, 0)",
                  ["maxHP", "hp", "damage", "hp'", "caAct", "woundChildRemoved"], cases,
                  notes="caAct: ChangeAction(act, -1) requested (15 death, 13 wound, -1 none); "
                        "woundChildRemoved: call to 0x40c0e0(vid.childNvid[13]) (wound effects removed after a repair)")


def gen_area(o, n=800):
    r = rng("area_damage")
    w = World(o, nvids=16)
    cases = []
    for _ in range(n):
        w.reset()
        o.calls.clear()
        grid = Grid(o, SHIFT, SHIFT, NOX, NOY)
        sc = {"deathDamage": r.choices([0, r.randrange(1, 256), 2], [1, 6, 1])[0],
              "deathDamageRadius": r.choices([0, r.randrange(1, 100), r.randrange(100, 400), 65535], [1, 6, 1, 0.2])[0],
              "sizeX": r.randrange(0, 64), "sizeY": r.randrange(0, 64), "maxHP": r.choice([0, 10])}
        sx, sy = r.randrange(0, NOX * 32), r.randrange(0, NOY * 32)
        sz = r.choices([0, r.randrange(0, 100)], [3, 1])[0]
        w.set_vid(1, sc)
        me = w.sprite({"x": sx, "y": sy, "z": sz, "nvid": 1, "hp": 9})
        w.put(me, "vtable", w.vtable("self"))
        grid.add(sx >> SHIFT, sy >> SHIFT, me)
        nobj = r.choices(range(9), [1, 2, 3, 3, 2, 1, 1, 1, 1])[0]
        objs, sprites = [], {}
        reach = sc["deathDamageRadius"] + 40 if sc["deathDamageRadius"] < 1000 else 300
        for i in range(nobj):
            oc = {"maxHP": r.choices([0, r.randrange(1, 256)], [1, 5])[0], "sizeX": r.randrange(0, 64),
                  "sizeY": r.randrange(0, 64), "sizeZ": r.choice([0, 10, 30, -30, r.randrange(-60, 100)])}
            ox = max(0, min(NOX * 32 - 1, sx + r.randrange(-reach, reach + 1)))
            oy = max(0, min(NOY * 32 - 1, sy + r.randrange(-reach, reach + 1)))
            oz = r.choices([0, sz, r.randrange(0, 120)], [3, 2, 1])[0]
            w.set_vid(2 + i, oc)
            s = w.sprite({"x": ox, "y": oy, "z": oz, "nvid": 2 + i})
            w.put(s, "vtable", w.vtable(("obj", i)))
            sprites[s] = i
            cx, cy = ox >> SHIFT, oy >> SHIFT
            grid.add(cx, cy, s)
            objs.append([ox, oy, oz, oc["maxHP"], oc["sizeX"], oc["sizeY"], oc["sizeZ"], cx, cy])
        grid.commit()
        o.call(MATERIAL_ACTION, [15, 0, 0], conv="thiscall", this=me)
        hit = 0
        for e in o.calls:
            assert isinstance(e[0], tuple) and e[2] == 43 and e[3] == sc["deathDamage"] and (e[4] & 0xFFFFFFFF) == me, e
            assert not hit & (1 << e[0][1])
            hit |= 1 << e[0][1]
        case = [sc["deathDamage"], sc["deathDamageRadius"], sc["sizeX"], sc["sizeY"], sc["maxHP"], sx, sy, sz,
                w.get(me, "hp"), hit, nobj]
        for ob in objs:
            case += ob
        cases.append(case)
    rec = family("area_damage", "material Action 0x412580 case act_death 15: area damage via grid 0x431ab0",
                 ["deathDamage", "deathDamageRadius", "sizeX", "sizeY", "maxHP", "x", "y", "z", "hp'", "hitMask", "nObjects"],
                 [], notes=f"variable length cases (objectColumns groups). Grid: cell shift {SHIFT}, {NOX}x{NOY} cells; "
                           "every sprite is filed in the cell of its position, the exploding sprite first. hp before the "
                           "call is 9. Each hit object receives Action(43, deathDamage, this).")
    rec["objectColumns"] = ["x", "y", "z", "maxHP", "sizeX", "sizeY", "sizeZ", "cellX", "cellY"]
    rec["cases"] = cases
    return rec


def generate(o):
    return [gen_direct(o), gen_area(o)]
