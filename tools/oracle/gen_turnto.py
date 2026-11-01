"""TurnTo(dir) 0x40b0b0 (thiscall, ret 4) after the rotation tables are built (0x40cf70);
ChangeAction 0x40b3e0 is recorded."""
from common import INT_MAX, INT_MIN, family, rng
from structs import World

TURN_TO, CHANGE_ACTION, INIT_TABLES = 0x40b0b0, 0x40b3e0, 0x40cf70

COLUMNS = ["dirs", "dirStep", "flags", "direction", "action", "target", "ret", "direction'", "caAct", "caDir"]


def generate(o, n=3000):
    r = rng("turnto")
    o.call(INIT_TABLES, [])
    w = World(o)
    o.stub(CHANGE_ACTION, o.recorder("ca", 2), popped=8)
    cases = []
    for i in range(n):
        dirs = r.choice([1, 2, 3, 4, 5, 6, 8, 8, 12, 16, 16, 24, 32, 32, 32, 40, 48, 64, 64, r.randrange(1, 65)])
        dir_step = (256 // dirs) & 0xFF
        if r.random() < 0.08:
            dir_step = r.randrange(256)  # inconsistent vid (the original reads the stored byte)
        flags = r.choice([0, 0, 0, 0, 1, 0x40])
        cur = r.randrange(256)
        action = r.choices([0, 2, 4, 5, 1, 8, 15], [2, 4, 3, 3, 1, 1, 1])[0]
        kind = r.random()
        if kind < 0.1:
            target = cur
        elif kind < 0.3:
            target = (cur + r.randrange(-12, 13)) & 0xFF
        elif kind < 0.85:
            target = r.randrange(256)
        elif kind < 0.95:
            target = r.choice([cur + 256, cur - 256, -1, 256, 261, -5, 512 + cur])
        else:
            target = r.randrange(INT_MIN, INT_MAX + 1)
        w.reset()
        o.calls.clear()
        w.set_vid(1, {"dirs": dirs, "dirStep": dir_step, "flags": flags})
        spr = w.sprite({"direction": cur, "action": action, "nvid": 1})
        ret = o.call(TURN_TO, [target], conv="thiscall", this=spr)
        ca = [e for e in o.calls if e[0] == "ca"]
        assert len(ca) <= 1
        cases.append([dirs, dir_step, flags, cur, action, target, ret, w.get(spr, "direction")] +
                     ([ca[0][2], ca[0][3]] if ca else [-1, 0]))
    return [family("turn_to", "TurnTo 0x40b0b0 (thiscall this=sprite, arg int32 dir; ret 4)", COLUMNS, cases,
                   notes="caAct -1 = no ChangeAction; dirStep = vid byte +0x127")]
