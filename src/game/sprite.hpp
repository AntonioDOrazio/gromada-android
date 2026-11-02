#pragma once
// Simulation objects of gromada.exe (docs/re/engine.md §2): the per-vid simulation data
// (VidInfo, mutable like the original vid table) and the SPRITE structure with its
// class extensions. Header-only.

#include "core/std.hpp"
#include "libgromada/resources.hpp"

namespace gromada::game {

using UnitId = std::int32_t;

// SPRITE::state bits (+0x1c), docs/re/engine.md §2.5.
namespace st {
inline constexpr std::uint16_t ArrivedX = 0x0001, ArrivedY = 0x0002, Screen = 0x0004, Sub = 0x0008,
                               Move = 0x0010, Attack = 0x0020, Passive = 0x0040, Constructing = 0x0080,
                               Accelerate = 0x0100, SoundDone = 0x0200, Invisible = 0x0400, Jump = 0x0800,
                               GammaMask = 0x3000, CommandMask = 0x00f0;
}

// vid flags (ObjectFlags) as plain bits.
namespace vf {
inline constexpr std::uint16_t RandomDirection = 0x1, Gravity = 0x2, Calltact = 0x4, SkipUpdate = 0x8,
                               SpawnChildren = 0x10, NoCollision = 0x20, PresentOnGrid = 0x40, Shadow = 0x80,
                               Randomized = 0x100, ZeroZ = 0x200, InvisibleSubobjects = 0x400,
                               IgnoreGlobalGamma = 0x800, Wind = 0x1000, CrushOnCollision = 0x4000;
}

// ObjectCategory bits.
namespace cat {
inline constexpr std::uint8_t Terrain = 0x1, Object = 0x2, Vehicle = 0x4, Avia = 0x8, Cannon = 0x10, Sprite = 0x20,
                              Item = 0x40;
}

// C++ classes of CreateUnit (ObjectClass values).
namespace cls {
inline constexpr std::uint8_t Terrain = 0, Static = 1, Vehicle = 2, Building = 3, Avia = 4, Projectile = 5,
                              CannonMissile = 6, DownedAvia = 7, NA = 8, Superstructure = 9, Image = 10,
                              Effect1 = 11, Effect2 = 12, Mine = 13, Shell = 14, AviaMissile = 15, Debris = 16,
                              Kassandra = 17, Bonus = 18, Font = 19, RepairCannon = 20;
}

// Action numbers used by the engine (actions.hpp has the full list with names).
namespace act {
inline constexpr int stand = 0, build = 1, go = 2, start_move = 3, l_rotate = 4, r_rotate = 5, open = 6, close = 7,
                     fight = 8, salut = 9, stand_open = 10, load = 11, unload = 12, wound = 13, birth = 14,
                     death = 15, path_block = 31, additem = 32, deleteitem = 33, askitem = 34, changeweapon = 35,
                     deleteallitem = 36, changeaction = 37, path_limit = 38, attack = 39, move = 40,
                     buildunit = 41, drawselected = 42, damage = 43, select = 44, nextcommand = 45, init = 46,
                     uncom = 47, deletetarget = 48, calltact = 49, save = 51, restore = 52, destroy = 54,
                     coor_attack = 56, child = 57, saveframe = 58, fullsave = 59, gettarget = 60,
                     cyclecommand = 61, changevid = 62, addammo = 63, getammo = 64, gethp = 65, clearcom = 66,
                     setbehave = 67, changecoor = 68, getlink = 69, jump = 70, settext = 71, gettext = 72,
                     setfile = 73, submove = 74, setarmy = 75, getarmy = 76, setbuildtime = 77,
                     getbuildtime = 78, setstate = 79, getstate = 80, getbehave = 81, getaction = 82,
                     load_object = 83, unload_object = 84;
}

// Per-vid simulation data. Mirrors the in-memory vid of the original (0x136 bytes, §2.1),
// including the fields scripts can change at run time (SetVid, MaxUnit) and the ones the
// engine itself writes (weapon of the flagman's vid).
struct VidInfo {
    const Vid* src = nullptr;
    const VidGraphics* gfx = nullptr;   // graphics (shared with the source vid, 0x130)
    std::uint8_t category = 0;
    std::uint8_t cls = 0;
    std::uint16_t flags = 0;
    std::uint8_t collisionMask = 0;
    std::int16_t sizeX = 0, sizeY = 0, sizeZ = 0;
    std::int16_t halfX = 0, halfY = 0, absSizeZ = 0;   // 0x121, 0x123, 0x125
    std::uint8_t maxHP = 0;
    std::int16_t visibilityRadius = 0;
    std::int16_t speedX = 0, speedY = 0;
    std::uint16_t acceleration = 0;
    std::int8_t rotationPeriod = 0;   // already rotationPeriod*6/dirs (0x406705)
    std::uint8_t army = 0;
    std::uint8_t weapon = 0;
    std::int16_t deathDamageRadius = 0;
    std::uint8_t deathDamage = 0;
    std::int8_t linkX = 0, linkY = 0, linkZ = 0;
    std::uint16_t linkedObjectVid = 0;
    std::uint8_t dirs = 1;
    std::uint8_t zLayer = 0;
    std::uint8_t dirStep = 0;          // 0x127 = (byte)(256/dirs)
    std::array<std::uint8_t, 16> animLen{};
    std::array<std::uint16_t, 16> nsfx{};
    std::array<std::array<std::int16_t, 16>, 3> childOff{};
    std::array<std::int16_t, 16> childNvid{};
    std::array<std::uint8_t, 16> childCount{};
    std::uint16_t frameDuration = 71;
    std::uint16_t numFrames = 0, width = 0, height = 0;
    std::int32_t maxUnit = -1;         // 0x128
    std::int32_t count = 0;            // 0x12c
    // SetVid(VID_GAMMA+k): alternative palettes (0x132), selected by state & 0x3000.
    std::shared_ptr<std::array<std::array<ColorRgb8, 256>, 4>> altPalettes;
};

struct Command {
    std::uint8_t act = 0;
    std::int32_t var1 = 0, var2 = 0;
};

// The command stack of a sprite (+0x3b): {s16 count; s16 capacity; 9-byte records}. The
// top is e[count-1]. Storage beyond `count` keeps its old contents like the original buffer
// (act_cyclecommand jumps back into it).
struct CommandStack {
    std::vector<Command> e;
    int count = 0;
    [[nodiscard]] const Command& top() const { return e[count - 1]; }
    void ensure(int n) { if (static_cast<int>(e.size()) < n) e.resize(n); }
};

struct Sprite {
    // base SPRITE (0x3f bytes)
    Sprite* next = nullptr;            // z-layer list
    Sprite* child = nullptr;           // sub-object chain
    std::int16_t x = 0, y = 0, z = 0;
    std::uint8_t direction = 0, action = 0;
    std::uint16_t nvid = 0;
    std::int16_t frame = 0, firstFrame = 0, lastFrame = 0;
    std::uint16_t state = 0;
    std::uint32_t lastTact = 0;
    std::int16_t speed = 0, vz = 0;
    std::int8_t fracX = 0, fracY = 0, fracZ = 0;
    std::uint8_t stepAccum = 0;
    std::int8_t dx = 0, dy = 0, dz = 0;
    std::int16_t goX = 0, goY = 0, goZ = 0;
    std::uint16_t timer = 0;
    std::int16_t attackX = 0, attackY = 0, attackZ = 0;
    std::unique_ptr<CommandStack> commands;
    // material (+0x3f)
    std::uint8_t hp = 0;
    // units (Vehicle, Building, Avia, Mine, Kassandra)
    UnitId target = 0;                 // +0x40 (an id, not a pointer: see docs/engine.md)
    std::uint8_t army = 0;             // +0x44
    std::uint8_t behave = 0;           // +0x45
    std::int16_t wpX = -1, wpY = -1;   // +0x46/+0x48
    std::uint8_t buildTime = 20;       // +0x4a
    std::int8_t turn = 0;              // +0x4b
    std::vector<std::int16_t> items;   // +0x4c
    std::uint16_t buildNvid = 0;       // Building +0x54
    std::array<std::int16_t, 8> ammo{};// Kassandra +0x54..+0x63 (ammo[4] = jumps, +0x5c)
    // engine bookkeeping
    UnitId id = 0;
    std::uint8_t cls = 0;              // C++ class (sub-objects are always Superstructure)
    bool alive = false;
    bool onGrid = false;               // inserted in the grid cells (diagnostics)
};

} // namespace gromada::game
