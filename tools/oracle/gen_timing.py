"""Time keeping:
  - game clock advance in SVGA::BeginFrame, range 0x401fb0..0x401feb (timeGetTime stubbed);
  - per-sprite tick scheduling in World::Tact, range 0x40bc35..0x40bc9d (real rand(),
    SPRITE::Tact 0x409a20 recorded);
  - countdown of the sprite timer (+0x33) in SPRITE::Tact, range 0x409b39..0x409b76."""
from common import family, rng
from structs import CURCLOCK, LAST_REAL, RAND_STATE, World

SPRITE_TACT = 0x409a20


def gen_clock(o, n=600):
    r = rng("clock")
    now = {}
    o.stub_import("WINMM.dll!timeGetTime", lambda o, args: now["t"], popped=0)
    cases = []
    for _ in range(n):
        clock = r.randrange(0, 1 << 32)
        last = r.randrange(0, 1 << 32)
        dt = r.choices([r.randrange(0, 100), 71, 72, 70, 0, r.randrange(0, 1 << 32), (1 << 32) - r.randrange(1, 100)],
                       [8, 1, 1, 1, 1, 1, 1])[0]
        now["t"] = (last + dt) & 0xFFFFFFFF
        o.w32(CURCLOCK, clock)
        o.w32(LAST_REAL, last)
        o.run(0x401fb0, [0x401feb])
        cases.append([clock, last, now["t"], o.u32(CURCLOCK), o.u32(LAST_REAL)])
    return family("clock", "SVGA::BeginFrame clock range 0x401fb0..0x401feb (curclock 0x42bf94, last real 0x42bf98)",
                  ["curclock", "lastReal", "timeGetTime", "curclock'", "lastReal'"], cases)


def gen_schedule(o, n=1200):
    r = rng("tick_schedule")
    w = World(o)
    o.stub(SPRITE_TACT, o.recorder("tact", 0), popped=0)
    cases = []
    for _ in range(n):
        fd = r.choices([71, 39, 28, 100, 0, 1, 3, 4, 7, 8, r.randrange(0, 1000), r.randrange(0, 65536), 65535],
                       [4, 2, 1, 1, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 2, 1, 0.3])[0]
        flags = r.choices([0, 0x100, 8, 0x108, 0x100 | 0x40], [3, 4, 0.5, 0.3, 1])[0]
        seed = r.randrange(0, 1 << 32)
        clock = r.randrange(0, 1 << 32)
        dt = r.choices([r.randrange(0, 120), fd, fd - 1, fd + 1, r.randrange(0, 1 << 32)], [6, 1, 1, 1, 0.5])[0]
        last = (clock - dt) & 0xFFFFFFFF
        w.reset()
        o.calls.clear()
        w.set_vid(1, {"frameDuration": fd, "flags": flags})
        spr = w.sprite({"nvid": 1, "lastTact": last})
        o.w32(RAND_STATE, seed)
        o.w32(CURCLOCK, clock)
        end = o.run(0x40bc35, [0x40bc97, 0x40bc9d, 0x40bca1], regs={"edi": spr, "ecx": 0})
        ticked = {0x40bc97: 1, 0x40bc9d: 0, 0x40bca1: 2}[end]
        assert (ticked == 1) == (len(o.calls) == 1)
        cases.append([fd, flags, seed, clock, last, ticked, o.u32(RAND_STATE)])
    return family("tick_schedule", "World::Tact scheduling range 0x40bc35..0x40bc9d (edi = sprite)",
                  ["frameDuration", "flags", "seed", "curclock", "lastTact", "ticked", "seed'"], cases,
                  notes="ticked: 1 = SPRITE::Tact called, 0 = not due, 2 = SkipUpdate (flag 8); "
                        "Randomized (0x100) consumes one rand() per call")


def gen_timer(o, n=600):
    r = rng("timer")
    w = World(o)
    cases = []
    for _ in range(n):
        timer = r.choices([0, r.randrange(0, 2000), r.randrange(0, 65536), 1, 65535], [1, 5, 2, 0.5, 0.5])[0]
        clock = r.randrange(0, 1 << 32)
        dt = r.choices([r.randrange(0, 100), timer, timer - 1, timer + 1, r.randrange(0, 1 << 32)], [5, 1, 1, 1, 1])[0]
        last = (clock - dt) & 0xFFFFFFFF
        frame = r.randrange(-32768, 32768)
        w.reset()
        spr = w.sprite({"timer": timer, "lastTact": last, "frame": frame})
        o.w32(CURCLOCK, clock)
        o.run(0x409b39, [0x409b76], regs={"esi": spr, "ebp": 0})
        cases.append([timer, clock, last, frame, w.get(spr, "timer"), w.get(spr, "frame")])
    return family("sprite_timer", "SPRITE::Tact range 0x409b39..0x409b76: frame++ and timer (+0x33) countdown",
                  ["timer", "curclock", "lastTact", "frame", "timer'", "frame'"], cases)


def generate(o):
    return [gen_clock(o), gen_schedule(o), gen_timer(o)]
