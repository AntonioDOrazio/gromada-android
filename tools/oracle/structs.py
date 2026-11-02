"""Synthetic engine structures in emulated memory (layouts from docs/re/engine.md §2).

Only the fields read by the functions under test are written; everything else is zero.
All offsets are those of the original (packed MSVC structs).
"""
from oracle import s16

# ---- globals (docs/re/engine.md) -----------------------------------------------------------
VIDS_PTR = 0x42df57        # Vid* table
NVIDS = 0x42df4f
CURCLOCK = 0x42bf94
LAST_REAL = 0x42bf98
G_DIST = 0x439d4c          # distance written by GetDirection
RAND_STATE = 0x42aebc
SCALE_X, SCALE_Y = 0x428e04, 0x428e08
MAP_W, MAP_H = 0x428e0c, 0x428e10
WIND_STRENGTH = 0x439d54   # s16
WIND_DIR = 0x429e14        # dword, & 0xff
PAUSED = 0x439d38          # u8
GRID = 0x431ab0            # spatial grid object
SIN, NEG_COS = 0x428e14, 0x429214
SIN_SCALED, NEG_COS_SCALED, COT = 0x429614, 0x429a14, 0x4316ac
ROT_LEFT, ROT_RIGHT = 0x431bf4, 0x435bf4

VID_SIZE = 0x136
SPRITE_SIZE = 0x64         # largest class (Kassandra); the base SPRITE is 0x3f bytes

# Vid fields: name -> (offset, size, signed)
VID = {
    "category": (0x22, 1, False), "cls": (0x23, 1, False), "flags": (0x24, 2, False),
    "collisionMask": (0x26, 1, False), "sizeX": (0x27, 2, True), "sizeY": (0x29, 2, True),
    "sizeZ": (0x2b, 2, True), "maxHP": (0x2d, 1, False), "visibilityRadius": (0x2e, 2, False),
    "speedX": (0x31, 2, True), "speedY": (0x33, 2, True), "acceleration": (0x35, 2, False),
    "rotationPeriod": (0x37, 1, True), "army": (0x38, 1, False), "weapon": (0x39, 1, False),
    "deathDamageRadius": (0x3b, 2, False), "deathDamage": (0x3d, 1, False),
    "dirs": (0x45, 1, False), "zLayer": (0x46, 1, False),
    "frameDuration": (0x108, 2, False),
    "halfX": (0x121, 2, True), "halfY": (0x123, 2, True), "absSizeZ": (0x125, 2, True),
    "dirStep": (0x127, 1, False),
}
VID_ANIM_LENGTHS = 0x47    # u8[16]
VID_CHILD_NVID = 0xd7      # s16[16]

# Sprite fields
SPR = {
    "vtable": (0x00, 4, False), "next": (0x04, 4, False), "child": (0x08, 4, False),
    "x": (0x0c, 2, True), "y": (0x0e, 2, True), "z": (0x10, 2, True),
    "direction": (0x12, 1, False), "action": (0x13, 1, False), "nvid": (0x14, 2, False),
    "frame": (0x16, 2, True), "firstFrame": (0x18, 2, True), "lastFrame": (0x1a, 2, True),
    "state": (0x1c, 2, False), "lastTact": (0x1e, 4, False), "speed": (0x22, 2, True),
    "vz": (0x24, 2, True), "fracX": (0x26, 1, True), "fracY": (0x27, 1, True),
    "fracZ": (0x28, 1, True), "stepAccum": (0x29, 1, False),
    "goX": (0x2d, 2, True), "goY": (0x2f, 2, True), "goZ": (0x31, 2, True),
    "timer": (0x33, 2, False), "commands": (0x3b, 4, False), "hp": (0x3f, 1, False),
}


def _put(o, base, layout, name, value):
    off, size, _ = layout[name]
    {1: o.w8, 2: o.w16, 4: o.w32}[size](base + off, value)


def _get(o, base, layout, name):
    off, size, signed = layout[name]
    if size == 1:
        return o.s8(base + off) if signed else o.u8(base + off)
    if size == 2:
        return o.s16(base + off) if signed else o.u16(base + off)
    return o.s32(base + off) if signed else o.u32(base + off)


