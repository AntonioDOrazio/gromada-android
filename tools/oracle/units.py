"""Unit-level structures for the generators of issue #12 (weapons, unit fields, the grid's unit
list, real class vtables). Offsets from docs/re/engine.md §2 and the disassembly; they extend
structs.py without changing it."""
from structs import GRID, SPR, VID, World, _get, _put

WEAPONS_PTR, NWEAPONS = 0x439d3c, 0x439d40
WEAPON_SIZE = 9
INIT_TABLES = 0x40cf70     # rotation tables 0x431bf4/0x435bf4 (TurnTo, rotations)

# More vid fields (structs.VID has the ones of issue #11).
VID_EXT = dict(VID)
VID_EXT.update({
    "linkX": (0x3e, 1, True), "linkY": (0x3f, 1, True), "linkZ": (0x40, 1, True),
    "linkedObjectVid": (0x41, 2, False),
})
VID_CHILD_OFF = (0x77, 0x97, 0xb7)   # s16[16] x, y, z offsets of the children per action
VID_CHILD_NVID = 0xd7                # s16[16]
VID_CHILD_COUNT = 0xf7               # u8[16]

# More sprite fields: base SPRITE attack point and the unit classes (Vehicle and derived).
SPR_EXT = dict(SPR)
SPR_EXT.update({
    "attackX": (0x35, 2, True), "attackY": (0x37, 2, True), "attackZ": (0x39, 2, True),
    "target": (0x40, 4, False), "army": (0x44, 1, False), "behave": (0x45, 1, False),
    "wpX": (0x46, 2, True), "wpY": (0x48, 2, True), "buildTime": (0x4a, 1, False), "turn": (0x4b, 1, True),
    "buildNvid": (0x54, 2, False),
})
for _k in range(8):   # Kassandra ammunition +0x54..+0x63
    SPR_EXT[f"ammo{_k}"] = (0x54 + 2 * _k, 2, True)

# Real vtables of the classes (docs/re/engine.md §2.3): virtual calls run the original code.
VTABLE = {"Static": 0x426214, "Vehicle": 0x42620c, "Building": 0x426210, "Avia": 0x426204,
          "Projectile": 0x426200, "Kassandra": 0x4261d4, "Superstructure": 0x4261c4}
ACTION = {"material": 0x412580, "Vehicle": 0x40ea20, "Building": 0x410e10, "Projectile": 0x40f710,
          "Kassandra": 0x411a60}


def put(o, p, name, value):
    _put(o, p, SPR_EXT, name, value)


def get(o, p, name):
    return _get(o, p, SPR_EXT, name)


def set_vid_ext(w, n, fields, anim_lengths=None, child_nvid=None, child_count=None, child_off=None):
    """structs.World.set_vid plus the fields of VID_EXT and the children tables."""
    base = w.set_vid(n, {k: v for k, v in fields.items() if k in VID}, anim_lengths, child_nvid)
    for k, v in fields.items():
        if k not in VID:
            _put(w.o, base, VID_EXT, k, v)
    for i, v in enumerate(child_count or []):
        w.o.w8(base + VID_CHILD_COUNT + i, v)
    for axis, row in enumerate(child_off or []):
        for i, v in enumerate(row):
            w.o.w16(base + VID_CHILD_OFF[axis] + 2 * i, v)
    return base


def set_weapons(o, weapons):
    """weapons: list of dicts {targetCategory, flags, range, scatter, cooldown}."""
    p = o.alloc(WEAPON_SIZE * max(1, len(weapons)))
    for i, wpn in enumerate(weapons):
        b = p + WEAPON_SIZE * i
        o.w8(b, wpn.get("targetCategory", 0))
        o.w8(b + 1, wpn.get("flags", 0))
        o.w16(b + 2, 0)
        o.w16(b + 4, wpn.get("range", 0))
        o.w8(b + 6, wpn.get("scatter", 0))
        o.w16(b + 7, wpn.get("cooldown", 0))
    o.w32(WEAPONS_PTR, p)
    o.w32(NWEAPONS, len(weapons))
    return p


def set_unit_list(o, sprites):
    """The grid's list of Vehicle/Avia units (0x431ab0: s16 count, +4 Sprite**), in order."""
    arr = o.alloc(4 * max(4, len(sprites)))
    for i, s in enumerate(sprites):
        o.w32(arr + 4 * i, s)
    o.w16(GRID, len(sprites))
    o.w16(GRID + 2, max(4, len(sprites)))
    o.w32(GRID + 4, arr)


class ArmyVtable:
    """One shared stub for the virtual Action of synthetic sprites: records (this, act, v1, v2)
    and answers act_getarmy (76) from `army[this]`."""

    def __init__(self, o):
        self.o = o
        self.army = {}

        def cb(o, args):
            this = o.reg("ecx")
            entry = ["act", this, args(0), args(1), args(2)]
            o.calls.append(entry)
            return self.army.get(this, 0) if args(0) == 76 else 0
        self.stub = o.new_stub(cb, popped=12)

    def vtable(self):
        vt = self.o.alloc(4)
        self.o.w32(vt, self.stub)
        return vt


def unit_world(o, nvids=16):
    return World(o, nvids=nvids)
