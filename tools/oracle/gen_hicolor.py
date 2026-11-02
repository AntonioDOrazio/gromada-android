"""Hicolor (16-bit) colour tables and pixel operations of the SVGA object (0x42bfb8), issue #15.

The tables are built by SVGA::SetMode (0x404a80, table part 0x404ed0..0x40541e) from the RGB
masks of the DirectDraw surface. The generator emulates that code with an RGB565 surface
(R 0xf800, G 0x07e0, B 0x001f: the usual format of the time) and records:
  - the derived masks and shifts, the blue brighten tables (+0x2f), the blend table (+0x243),
    the darken table ([+0x27]); the 64 RG brighten tables ([+0x2b]) as FNV-1a hashes;
  - RGB -> 565 conversion (0x405560);
  - Effect() screen operations on a buffer holding all 65536 pixel values: white flash
    0x405da0, per-channel flash 0x405e20, fade 0x405c40 (hash of the whole output);
  - SetGamma on a palette (0x4072c0) for every gamma 0..0xfff and a sample above 0xfff
    (hash of the 256 output entries), full outputs for the gammas used by the scripts;
  - putcadr (0x4021f0) on small synthetic frames (formats 2, 3, 4, 8, cast shadow pass)
    drawn over a background holding varied colours: the output pixels.
Only numbers are written (inputs chosen here and outputs of the original code).
"""
import struct

import oracle
from common import family, rng

SVGA = 0x42bfb8
MALLOC = 0x41bfb1
SETMODE_TABLES, SETMODE_END = 0x404ed0, 0x40541e
RGB_TO_16 = 0x405560
FLASH_WHITE, FLASH_RGB, FADE = 0x405da0, 0x405e20, 0x405c40
GAMMA = 0x4072c0
PUTCADR = 0x4021f0
VIDS_PTR, NVIDS = 0x42df57, 0x42df4f
SURFACE = SVGA + 0x1f9b          # locked surface pointer
PITCH = SVGA + 0x1a50            # pixels per row
CLIP = SVGA + 0x1a54             # left, right, top, bottom (inclusive)
SHADOW_FLAG = 0x42df8c
HALF_MASK_DWORD = 0x42bfa4       # +6 duplicated in both words, set by BeginFrame 0x401f70
VID_SIZE = 0x136

RGB565 = (0xF800, 0x07E0, 0x001F)
SCRIPT_GAMMAS = [0x777, 2167, 1943, 1927, 1911, 1860, 2166, 1608, 1908, 1877, 1094, 1879, 96, 64, 624]


def fnv(words):
    h = 0x811C9DC5
    for w in words:
        h = ((h ^ (w & 0xFFFF)) * 0x01000193) & 0xFFFFFFFF
    return h


def words(o, addr, n):
    return list(struct.unpack(f"<{n}H", o.read(addr, 2 * n)))


def setup_tables(o):
    oracle.MAX_INSNS = max(oracle.MAX_INSNS, 20_000_000)   # the table loops run ~5M instructions
    o.stub(MALLOC, lambda o, a: o.alloc(a(0)), 0)
    for i, m in enumerate(RGB565):
        o.w16(SVGA + 2 * i, m)
    o.run(SETMODE_TABLES, [SETMODE_END], regs={"ebp": SVGA, "esi": 0, "edi": 480})


