"""SetDestination(x, y, z) 0x409770 (thiscall, ret 0xc) with the real GetDirection; ChangeAction
0x40b3e0 is recorded instead of executed."""
from common import family, rng
from structs import G_DIST, World

SET_DESTINATION, CHANGE_ACTION = 0x409770, 0x40b3e0

COLUMNS_IN = ["speedX", "speedY", "dirs", "flags", "category", "cls",
              "x", "y", "z", "direction", "state", "speed", "argX", "argY", "argZ"]
COLUMNS_OUT = ["state'", "direction'", "goX", "goY", "goZ", "vz", "fracsZeroed", "gDist", "caAct", "caDir"]


def make_case(r):
    c = {}
    c["speedX"] = r.choices([0, r.randrange(1, 300), r.randrange(-300, 0), 9999, r.randrange(-32768, 32768)],
                            [2, 8, 1, 1, 0.5])[0]
    c["speedY"] = r.choices([0, r.randrange(1, 100), r.randrange(-50, 0), r.randrange(-32768, 32768)], [2, 6, 1, 0.5])[0]
    c["dirs"] = r.choice([1, 1, 8, 16, 32, 32, 64, 3, 255])
    c["flags"] = r.choice([0, 1, 2, 3, 0x40, 0x42, 0x100])
    c["category"] = r.choice([4, 4, 8, 16, 16, 2, 0x0c, 0x14])
    c["cls"] = r.choice([2, 2, 4, 5, 6, 15, 15, 14])
    c["x"], c["y"] = r.randrange(0, 3000), r.randrange(0, 3000)
    c["z"] = r.choices([0, r.randrange(0, 150), r.randrange(-32768, 32768)], [4, 4, 0.5])[0]
    c["direction"] = r.randrange(256)
    c["state"] = r.choice([0, 3, 0x13, 0x113, 0x20, 0x803, 0xffff])
    c["speed"] = r.choice([0, 0, 50, -20, 9999])
    kind = r.random()
    if kind < 0.1:
        c["argX"], c["argY"] = c["x"], c["y"]                         # already there
    elif kind < 0.6:
        c["argX"], c["argY"] = c["x"] + r.randrange(-600, 601), c["y"] + r.randrange(-600, 601)
    elif kind < 0.9:
        c["argX"], c["argY"] = r.randrange(-100, 4000), r.randrange(-100, 4000)
    else:
        c["argX"], c["argY"] = r.randrange(-70000, 70000), r.randrange(-70000, 70000)  # truncated to s16 when stored
    c["argZ"] = r.choices([c["z"], 0, 80, c["z"] + r.randrange(-200, 200), r.randrange(-70000, 70000)], [3, 3, 2, 3, 0.5])[0]
    return c


def generate(o, n=1200):
    r = rng("setdest")
    w = World(o)
    o.stub(CHANGE_ACTION, o.recorder("ca", 2), popped=8)
    cases = []
    for _ in range(n):
        c = make_case(r)
        w.reset()
        o.calls.clear()
        w.set_vid(1, {k: c[k] for k in ("speedX", "speedY", "dirs", "flags", "category", "cls")})
        spr = w.sprite({k: c[k] for k in ("x", "y", "z", "direction", "state", "speed")})
        w.put(spr, "nvid", 1)
        for k in ("fracX", "fracY", "fracZ"):
            w.put(spr, k, 0x55)
        w.put(spr, "vz", 0x1234)
        w.put(spr, "goX", 0x2222)
        w.put(spr, "goY", 0x3333)
        w.put(spr, "goZ", 0x4444)
        o.w32(G_DIST, 0x5A5A5A5A)
        o.call(SET_DESTINATION, [c["argX"], c["argY"], c["argZ"]], conv="thiscall", this=spr)
        fz = int(all(w.get(spr, k) == 0 for k in ("fracX", "fracY", "fracZ")))
        assert fz or all(w.get(spr, k) == 0x55 for k in ("fracX", "fracY", "fracZ"))
        ca = [e for e in o.calls if e[0] == "ca"]
        assert len(ca) <= 1
        out = [w.get(spr, "state"), w.get(spr, "direction"), w.get(spr, "goX"), w.get(spr, "goY"), w.get(spr, "goZ"),
               w.get(spr, "vz"), fz, o.s32(G_DIST)] + ([ca[0][2], ca[0][3]] if ca else [-1, 0])
        cases.append([c[k] for k in COLUMNS_IN] + out)
    return [family("set_destination", "SetDestination 0x409770 (thiscall this=sprite, args int32 x, y, z; ret 0xc)",
                   COLUMNS_IN + COLUMNS_OUT, cases,
                   notes="before the call the sprite has fracs 0x55, vz 0x1234, go = (0x2222, 0x3333, 0x4444): "
                         "unchanged values show the early return when the distance is 0; caAct -1 = no ChangeAction")]
