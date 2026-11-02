"""Weapons of issue #12 (docs/re/engine.md §7):
  - FindTarget 0x40c970 (thiscall): the grid's unit list, virtual Action(act_getarmy 76)
    answered by a recorder, real Distance/GetDirection/rand (family find_target);
  - Fire(x, y, z) 0x40e6b0 and Aim(x, y, z) 0x40ada0 (thiscall, ret 0xc) with the real TurnTo
    and GetDirection, ChangeAction 0x40b3e0 recorded (family aim_fire);
  - CreateChildren 0x40a950 (thiscall): the real offsets, directions, scatter and rand();
    CreateUnit 0x40e1b0, AskPlaceVid 0x40a2d0, SetDestination 0x409770 and the children's
    virtual Action(act_child 57) recorded (family create_children).
"""
from common import family, rng
from structs import G_DIST, RAND_STATE, World
from units import INIT_TABLES, ArmyVtable, get, put, set_unit_list, set_vid_ext, set_weapons

FIND_TARGET, FIRE, AIM, CREATE_CHILDREN = 0x40c970, 0x40e6b0, 0x40ada0, 0x40a950
CHANGE_ACTION, CREATE_UNIT, ASK_PLACE_VID, SET_DESTINATION = 0x40b3e0, 0x40e1b0, 0x40a2d0, 0x409770


def weapons_with(r, wpn):
    """Four weapons, the case's one at a random index."""
    idx = r.randrange(4)
    lst = [{"targetCategory": r.choice([4, 8, 12]), "flags": r.randrange(16), "range": r.randrange(500),
            "scatter": r.randrange(20), "cooldown": r.randrange(300)} for _ in range(4)]
    lst[idx] = wpn
    return idx, lst


def gen_find_target(o, n=1500):
    r = rng("find_target")
    w = World(o, nvids=16)
    av = ArmyVtable(o)
    cases = []
    for _ in range(n):
        w.reset()
        o.calls.clear()
        av.army.clear()
        wpn = {"targetCategory": r.choices([4, 8, 12, 0x10, 0x14, 1, 0, 0x18], [4, 3, 5, 0.3, 0.5, 0.2, 0.2, 0.5])[0],
               "flags": r.choices([0, 2, 8, 0x0a, 1, 4], [4, 3, 4, 1, 1, 1])[0]}
        widx, lst = weapons_with(r, wpn)
        set_weapons(o, lst)
        radius = r.choices([0, r.randrange(50, 600), 300, 65535, r.randrange(600, 3000)], [0.5, 6, 1, 0.2, 0.5])[0]
        set_vid_ext(w, 1, {"visibilityRadius": radius, "weapon": widx, "category": 4})
        sx, sy = r.randrange(0, 2048), r.randrange(0, 2048)
        direction = r.randrange(256)
        my_army = r.choice([0, 1, 1, 2, 3])
        me = w.sprite({"x": sx, "y": sy, "nvid": 1, "direction": direction})
        put(o, me, "vtable", av.vtable())
        av.army[me] = my_army
        nunits = r.choices(range(9), [1, 2, 3, 3, 3, 2, 2, 1, 1])[0]
        units, rows = [], []
        reach = min(radius if radius < 32768 else 300, 1500) * 4 // 5 + 20
        for k in range(nunits):
            cat = r.choices([4, 8, 12, 0x14, 0x18], [5, 4, 1, 0.5, 0.5])[0]
            set_vid_ext(w, 2 + k, {"category": cat})
            ux = max(0, min(2047, sx + r.randrange(-reach, reach + 1)))
            uy = max(0, min(2047, sy + r.randrange(-reach, reach + 1)))
            army = r.choices([0, 1, 2, 3, my_army], [3, 3, 0.7, 2, 1])[0]
            state = 0x400 if r.random() < 0.08 else r.choice([0, 0x10, 0x20, 0x13])
            u = w.sprite({"x": ux, "y": uy, "nvid": 2 + k, "state": state})
            put(o, u, "vtable", av.vtable())
            av.army[u] = army
            units.append(u)
            rows.append([ux, uy, cat, army, state])
        self_at = r.randrange(nunits + 1) if r.random() < 0.6 else -1
        order = list(units)
        if self_at >= 0:
            order.insert(self_at, me)
        set_unit_list(o, order)
        seed = r.randrange(1 << 32)
        o.w32(RAND_STATE, seed)
        res = o.call(FIND_TARGET, [], conv="thiscall", this=me) & 0xFFFFFFFF
        best = -1 if res == 0 else (-2 if res == me else units.index(res))
        assert all(e[2] == 76 for e in o.calls), o.calls
        case = [wpn["targetCategory"], wpn["flags"], radius, sx, sy, direction, my_army, seed, self_at, best,
                o.u32(RAND_STATE), len(o.calls), nunits]
        for row in rows:
            case += row
        cases.append(case)
    rec = family("find_target", "FindTarget 0x40c970 (thiscall this=sprite) over the grid's unit list 0x431ab0",
                 ["targetCategory", "weaponFlags", "visibilityRadius", "x", "y", "direction", "army", "seed", "selfAt",
                  "result", "seed'", "armyCalls", "nUnits"], [],
                 notes="units (objectColumns) are listed in order with the searching sprite inserted at index selfAt "
                       "(-1 = not in the list); virtual Action(act_getarmy) returns the army column (the searcher's: "
                       "army); result = unit index, -1 none, -2 the searcher; seed' = rand state afterwards; armyCalls "
                       "= number of act_getarmy calls")
    rec["objectColumns"] = ["x", "y", "category", "army", "state"]
    rec["cases"] = cases
    return rec