def gen_tables(o):
    arrays = {
        "shifts": [o.s32(SVGA + 8), o.s32(SVGA + 0x10), o.s32(SVGA + 0xc)],          # R G B
        "bits": [o.s32(SVGA + 0x14), o.s32(SVGA + 0x1c), o.s32(SVGA + 0x18)],
        "shift5": [o.u8(SVGA + 0x20), o.u8(SVGA + 0x21), o.u8(SVGA + 0x22)],
        "halfMask": [o.u16(SVGA + 6)],
        "grey": [o.u16(SVGA + 0x1a82)],
        "fadeMasks": [o.u32(SVGA + 0x22f + 4 * i) for i in range(5)],
        "blueBrighten": words(o, SVGA + 0x2f, 8 * 32),
        "blend": list(o.read(SVGA + 0x243, 6 * 1024)),
        "darken": words(o, o.u32(SVGA + 0x27), 6 * 2048),
    }
    rg = o.u32(SVGA + 0x2b)
    arrays["rgBrightenHash"] = [fnv(words(o, rg + 4096 * t, 2048)) for t in range(64)]
    arrays["rgBrightenSample"] = words(o, rg + 4096 * 9, 2048)   # table r=1, g=1
    cases = []
    for c in range(3):
        for v in range(256):
            rgbv = [0, 0, 0]
            rgbv[c] = v
            cases.append(rgbv + [o.call(RGB_TO_16, rgbv, conv="thiscall", this=SVGA) & 0xFFFF])
    r = rng("hicolor_rgb")
    for _ in range(256):
        rgbv = [r.randrange(256) for _ in range(3)]
        cases.append(rgbv + [o.call(RGB_TO_16, rgbv, conv="thiscall", this=SVGA) & 0xFFFF])
    for rgbv in ([0x53, 0xa5, 0x36], [0x38, 0x91, 0xa5], [0x23, 0x58, 0x64], [0xc8, 0x32, 0x32], [0xc8, 0xc8, 0x32]):
        cases.append(rgbv + [o.call(RGB_TO_16, rgbv, conv="thiscall", this=SVGA) & 0xFFFF])
    return family("hicolor_tables", "SVGA::SetMode tables 0x404ed0..0x40541e (RGB565) and RGB->16 bit 0x405560",
                  ["r", "g", "b", "color16"], cases, arrays=arrays,
                  notes="rgBrightenHash: FNV-1a of the 2048 words of each RG table t = rLevel*8 + gLevel")


def gen_effects(o):
    buf = o.alloc(2 * 65536)
    every = b"".join(i.to_bytes(2, "little") for i in range(65536))
    o.w32(SURFACE, buf)
    cases = []

    def op(fn, kind, level):
        o.write(buf, every)
        o.call(fn, [0, 65536, level], conv="thiscall", this=SVGA)
        out = words(o, buf, 65536)
        cases.append([kind, level, fnv(out), out[0x1234], out[0xffff], out[0x8410], out[0x7bef]])

    for level in range(0, 9):
        op(FLASH_WHITE, 1, level)
    for p in range(512):
        op(FLASH_RGB, 2, p)
    for level in range(0, 10):
        op(FADE, 5, level)
    return family("hicolor_effects", "Effect() pixel operations on all 65536 values: 0x405da0 (kind 1), 0x405e20 (2), 0x405c40 (5)",
                  ["kind", "level", "hash", "out1234", "outFFFF", "out8410", "out7BEF"], cases)


def palette_input():
    r = rng("hicolor_palette")
    pal = [0, 0xFFFF, 0xF800, 0x07E0, 0x001F, 0x8410, 0x7BEF, 0x0821, 0xF81F, 0x07FF, 0xFFE0]
    while len(pal) < 256:
        pal.append(r.randrange(65536))
    return pal


def gen_gamma(o):
    pal = palette_input()
    raw = b"".join(v.to_bytes(2, "little") for v in pal)
    buf = o.alloc(512)
    cases = []
    full = {}
    r = rng("hicolor_gamma")
    gammas = list(range(0x1000)) + sorted({r.randrange(0x1000, 0x10000) for _ in range(200)} | {0xf000, 0xffff, 0x1777, 0x8777})
    for g in gammas:
        o.write(buf, raw)
        o.call(GAMMA, [buf, g], conv="thiscall", this=SVGA)
        out = words(o, buf, 256)
        cases.append([g, fnv(out)])
        if g in SCRIPT_GAMMAS:
            full[f"g{g}"] = out
    arrays = {"palette": pal}
    arrays.update(full)
    return family("hicolor_gamma", "SVGA gamma on a 16-bit palette 0x4072c0 (thiscall: palette, gamma)",
                  ["gamma", "hash"], cases, arrays=arrays, notes="hash: FNV-1a of the 256 output entries")


# ---- putcadr on synthetic frames ---------------------------------------------------------
W, H = 40, 12        # frame size
FBW, FBH = 56, 18    # surface


def frame_mode2(r):
    """Format 2: per line [count|cmd]: 0x00 skip, 0x40 shadow, 0x80 literal, 0xc0 repeat."""
    out = bytearray()
    out += (1).to_bytes(2, "little") + (H - 2).to_bytes(2, "little")   # startY, lines
    for y in range(1, H - 1):
        x = 0
        while x < W:
            n = min(W - x, r.randrange(1, 9))
            kind = r.randrange(4)
            if kind == 0:
                out.append(n)
            elif kind == 1:
                out.append(0x40 | n)
            elif kind == 2:
                out.append(0x80 | n)
                out += bytes(r.randrange(256) for _ in range(n))
            else:
                out.append(0xC0 | n)
                out.append(r.randrange(256))
            x += n
        out.append(0)
    return bytes(out)


