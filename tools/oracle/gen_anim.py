"""ChangeAction(act, dir) 0x40b3e0 (thiscall, ret 8) and SetAnimation 0x40b330 executed for real
on synthetic vids, optionally with a linked child sprite (turret) that has its own vid."""
from common import family, rng
from structs import World

CHANGE_ACTION = 0x40b3e0
SPR_FIELDS = ["action", "direction", "frame", "firstFrame", "lastFrame", "state"]


def lengths(r):
    mode = r.random()
    if mode < 0.7:
        ls = [r.choice([0, 0, 1, 2, 4, 6, 8, 10, 12, 16]) for _ in range(16)]
    elif mode < 0.9:
        ls = [r.randrange(0, 256) for _ in range(16)]  # 16-bit wraparound of the frame sums
    else:
        ls = [0] * 16
        for a in r.sample(range(16), r.randrange(0, 4)):
            ls[a] = r.randrange(1, 9)
    return ls


def generate(o, n=1000):
    r = rng("anim")
    w = World(o)
    cases = []
    for _ in range(n):
        dirs = r.choice([1, 1, 2, 8, 16, 32, 32, 64, 3, 5, 255, 128])
        ls = lengths(r)
        has_child = 1 if r.random() < 0.3 else 0
        cdirs = r.choice([1, 8, 32])
        cls = lengths(r)
        p = {"action": r.choices([0, 1, 2, 4, 5, 6, 8, 10, 15, r.randrange(16)], [3, 1, 3, 1, 1, 1, 1, 2, 1, 2])[0],
             "direction": r.randrange(256), "state": r.choice([0, 0x200, 0x210, 0x3fff, 0xffff])}
        c = {"action": r.choice([0, 6, 10, 15, r.randrange(16)]), "direction": r.randrange(256), "state": r.choice([0, 0x208])}
        act = r.choices([0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 13, 14, 15, r.randrange(16), p["action"]],
                        [2, 1, 3, 1, 1, 1, 2, 2, 1, 2, 2, 1, 2, 2, 2])[0]
        d = r.choices([-1, r.randrange(256), p["direction"], r.choice([256, 300, -2, 511])], [5, 4, 2, 0.5])[0]
        w.reset()
        w.set_vid(1, {"dirs": dirs}, anim_lengths=ls)
        w.set_vid(2, {"dirs": cdirs}, anim_lengths=cls)
        pre_frames = {"frame": 0x1111, "firstFrame": 0x2222, "lastFrame": 0x3333}
        spr = w.sprite(dict(p, nvid=1, **pre_frames))
        child = 0
        if has_child:
            child = w.sprite(dict(c, nvid=2, **pre_frames))
            w.put(spr, "child", child)
        o.call(CHANGE_ACTION, [act, d], conv="thiscall", this=spr)
        out = [w.get(spr, k) for k in SPR_FIELDS]
        out += [w.get(child, k) for k in SPR_FIELDS] if child else [0] * 6
        cases.append([dirs] + ls + [p["action"], p["direction"], p["state"], act, d, has_child, cdirs] + cls +
                     [c["action"], c["direction"], c["state"]] + out)
    cols = (["dirs"] + [f"len{i}" for i in range(16)] + ["action", "direction", "state", "act", "dir", "hasChild", "childDirs"] +
            [f"childLen{i}" for i in range(16)] + ["childAction", "childDirection", "childState"] +
            [f"{k}'" for k in SPR_FIELDS] + [f"child.{k}'" for k in SPR_FIELDS])
    return [family("change_action", "ChangeAction 0x40b3e0 (thiscall, args int act, int dir; ret 8) + SetAnimation 0x40b330",
                   cols, cases,
                   notes="before the call frame/firstFrame/lastFrame of both sprites are 0x1111/0x2222/0x3333 "
                         "(4369/8738/13107): unchanged values mean SetAnimation was not called; vid dirStep = 256/dirs as the loader")]