AIM_IN = ["mode", "weaponFlags", "range", "cooldown", "dirs", "dirStep", "vidFlags", "fightChild", "linked",
          "hasChild", "childDirs", "childFightChild", "x", "y", "direction", "action", "timer", "childDirection",
          "childAction", "argX", "argY", "argZ", "gDist"]
AIM_OUT = ["ret", "timer'", "direction'", "childDirection'", "attackX", "attackY", "attackZ", "childAttackX",
           "childAttackY", "childAttackZ", "gDist'", "nChanges", "ca1Who", "ca1Act", "ca1Dir", "ca2Who", "ca2Act",
           "ca2Dir"]


def gen_aim_fire(o, n=1500):
    r = rng("aim_fire")
    o.call(INIT_TABLES, [])
    w = World(o)
    o.stub(CHANGE_ACTION, o.recorder("ca", 2), popped=8)
    cases = []
    for _ in range(n):
        w.reset()
        o.calls.clear()
        c = {"mode": r.choice([0, 0, 1]),
             "weaponFlags": r.choices([0, 1, 4, 5, 8, 2, 9], [4, 2, 2, 1, 1, 1, 1])[0],
             "range": r.choices([r.randrange(0, 600), 0, r.randrange(-50, 0), 32767], [8, 1, 0.5, 0.3])[0],
             "cooldown": r.choice([0, 1, 50, 120, 6553, 65535]),
             "dirs": r.choice([1, 8, 16, 32, 32, 64]),
             "vidFlags": r.choices([0, 1], [5, 1])[0],
             "fightChild": r.choices([0, 5], [1, 5])[0],
             "linked": r.choices([0, 2], [1, 1])[0],
             "hasChild": r.choices([0, 1], [1, 3])[0],
             "childDirs": r.choice([1, 8, 32, 32, 64]),
             "childFightChild": r.choices([0, 5], [1, 4])[0],
             "x": r.randrange(0, 3000), "y": r.randrange(0, 3000), "direction": r.randrange(256),
             "action": r.choice([0, 2, 4, 5, 8]), "timer": r.choices([0, r.randrange(1, 2000)], [4, 1])[0],
             "childDirection": r.randrange(256), "childAction": r.choice([0, 4, 5, 8])}
        c["dirStep"] = (256 // c["dirs"]) & 0xFF
        kind = r.random()
        if kind < 0.6:
            c["argX"], c["argY"] = c["x"] + r.randrange(-700, 701), c["y"] + r.randrange(-700, 701)
        elif kind < 0.8:
            c["argX"], c["argY"] = c["x"] + r.randrange(-60, 61), r.randrange(0, 256)   # flags 4: y is the "direction"
        else:
            c["argX"], c["argY"] = r.randrange(-500, 4000), r.randrange(-500, 4000)
        c["argZ"] = r.choice([0, 19, 80, r.randrange(-50, 200)])
        c["gDist"] = r.choice([0, 100, 5000, r.randrange(-3000, 3000)])
        widx, lst = weapons_with(r, {"targetCategory": 4, "flags": c["weaponFlags"], "range": c["range"],
                                      "scatter": 0, "cooldown": c["cooldown"]})
        set_weapons(o, lst)
        fight = [0] * 16
        fight[8] = c["fightChild"]
        set_vid_ext(w, 1, {"dirs": c["dirs"], "dirStep": c["dirStep"], "flags": c["vidFlags"], "weapon": widx,
                           "linkedObjectVid": c["linked"]}, child_nvid=fight)
        cfight = [0] * 16
        cfight[8] = c["childFightChild"]
        set_vid_ext(w, 2, {"dirs": c["childDirs"], "dirStep": (256 // c["childDirs"]) & 0xFF}, child_nvid=cfight)
        me = w.sprite({"x": c["x"], "y": c["y"], "direction": c["direction"], "action": c["action"], "nvid": 1,
                       "timer": c["timer"]})
        child = 0
        for p in (me,):
            for k, v in (("attackX", 0x1111), ("attackY", 0x2222), ("attackZ", 0x3333)):
                put(o, p, k, v)
        if c["hasChild"]:
            child = w.sprite({"direction": c["childDirection"], "action": c["childAction"], "nvid": 2})
            for k, v in (("attackX", 0x1111), ("attackY", 0x2222), ("attackZ", 0x3333)):
                put(o, child, k, v)
            put(o, me, "child", child)
        o.w32(G_DIST, c["gDist"])
        ret = o.call(FIRE if c["mode"] == 0 else AIM, [c["argX"], c["argY"], c["argZ"]], conv="thiscall", this=me)
        cas = [[0 if e[1] == me else 1, e[2], e[3]] for e in o.calls if e[0] == "ca"]
        assert len(cas) <= 2 and all(e[1] in (me, child) for e in o.calls)
        cas += [[-1, -1, 0]] * (2 - len(cas))
        out = [ret, get(o, me, "timer"), get(o, me, "direction"), get(o, child, "direction") if child else 0,
               get(o, me, "attackX"), get(o, me, "attackY"), get(o, me, "attackZ")]
        out += [get(o, child, k) for k in ("attackX", "attackY", "attackZ")] if child else [0, 0, 0]
        out += [o.s32(G_DIST), sum(1 for e in o.calls if e[0] == "ca")] + cas[0] + cas[1]
        cases.append([c[k] for k in AIM_IN] + out)
    return family("aim_fire", "Fire 0x40e6b0 / Aim 0x40ada0 (thiscall this=sprite, args x, y, z; ret 0xc)",
                  AIM_IN + AIM_OUT, cases,
                  notes="mode 0 = Fire, 1 = Aim; real TurnTo/GetDirection, ChangeAction recorded (ca*Who 0 = the "
                        "sprite, 1 = its child; ca*Dir = the direction argument); vid 1: dirs, flags, weapon, "
                        "linkedObjectVid (linked), childNvid[8] (fightChild); child vid 2: childDirs, childNvid[8]; "
                        "attack fields start at 0x1111/0x2222/0x3333; gDist = the stale 0x439d4c before the call")


CC_IN = ["action", "nv", "count", "offX", "offY", "offZ", "childCls", "childFlags", "x", "y", "z", "direction",
         "attackX", "attackY", "attackZ", "scatter", "seed", "askMask", "failMask"]
CC_OBJ = ["asked", "askX", "askY", "askZ", "created", "cuX", "cuY", "cuZ", "cuDir", "dest", "destX", "destY", "destZ",
          "childAct", "childIndex"]


def gen_create_children(o, n=1200):
    r = rng("create_children")
    o.call(INIT_TABLES, [])  # perspective tables 0x429614/0x429a14 at their startup values (24/16)
    w = World(o)
    st = {}

    def create_unit(o, args):
        k = st["cu"]
        st["cu"] += 1
        st["log"].append(["cu", k, [args(i) for i in range(7)]])
        if (st["fail"] >> k) & 1:
            return 0
        c = w.sprite({"nvid": args(0)})
        put(o, c, "vtable", st["vt"])
        st["children"][c] = k
        return c

    def ask(o, args):
        k = st["ask"]
        st["ask"] += 1
        st["log"].append(["ask", k, [args(i) for i in range(4)]])
        return st["blocker"] if (st["askMask"] >> k) & 1 else 0

    def dest(o, args):
        st["log"].append(["dest", st["children"][o.reg("ecx")], [args(0), args(1), args(2)]])

    def child_action(o, args):
        st["log"].append(["act", st["children"][o.reg("ecx")], [args(0), args(1), args(2)]])
        return 0

    o.stub(CREATE_UNIT, create_unit, popped=0)
    o.stub(ASK_PLACE_VID, ask, popped=0)
    o.stub(SET_DESTINATION, dest, popped=12)
    vt_stub = o.new_stub(child_action, popped=12)
    cases = []
    for _ in range(n):
        w.reset()
        o.calls.clear()
        action = r.choices([8, 15, 0, 13, 2], [6, 2, 1, 1, 1])[0]
        nv = r.choice([2, 3])
        sign = r.choices([1, -1, 0], [6, 2, 0.3])[0]
        count = r.choices([0, 1, 2, 3, 4, 5], [0.3, 3, 3, 2, 1, 1])[0]
        off = [r.choices([0, r.randrange(-40, 41), r.randrange(-200, 200)], [1, 4, 1])[0] for _ in range(2)]
        if sign < 0:
            off = [abs(v) for v in off]  # random offsets in [-off, off]
        offz = r.choice([0, 0, 10, 30, -5, r.randrange(-100, 100)])
        child_cls = r.choices([5, 2, 14, 11], [5, 2, 1, 1])[0]
        child_flags = r.choices([0, 1], [3, 1])[0]
        scatter = r.choice([0, 0, 1, 5, 20, 255])
        widx, lst = weapons_with(r, {"targetCategory": 4, "scatter": scatter})
        set_weapons(o, lst)
        nvids, counts, offs = [0] * 16, [0] * 16, [[0] * 16 for _ in range(3)]
        nvids[action] = sign * nv
        counts[action] = count
        offs[0][action], offs[1][action], offs[2][action] = off[0], off[1], offz
        set_vid_ext(w, 1, {"weapon": widx}, child_nvid=nvids, child_count=counts, child_off=offs)
        set_vid_ext(w, nv, {"cls": child_cls, "flags": child_flags})
        c = {"action": action, "nv": sign * nv, "count": count, "offX": off[0], "offY": off[1], "offZ": offz,
             "childCls": child_cls, "childFlags": child_flags, "x": r.randrange(0, 3000), "y": r.randrange(0, 3000),
             "z": r.choice([0, 0, 40, r.randrange(-20, 150)]), "direction": r.randrange(256),
             "attackX": r.randrange(-100, 3100), "attackY": r.randrange(-100, 3100), "attackZ": r.choice([0, 19, 80]),
             "scatter": scatter, "seed": r.randrange(1 << 32), "askMask": r.choice([0, 0, 1, 2, 5, 0x1f]),
             "failMask": r.choice([0, 0, 0, 1, 2, 4, 0x1f])}
        me = w.sprite({k: c[k] for k in ("x", "y", "z", "direction", "action")})
        put(o, me, "nvid", 1)
        for k in ("attackX", "attackY", "attackZ"):
            put(o, me, k, c[k])
        vt = o.alloc(4)
        o.w32(vt, vt_stub)
        st.update(cu=0, ask=0, fail=c["failMask"], askMask=c["askMask"], blocker=me + 0x1000, log=[], children={}, vt=vt)
        o.w32(RAND_STATE, c["seed"])
        o.call(CREATE_CHILDREN, [], conv="thiscall", this=me)
        # one row per iteration i: what happened to the i-th child
        rows = [[0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1] for _ in range(count if sign else 0)]
        it = -1
        starter = "ask" if child_cls == 2 else "cu"   # an iteration starts with AskPlaceVid for Vehicle children
        for kind, k, a in st["log"]:
            if kind == starter:
                it += 1
            if kind == "ask":
                rows[it][0:4] = [1] + a[0:3]
                assert a[3] == nv
            elif kind == "cu":
                assert a[0] == nv and a[4] == 0 and (a[6] & 0xFFFFFFFF) == me
                rows[it][4:9] = [1 if not (c["failMask"] >> k) & 1 else 2] + a[1:4] + [a[5]]
            elif kind == "dest":
                rows[it][9:13] = [1] + a
            else:
                assert (a[1] & 0xFFFFFFFF) == me and a[0] == 57
                rows[it][13:15] = [a[0], a[2]]
        assert it == len(rows) - 1 or not rows, (st["log"], count)
        case = [c[k] for k in CC_IN] + [o.u32(RAND_STATE), len(rows)]
        for row in rows:
            case += row
        cases.append(case)
    rec = family("create_children", "CreateChildren 0x40a950 (thiscall this=sprite)", CC_IN + ["seed'", "nIterations"], [],
                 notes="vid 1: childNvid[action] = nv, childrenCount[action] = count, childrenOffsets[*][action] = "
                       "offX/offY/offZ; vid |nv|: class childCls, flags childFlags. Per iteration (objectColumns): "
                       "asked = AskPlaceVid called (Vehicle children; returns a sprite when bit k of askMask is set, k "
                       "= call number) with askX/Y/Z; created = CreateUnit called (1 = returned a sprite, 2 = returned "
                       "0 because bit k of failMask is set, k = call number) with cuX/Y/Z/Dir (action 0, parent this); "
                       "dest = SetDestination(destX, destY, destZ) on the child; childAct/childIndex = the child's "
                       "virtual Action(57, this, i)")
    rec["objectColumns"] = CC_OBJ
    rec["cases"] = cases
    return rec


def generate(o):
    return [gen_find_target(o), gen_aim_fire(o), gen_create_children(o)]