def vid_derived(fields):
    """Fields computed by the vid loader (0x4066a8..0x406717), see gen_tables.py vid_derived."""
    dirs = fields.get("dirs", 1)
    d = dict(fields)
    d.setdefault("halfX", s16(fields.get("sizeX", 0)) >> 1)
    d.setdefault("halfY", s16(fields.get("sizeY", 0)) >> 1)
    d.setdefault("absSizeZ", abs(s16(fields.get("sizeZ", 0))))
    d.setdefault("dirStep", (256 // dirs) & 0xFF if dirs else 0)
    return d


class World:
    """A vid table and sprites in the oracle heap. Call `reset()` before each case."""

    def __init__(self, o, nvids=8):
        self.o = o
        self.nvids = nvids

    def reset(self):
        o = self.o
        o.heap_reset()
        self.vids = o.alloc(VID_SIZE * self.nvids)
        o.w32(VIDS_PTR, self.vids)
        o.w32(NVIDS, self.nvids)

    def vid(self, n):
        return self.vids + VID_SIZE * n

    def set_vid(self, n, fields, anim_lengths=None, child_nvid=None):
        base = self.vid(n)
        self.o.write(base, b"\0" * VID_SIZE)
        for k, v in vid_derived(fields).items():
            _put(self.o, base, VID, k, v)
        for i, v in enumerate(anim_lengths or []):
            self.o.w8(base + VID_ANIM_LENGTHS + i, v)
        for i, v in enumerate(child_nvid or []):
            self.o.w16(base + VID_CHILD_NVID + 2 * i, v)
        return base

    def sprite(self, fields):
        p = self.o.alloc(SPRITE_SIZE)
        for k, v in fields.items():
            _put(self.o, p, SPR, k, v)
        return p

    def get(self, p, name):
        return _get(self.o, p, SPR, name)

    def put(self, p, name, value):
        _put(self.o, p, SPR, name, value)

    def vtable(self, tag, ret=0):
        """A 1-slot vtable whose Action(act, v1, v2) records (tag, this, act, v1, v2)."""
        cache = self.o.__dict__.setdefault("vtable_stubs", {})
        key = (tag, ret)
        if key not in cache:  # one hook per tag: unicorn slows down with thousands of hooks
            cache[key] = self.o.new_stub(self.o.recorder(tag, 3, ret=ret), popped=12)
        vt = self.o.alloc(4)
        self.o.w32(vt, cache[key])
        return vt


class Grid:
    """The spatial grid object at 0x431ab0 (docs/re/engine.md §5.3), filled synthetically.

    +0x08 cells (8 bytes each: s16 n, s16 cap, Sprite** v), +0x0c row shift (cells index =
    (cy << rowShift) + cx), +0x0e nox, +0x10 noy, +0x12 x shift, +0x14 y shift.
    """

    def __init__(self, o, shift_x, shift_y, nox, noy):
        self.o = o
        self.shift_x, self.shift_y, self.nox, self.noy = shift_x, shift_y, nox, noy
        self.row_shift = max(1, (nox - 1).bit_length())
        ncells = noy << self.row_shift
        self.cells = o.alloc(8 * ncells)
        self.content = {}
        o.w32(GRID + 0x08, self.cells)
        o.w8(GRID + 0x0c, self.row_shift)
        o.w16(GRID + 0x0e, nox)
        o.w16(GRID + 0x10, noy)
        o.w16(GRID + 0x12, shift_x)
        o.w16(GRID + 0x14, shift_y)

    def add(self, cx, cy, sprite):
        self.content.setdefault((cx, cy), []).append(sprite)

    def commit(self):
        o = self.o
        for (cx, cy), lst in self.content.items():
            cell = self.cells + 8 * ((cy << self.row_shift) + cx)
            arr = o.alloc(4 * max(4, len(lst)))
            for i, s in enumerate(lst):
                o.w32(arr + 4 * i, s)
            o.w16(cell, len(lst))
            o.w16(cell + 2, max(4, len(lst)))
            o.w32(cell + 4, arr)
