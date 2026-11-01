"""Static and computed tables: sin/cos (0x428e14/0x429214), CORDIC angles (0x429e18), the
rotation step tables built by 0x40cf70 (0x431bf4 left / 0x435bf4 right, 256 bytes per
direction count), the perspective tables (0x429614/0x429a14) and the cotangent table
(0x4316ac) at startup (0x40cf70) and after a map load (0x414f36..0x414f8f), and the vid
fields derived by the loader (0x4066ae..0x406717)."""
from common import CpuFault, family, rng
from structs import COT, NEG_COS, NEG_COS_SCALED, ROT_LEFT, ROT_RIGHT, SIN, SIN_SCALED, VID_SIZE

INIT_TABLES = 0x40cf70


def s32list(o, addr, n):
    return [o.s32(addr + 4 * i) for i in range(n)]


def gen_static(o):
    arrays = {"sin": s32list(o, SIN, 256), "negCos": s32list(o, NEG_COS, 256),
              "cordicAngles": [o.u8(0x429e18 + i) for i in range(7)]}
    o.call(INIT_TABLES, [])
    arrays["startupSinScaled"] = s32list(o, SIN_SCALED, 256)
    arrays["startupNegCosScaled"] = s32list(o, NEG_COS_SCALED, 256)
    arrays["startupCot"] = s32list(o, COT, 256)
    cases = []
    for d in range(1, 65):
        left = [o.u8(ROT_LEFT + 256 * d + i) for i in range(256)]
        right = [o.u8(ROT_RIGHT + 256 * d + i) for i in range(256)]
        cases.append([d] + left + right)
    rot = family("rotation_tables", "rotation step tables built by 0x40cf70: left 0x431bf4[dirs*256+dir], right 0x435bf4[dirs*256+dir]",
                 ["dirs"] + [f"l{i}" for i in range(256)] + [f"r{i}" for i in range(256)], cases,
                 notes="rows 1..64; row 64 of the left table is the same memory as row 0 of the right table")
    static = family("tables", "static .data tables and the startup tables of 0x40cf70", [], [], arrays=arrays)
    return [static, rot]


def gen_map_scale(o):
    pairs = [(24, 16), (16, 16), (32, 16), (7, 3), (1, 100), (-24, 16)]
    cases = []
    for sx, sy in pairs:
        o.run(0x414f36, [0x414f8f], regs={"edi": sx, "ebx": sy})
        for i in range(256):
            cases.append([sx, sy, i, o.s32(SIN_SCALED + 4 * i), o.s32(NEG_COS_SCALED + 4 * i), o.s32(COT + 4 * i)])
    return family("map_scale_tables", "map load loop 0x414f36..0x414f8f (edi = scaleX 0x428e04, ebx = scaleY 0x428e08)",
                  ["scaleX", "scaleY", "i", "sinScaled", "negCosScaled", "cot"], cases)


def gen_vid_derived(o):
    r = rng("vid_derived")
    vid = o.alloc(VID_SIZE)
    combos = []
    for dirs in [0, 1, 2, 3, 4, 5, 7, 8, 16, 24, 32, 33, 64, 100, 128, 255]:
        for rot in (0, 1, 5, 10, 21, 42, 64, 127, -1, -128):
            combos.append((dirs, r.randrange(-300, 300), r.randrange(-300, 300), r.randrange(-300, 300), rot))
    for _ in range(300):
        combos.append((r.randrange(1, 256), r.randrange(-32768, 32768), r.randrange(-32768, 32768),
                       r.randrange(-32768, 32768), r.randrange(-128, 128)))
    cases = []
    for dirs, sx, sy, sz, rot in combos:
        o.write(vid, b"\0" * VID_SIZE)
        o.w8(vid + 0x45, dirs)
        o.w16(vid + 0x27, sx)
        o.w16(vid + 0x29, sy)
        o.w16(vid + 0x2b, sz)
        o.w8(vid + 0x37, rot)
        try:
            o.run(0x4066ae, [0x406717], regs={"edi": vid, "ecx": 0})
            crash = 0
            out = [o.u8(vid + 0x127), o.s16(vid + 0x121), o.s16(vid + 0x123), o.s16(vid + 0x125),
                   o.s8(vid + 0x37), o.s32(vid + 0x128)]
        except CpuFault:
            crash, out = 1, [0] * 6
        cases.append([dirs, sx, sy, sz, rot, crash] + out)
    return family("vid_derived", "vid loader 0x4066ae..0x406717: derived fields",
                  ["dirs", "sizeX", "sizeY", "sizeZ", "rotationPeriod", "crash",
                   "dirStep", "halfX", "halfY", "absSizeZ", "rotationPeriodOut", "maxUnit"], cases,
                  notes="crash=1: dirs == 0 divides by zero")


def generate(o):
    return gen_static(o) + [gen_map_scale(o), gen_vid_derived(o)]
