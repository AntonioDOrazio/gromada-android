"""Command stack step of SPRITE::Tact: code range 0x40a116..0x40a201 (docs/re/engine.md §3.3).

Runs with esi = sprite and the SpriteTact local `step` ([esp+0x24], computed by the physics
range) preset. The virtual Action (commands > 15 and act_nextcommand 45), ChangeAction
0x40b3e0 (commands <= 15) and the CRT free 0x41c881 are recorded. Stack memory:
{s16 count; s16 capacity; 9-byte records {u8 act; i32 var1; i32 var2}}; the top is e[count-1]."""
import struct

from common import family, rng
from structs import World

BEGIN, END = 0x40a116, 0x40a201
CHANGE_ACTION, FREE = 0x40b3e0, 0x41c881
STEP_LOCAL = 0x24
MAX_CMDS = 6


def generate(o, n=2000):
    r = rng("commands")
    w = World(o)
    o.stub(CHANGE_ACTION, o.recorder("ca", 2), popped=8)
    o.stub(FREE, o.recorder("free", 1, this=False), popped=0)
    cases = []
    for _ in range(n):
        w.reset()
        o.calls.clear()
        last = r.randrange(0, 40)
        frame = last + r.choice([1, 1, 0, -1, -3, 2])
        action = r.choices([0, 1, 2, 3, 4, 5, 6, 8, 15, 10], [2, 1, 3, 1, 2, 2, 1, 1, 1, 1])[0]
        state = r.choices([0, 0x10, 0x110, 0x13, 0x3, 0x20, 0x30, 0x80, 0x1, 0x40, 0x200], [3, 3, 2, 1, 1, 1, 1, 1, 1, 1, 1])[0]
        rot = r.choices([r.randrange(0, 30), r.randrange(-128, 128)], [5, 1])[0]
        step = r.choices([r.randrange(0, 40), 0x7fff, r.randrange(-100, 100)], [5, 1, 1])[0]
        ncmd = r.choices(range(MAX_CMDS + 1), [3, 3, 2, 1, 1, 1, 1])[0]
        cmds = []
        for _ in range(ncmd):
            act = r.choices([0, 2, 6, 8, 15, 30, 33, 45, 66, 74, r.randrange(256)], [1, 1, 1, 1, 1, 2, 2, 1, 1, 2, 1])[0]
            if act == 61:
                act = 60  # act_cyclecommand only at the bottom (as AddCommand guarantees)
            cmds.append([act, r.randrange(-50, 3000), r.randrange(-50, 3000)])
        if ncmd >= 2 and r.random() < 0.35:
            cmds[0] = [61, r.randrange(0, ncmd), r.randrange(0, 5)]  # act_cyclecommand(n) at the bottom
        count0 = ncmd
        if ncmd >= 2 and cmds[0][0] == 61 and r.random() < 0.5:
            count0 = 1  # e[1..] already executed: the next pop reaches act_cyclecommand
        elif ncmd >= 2 and r.random() < 0.2:
            count0 = r.randrange(1, ncmd + 1)
        if ncmd and r.random() < 0.3:
            cmds[count0 - 1][0] = 74  # act_submove on top
        w.set_vid(1, {"rotationPeriod": rot})
        spr = w.sprite({"frame": frame, "lastFrame": last, "action": action, "state": state, "nvid": 1})
        w.put(spr, "vtable", w.vtable("act"))
        mem = 0
        if ncmd:
            mem = o.alloc(4 + 9 * ncmd)
            o.write(mem, struct.pack("<hh", count0, ncmd) + b"".join(struct.pack("<Bii", a, v1, v2) for a, v1, v2 in cmds))
            w.put(spr, "commands", mem)
        o.run(BEGIN, [END], regs={"esi": spr, "ebp": 0}, stack_words=32, frame_init={STEP_LOCAL: step & 0xFFFFFFFF})
        acts = [e for e in o.calls if e[0] in ("act", "ca")]
        frees = [e for e in o.calls if e[0] == "free"]
        assert len(acts) <= 1 and len(frees) <= 1
        if acts and acts[0][0] == "act":
            kind, act, v1, v2 = 1, acts[0][2], acts[0][3], acts[0][4]
        elif acts:
            assert acts[0][3] == -1
            kind, act, v1, v2 = 2, acts[0][2] & 0xFF, 0, 0  # only the low byte is meaningful (dl)
        else:
            kind, act, v1, v2 = 0, -1, 0, 0
        ptr = w.get(spr, "commands")
        assert bool(frees) == (mem != 0 and ptr == 0)
        count = o.s16(mem) if ptr else -1
        case = [frame, last, action, state, rot, step, count0, kind, act, v1, v2, count, ncmd]
        for c in cmds:
            case += c
        cases.append(case)
    rec = family("commands", "SPRITE::Tact command step range 0x40a116..0x40a201 (esi = sprite, step local [esp+0x24])",
                 ["frame", "lastFrame", "action", "state", "rotationPeriod", "step", "count",
                  "kind", "act", "v1", "v2", "count'", "nCommands"], [],
                 notes="variable length cases: nCommands records e[0..n-1] (objectColumns) of which the first `count` are live, the top is e[count-1]; "
                       "kind 0 = nothing, 1 = virtual Action(act, v1, v2) (45 = act_nextcommand), 2 = ChangeAction(act, -1); "
                       "count' = stack count afterwards, -1 = no stack (freed or none)")
    rec["objectColumns"] = ["act", "var1", "var2"]
    rec["cases"] = cases
    return [rec]