def frame_mode3(r):
    """Format 3: [count:5 | factor:3]; factor 0 skips."""
    out = bytearray((1).to_bytes(2, "little") + (H - 2).to_bytes(2, "little"))
    for y in range(1, H - 1):
        x = 0
        while x < W:
            n = min(W - x, r.randrange(1, 9))
            out.append(n | (r.randrange(8) << 5))
            x += n
        out.append(0)
    return bytes(out)


def frame_mode4(r):
    """Format 4: u16 [count:7 | b:3 | g:3 | r:3]."""
    out = bytearray((1).to_bytes(2, "little") + (H - 2).to_bytes(2, "little"))
    for y in range(1, H - 1):
        x = 0
        while x < W:
            n = min(W - x, r.randrange(1, 9))
            out += (n | (r.randrange(8) << 7) | (r.randrange(8) << 10) | (r.randrange(8) << 13)).to_bytes(2, "little")
            x += n
        out += b"\0\0"
    return bytes(out)


def frame_mode8(r):
    """Format 8: [count:5 | opacity:3]; 0 skips, 7 opaque, 1..6 blend; indices follow."""
    out = bytearray((1).to_bytes(2, "little") + (H - 2).to_bytes(2, "little"))
    for y in range(1, H - 1):
        x = 0
        while x < W:
            n = min(W - x, r.randrange(1, 9))
            op = r.randrange(8)
            out.append(n | (op << 5))
            if op:
                out += bytes(r.randrange(256) for _ in range(n))
            x += n
        out.append(0)
    return bytes(out)


def gen_putcadr(o):
    r = rng("hicolor_putcadr")
    pal = [r.randrange(65536) for _ in range(256)]
    background = [r.randrange(65536) for _ in range(FBW * FBH)]
    surface = o.alloc(2 * FBW * FBH)
    vids = o.alloc(VID_SIZE * 4)
    o.w32(VIDS_PTR, vids)
    o.w32(NVIDS, 4)
    o.w32(PITCH, FBW)
    for i, v in enumerate((0, FBW - 1, 0, FBH - 1)):
        o.w32(CLIP + 4 * i, v)
    o.w32(SURFACE, surface)
    o.w32(SHADOW_FLAG, 0)
    half = o.u16(SVGA + 6)
    o.w32(HALF_MASK_DWORD, half | (half << 16))
    cases = []
    frames = {}
    for nvid, (fmt, maker) in enumerate(((2, frame_mode2), (3, frame_mode3), (4, frame_mode4), (8, frame_mode8))):
        data = maker(r)
        frames[f"frame{fmt}"] = list(data)
        blob = b"".join(v.to_bytes(2, "little") for v in pal) + (0xFFFF).to_bytes(2, "little") + data
        gfx = o.alloc(len(blob))
        o.write(gfx, blob)
        offsets = o.alloc(4)
        o.w32(offsets, 0x200)
        v = vids + VID_SIZE * nvid
        o.w8(v + 0x107, fmt)
        o.w16(v + 0x10a, 1)
        o.w16(v + 0x10c, W)
        o.w16(v + 0x10e, H)
        o.w32(v + 0x114, offsets)
        o.w32(v + 0x118, gfx)
        o.w8(v + 0x120, 1)
        o.w16(v + 0x130, 0xFFFF)
        for shadow in (0, 1):
            o.w16(v + 0x24, 0x80 if shadow else 0)
            for (x, y, z) in ((8, 6, 0), (-5, 3, 0), (30, -4, 0), (12, 9, 6), (20, 4, 13)):
                o.write(surface, b"".join(c.to_bytes(2, "little") for c in background))
                o.call(PUTCADR, [nvid, 0, x, y, z], conv="thiscall", this=SVGA)
                out = words(o, surface, FBW * FBH)
                cases.append([fmt, shadow, x, y, z] + out)
    arrays = {"palette": pal, "background": background}
    arrays.update(frames)
    return family("hicolor_putcadr", f"putcadr 0x4021f0 (nvid, frame 0, x, y, z) of synthetic {W}x{H} frames on a {FBW}x{FBH} surface",
                  ["format", "shadowFlag", "x", "y", "z"] + [f"p{i}" for i in range(FBW * FBH)], cases, arrays=arrays,
                  notes="frame data = the bytes after the 2-byte frame header; shadowFlag sets vid flags 0x80 (cast shadow)")


def generate(o):
    setup_tables(o)
    return [gen_tables(o), gen_effects(o), gen_gamma(o), gen_putcadr(o)]
