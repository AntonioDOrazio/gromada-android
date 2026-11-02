#pragma once
// Save states of the game (issue #14): RetroArch save/load, rewind and run-ahead.
// Format, coverage and limits: docs/savestates.md.
//
// A state is a 64-byte header (magic, version, sizes, 64-bit checksum of header and payload, fingerprint of
// fw.res) followed by chunks {u32 tag, u32 size, data}:
//   CORE  extras of the frontend glue (audio sample debt, input mapper: INP1)
//   SESS  the session (map, script identity, UI, files, input record builder, demo playback
//         and recording, audio playlist, timers, posted operations, carried cells...)
//   SVM1  the script interpreter (cells, patched code, string memory), when a script runs
//   ENGN  the engine: World (the whole sprite pool with links as slot indices, z-layer lists,
//         grid cells in their order, free lists, players, squads, vids changed at run time,
//         weapons, clock, pause, RNG, wind...) and GameEngine (camera, effects, gamma,
//         lights, palette cache, iterators)
//   MIX1  the mixer (music track and exact position, sound-effect voices)
//
// Saving reads the live objects directly. Loading is two-phase: every chunk is decoded and
// validated into staged values first (a truncated, corrupted or foreign state throws
// state::Error and nothing changes), then the staged values are moved in.
//
// Header-only; the integration module of the save states: depends on the session, the engine
// implementation (GameEngine), the script VM, ui, input and the mixer.

#include "core/state_io.hpp"
#include "engine/audio_state.hpp"
#include "game/engine.hpp"
#include "game/session.hpp"
#include "input/mapper_state.hpp"
#include "script/vm_state.hpp"
#include "ui/text_state.hpp"

#include <chrono>

namespace gromada::game {

inline constexpr std::array<char, 8> SaveStateMagic = {'G', 'R', 'M', 'D', 'S', 'T', 'A', 'T'};
inline constexpr std::uint32_t SaveStateVersion = 4; // v3: game-clock typewriter (issue #13); v4: zoom buttons of the input mapper (issue #19)
inline constexpr std::size_t SaveStateHeaderSize = 64;
// retro_serialize_size(): libretro wants one size for the whole session, so this is a fixed
// upper bound (see docs/savestates.md for the measured sizes); a larger state fails to save.
inline constexpr std::size_t SaveStateMaxSize = 8u << 20;

// Identity of the game data a state belongs to (fw.res: vids, sounds, weapons).
inline std::uint64_t dataFingerprint(std::span<const VidInfo> vids, std::span<const SoundData> sounds,
                                     std::span<const Weapon> weaponsAtLoad) {
    std::uint64_t h = state::fnv1a(std::string_view{"gromada-fw.res"});
    const auto mix = [&](std::uint64_t v) { h = state::fnv1a(std::span{reinterpret_cast<const std::uint8_t*>(&v), 8}, h); };
    mix(vids.size());
    mix(sounds.size());
    mix(weaponsAtLoad.size());
    for (const VidInfo& v : vids) {
        if (v.src) h = state::fnv1a(std::span{reinterpret_cast<const std::uint8_t*>(v.src->name.data()), v.src->name.size()}, h);
        mix((std::uint64_t{v.src ? static_cast<std::uint8_t>(v.src->type) : 0u} << 8) | (v.gfx ? 1u : 0u));
    }
    for (const SoundData& s : sounds) mix((std::uint64_t{s.sampleRate} << 16) | s.numChannels);
    return h;
}

// ------------------------------------------------------------------------------- staged state
namespace ss {

// Fixed part of a sprite in the state (links are slot references: 0 = none, i + 1 = pool[i]).
#pragma pack(push, 1)
struct SpriteRecord {
    std::uint32_t next, prev, child;
    std::uint8_t layer;
    std::int16_t x, y, z;
    std::uint8_t direction, action;
    std::uint16_t nvid;
    std::int16_t frame, firstFrame, lastFrame;
    std::uint16_t state;
    std::uint32_t lastTact;
    std::int16_t speed, vz;
    std::int8_t fracX, fracY, fracZ;
    std::uint8_t stepAccum;
    std::int8_t dx, dy, dz;
    std::int16_t goX, goY, goZ;
    std::uint16_t timer;
    std::int16_t attackX, attackY, attackZ;
    std::uint8_t hp;
    std::int32_t target;
    std::uint8_t army, behave;
    std::int16_t wpX, wpY;
    std::uint8_t buildTime;
    std::int8_t turn;
    std::uint16_t buildNvid;
    std::int16_t ammo[8];
    std::int32_t id;
    std::uint8_t cls;
    std::uint8_t flags;         // 1 alive, 2 onGrid, 4 has a command stack
    std::int32_t commandCount;  // CommandStack::count
    std::uint32_t commandSize;  // CommandStack::e.size()
    std::uint32_t itemCount;
};
struct CommandRecord {
    std::uint8_t act;
    std::int32_t var1, var2;
};
#pragma pack(pop)
static_assert(std::is_trivially_copyable_v<SpriteRecord>);

struct SpriteState {
    SpriteRecord rec;
    std::vector<Command> commands;
    std::vector<std::int16_t> items;
};

struct VidRecord {   // the per-vid values that change at run time (dense part)
    std::int32_t maxUnit = -1, count = 0;
    std::uint8_t weapon = 0;
    std::uint8_t paletteCache = 0;   // GameEngine::m_palettes: 0 none, 1 rebuilt from the vid, 3 raw bytes
    std::int32_t altPalettes = 0;    // index + 1 into WorldState::palettes
};

struct WorldState {
    std::vector<SpriteState> pool;
    std::vector<std::uint32_t> freeSlots, pendingFree;   // slot indices
    std::uint32_t byIdSize = 0;
    std::uint64_t live = 0;
    std::array<std::uint32_t, World::MaxLayers> layers{};
    std::uint32_t iterNext = 0, orphan = 0;
    std::int8_t orphanDx = 0, orphanDy = 0, orphanDz = 0;
    int gridShiftX = 0, gridShiftY = 0, gridRows = 0, gridCols = 0, gridRowShift = 0, gridStride = 1;
    std::vector<std::vector<std::uint32_t>> cells;   // slot references
    std::vector<std::uint32_t> gridUnits;
    std::array<Player, World::MaxPlayers> players{};
    struct GroupState {
        std::vector<std::uint32_t> members;
        int centerX = 0, centerY = 0;
    };
    std::vector<GroupState> groups;
    std::vector<SoundRequest> sounds;
    InputRecord localInput;
    bool netGame = false;
    std::uint32_t clock = 0;
    bool paused = false;
    std::uint32_t pauseStart = 0;
    std::int16_t windStrength = 0;
    std::uint8_t windDir = 0x40;
    int mapW = 0, mapH = 0, scaleX = 24, scaleY = 16;
    int camX = 0, camY = 0, viewW = 640, viewH = 480;
    int lastDist = 0;
    std::uint32_t rng = 1;
    std::uint64_t fireCount = 0;
    std::vector<VidRecord> vids;
    std::vector<std::pair<std::uint16_t, std::vector<std::uint8_t>>> modifiedVids;   // nvid, full record
    std::vector<std::array<std::array<ColorRgb8, 256>, 4>> palettes;
    std::vector<std::pair<std::uint16_t, std::array<ColorRgb8, 256>>> rawPaletteCache;
    std::vector<Weapon> weapons;
};

struct EngineState {
    WorldState world;
    MapHeaderRawData header;
    Camera camera;
    OriginalInput input;
    bool loaded = false;
    int realElapsed = 0, scrollVX = 0, scrollVY = 0;
    bool autoScroll = false, scrollLock = false;
    int gamma = 0x777;
    // ScreenEffects (Effect(), survives map loads)
    int effect = 0, effectLevel = 0;
    std::uint32_t effectLast = 0;
    int effectW = 0, effectH = 0;
    std::array<int, 4> effectFull{0, -1, 0, -1}, effectClip{0, -1, 0, -1};
    bool effectDirty = false;
    bool hasPictures = false;                 // eff_openwindow running: both pictures stored
    std::vector<RGBA8> offscreen, screen;
    bool effectsDone = true, frameLocked = false;
    std::uint32_t turretClock = 0;
    int turretSteps = 1;
    std::vector<std::array<int, 4>> lights;
    int iterUnit = -1, iterArmy = 0, iterType = 0;
    std::string commandsBuffer;
};

// Pointer -> slot reference over the sprite pool (open addressing).
class SlotIndex {
public:
    explicit SlotIndex(const std::vector<std::unique_ptr<Sprite>>& pool) {
        std::size_t cap = 16;
        while (cap < pool.size() * 2) cap <<= 1;
        m_mask = cap - 1;
        m_keys.assign(cap, nullptr);
        m_vals.assign(cap, 0);
        for (std::size_t i = 0; i < pool.size(); ++i) {
            const Sprite* p = pool[i].get();
            std::size_t h = hash(p);
            while (m_keys[h]) h = (h + 1) & m_mask;
            m_keys[h] = p;
            m_vals[h] = static_cast<std::uint32_t>(i + 1);
        }
    }
    std::uint32_t ref(const Sprite* p) const {
        if (!p) return 0;
        for (std::size_t h = hash(p);; h = (h + 1) & m_mask) {
            if (m_keys[h] == p) return m_vals[h];
            if (!m_keys[h]) throw state::Error("save state: sprite pointer outside the pool");
        }
    }

private:
    std::size_t hash(const Sprite* p) const {
        return static_cast<std::size_t>((reinterpret_cast<std::uintptr_t>(p) >> 4) * 0x9E3779B97F4A7C15ull >> 20) & m_mask;
    }
    std::size_t m_mask = 0;
    std::vector<const Sprite*> m_keys;
    std::vector<std::uint32_t> m_vals;
};

// Fixed-width record of the VidInfo values (everything but the pointers and the dense part).
inline void writeVidRecord(state::Writer& w, const VidInfo& v) {
    w.u8(v.category);
    w.u8(v.cls);
    w.u16(v.flags);
    w.u8(v.collisionMask);
    w.i16(v.sizeX); w.i16(v.sizeY); w.i16(v.sizeZ);
    w.i16(v.halfX); w.i16(v.halfY); w.i16(v.absSizeZ);
    w.u8(v.maxHP);
    w.i16(v.visibilityRadius);
    w.i16(v.speedX); w.i16(v.speedY);
    w.u16(v.acceleration);
    w.i8(v.rotationPeriod);
    w.u8(v.army);
    w.i16(v.deathDamageRadius);
    w.u8(v.deathDamage);
    w.i8(v.linkX); w.i8(v.linkY); w.i8(v.linkZ);
    w.u16(v.linkedObjectVid);
    w.u8(v.dirs);
    w.u8(v.zLayer);
    w.u8(v.dirStep);
    w.bytes(v.animLen.data(), sizeof v.animLen);
    w.bytes(v.nsfx.data(), sizeof v.nsfx);
    w.bytes(v.childOff.data(), sizeof v.childOff);
    w.bytes(v.childNvid.data(), sizeof v.childNvid);
    w.bytes(v.childCount.data(), sizeof v.childCount);
    w.u16(v.frameDuration);
    w.u16(v.numFrames); w.u16(v.width); w.u16(v.height);
}
inline void readVidRecord(state::Reader& r, VidInfo& v, std::size_t vidCount) {
    v.category = r.u8();
    v.cls = r.u8();
    v.flags = r.u16();
    v.collisionMask = r.u8();
    v.sizeX = r.i16(); v.sizeY = r.i16(); v.sizeZ = r.i16();
    v.halfX = r.i16(); v.halfY = r.i16(); v.absSizeZ = r.i16();
    v.maxHP = r.u8();
    v.visibilityRadius = r.i16();
    v.speedX = r.i16(); v.speedY = r.i16();
    v.acceleration = r.u16();
    v.rotationPeriod = r.i8();
    v.army = r.u8();
    v.deathDamageRadius = r.i16();
    v.deathDamage = r.u8();
    v.linkX = r.i8(); v.linkY = r.i8(); v.linkZ = r.i8();
    v.linkedObjectVid = r.u16();
    v.dirs = r.u8();
    v.zLayer = r.u8();
    v.dirStep = r.u8();
    r.bytes(v.animLen.data(), sizeof v.animLen);
    r.bytes(v.nsfx.data(), sizeof v.nsfx);
    r.bytes(v.childOff.data(), sizeof v.childOff);
    r.bytes(v.childNvid.data(), sizeof v.childNvid);
    r.bytes(v.childCount.data(), sizeof v.childCount);
    v.frameDuration = r.u16();
    v.numFrames = r.u16(); v.width = r.u16(); v.height = r.u16();
    if (v.dirs == 0 || v.zLayer >= World::MaxLayers || v.linkedObjectVid >= vidCount)
        throw state::Error("save state: bad vid record");
}

inline void writeInputRecord(state::Writer& w, const InputRecord& in) {
    w.i16(in.mouseWorldX); w.i16(in.mouseWorldY); w.i16(in.mouseScreenX); w.i16(in.mouseScreenY);
    w.u16(in.bits); w.u8(in.key);
}
inline InputRecord readInputRecord(state::Reader& r) {
    InputRecord in;
    in.mouseWorldX = r.i16(); in.mouseWorldY = r.i16(); in.mouseScreenX = r.i16(); in.mouseScreenY = r.i16();
    in.bits = r.u16(); in.key = r.u8();
    return in;
}
inline void writeOriginalInput(state::Writer& w, const OriginalInput& in) {
    w.i16(in.mouseWorldX); w.i16(in.mouseWorldY); w.i16(in.mouseScreenX); w.i16(in.mouseScreenY);
    w.u16(in.bits); w.u8(in.key);
}
inline OriginalInput readOriginalInput(state::Reader& r) {
    OriginalInput in;
    in.mouseWorldX = r.i16(); in.mouseWorldY = r.i16(); in.mouseScreenX = r.i16(); in.mouseScreenY = r.i16();
    in.bits = r.u16(); in.key = r.u8();
    return in;
}

} // namespace ss

// Session state staged by decode (see SaveStateAccess::decodeSession).
struct SessionStaged;

// ------------------------------------------------------------------------------- access
struct SaveStateAccess {
    static constexpr std::uint32_t EngineTag = state::fourcc("ENGN");
    static constexpr std::uint32_t SessionTag = state::fourcc("SESS");

    // ---- World ----------------------------------------------------------------------------
    static void saveWorld(state::Writer& w, const World& wd, const GameEngine* engine) {
        const ss::SlotIndex slots{wd.m_pool};
        const auto ref = [&](const Sprite* p) { return slots.ref(p); };

        w.count(wd.m_pool.size());
        for (const auto& up : wd.m_pool) {
            const Sprite& s = *up;
            ss::SpriteRecord r{};
            r.next = ref(s.next); r.prev = ref(s.prev); r.child = ref(s.child);
            r.layer = s.layer;
            r.x = s.x; r.y = s.y; r.z = s.z;
            r.direction = s.direction; r.action = s.action; r.nvid = s.nvid;
            r.frame = s.frame; r.firstFrame = s.firstFrame; r.lastFrame = s.lastFrame;
            r.state = s.state; r.lastTact = s.lastTact; r.speed = s.speed; r.vz = s.vz;
            r.fracX = s.fracX; r.fracY = s.fracY; r.fracZ = s.fracZ; r.stepAccum = s.stepAccum;
            r.dx = s.dx; r.dy = s.dy; r.dz = s.dz;
            r.goX = s.goX; r.goY = s.goY; r.goZ = s.goZ; r.timer = s.timer;
            r.attackX = s.attackX; r.attackY = s.attackY; r.attackZ = s.attackZ;
            r.hp = s.hp; r.target = s.target; r.army = s.army; r.behave = s.behave;
            r.wpX = s.wpX; r.wpY = s.wpY; r.buildTime = s.buildTime; r.turn = s.turn;
            r.buildNvid = s.buildNvid;
            for (int i = 0; i < 8; ++i) r.ammo[i] = s.ammo[static_cast<std::size_t>(i)];
            r.id = s.id; r.cls = s.cls;
            r.flags = static_cast<std::uint8_t>((s.alive ? 1 : 0) | (s.onGrid ? 2 : 0) | (s.commands ? 4 : 0));
            r.commandCount = s.commands ? s.commands->count : 0;
            r.commandSize = s.commands ? static_cast<std::uint32_t>(s.commands->e.size()) : 0;
            r.itemCount = static_cast<std::uint32_t>(s.items.size());
            w.bytes(&r, sizeof r);
            if (s.commands)
                for (const Command& c : s.commands->e) {
                    const ss::CommandRecord cr{c.act, c.var1, c.var2};
                    w.bytes(&cr, sizeof cr);
                }
            w.bytes(s.items.data(), s.items.size() * sizeof(std::int16_t));
        }
        const auto refs = [&](const std::vector<Sprite*>& v) {
            w.count(v.size());
            for (const Sprite* p : v) w.u32(ref(p));
        };
        refs(wd.m_free);
        refs(wd.m_pendingFree);
        w.count(wd.m_byId.size());
        w.u64(wd.m_live);
        for (const Sprite* h : wd.m_layers) w.u32(ref(h));
        w.u32(ref(wd.m_iterNext));
        w.u32(ref(wd.m_orphan));
        w.i8(wd.m_orphanDx); w.i8(wd.m_orphanDy); w.i8(wd.m_orphanDz);

        w.i32(wd.m_gridShiftX); w.i32(wd.m_gridShiftY); w.i32(wd.m_gridRows); w.i32(wd.m_gridCols);
        w.i32(wd.m_gridRowShift); w.i32(wd.m_gridStride);
        w.count(wd.m_cells.size());
        for (const auto& cell : wd.m_cells) refs(cell);
        refs(wd.m_gridUnits);

        for (const Player& p : wd.m_players) {
            w.u32(p.type); w.u32(p.b); w.u32(p.netIndex);
            w.i32(p.flagman); w.i32(p.underCursor);
            ss::writeInputRecord(w, p.input);
            w.u8(p.weapon);
        }
        w.count(wd.m_groups.size());
        for (const Group& g : wd.m_groups) {
            refs(g.members);
            w.i32(g.centerX); w.i32(g.centerY);
        }
        w.count(wd.m_sounds.size());
        for (const SoundRequest& s : wd.m_sounds) { w.i32(s.nsfx); w.i32(s.x); w.i32(s.y); }
        ss::writeInputRecord(w, wd.m_localInput);
        w.boolean(wd.m_netGame);

        w.u32(wd.m_clock);
        w.boolean(wd.m_paused);
        w.u32(wd.m_pauseStart);
        w.i16(wd.m_windStrength);
        w.u8(wd.m_windDir);
        w.i32(wd.m_mapW); w.i32(wd.m_mapH); w.i32(wd.m_scaleX); w.i32(wd.m_scaleY);
        w.i32(wd.m_camX); w.i32(wd.m_camY); w.i32(wd.m_viewW); w.i32(wd.m_viewH);
        w.i32(wd.m_lastDist);
        w.u32(wd.m_rand.state);
        w.u64(wd.fireCount);

        // vids: dense values, palette sets shared by graphics, records that differ from fw.res
        std::vector<const std::array<std::array<ColorRgb8, 256>, 4>*> palettes;
        w.count(wd.m_vids.size());
        for (std::size_t i = 0; i < wd.m_vids.size(); ++i) {
            const VidInfo& v = wd.m_vids[i];
            w.i32(v.maxUnit);
            w.i32(v.count);
            w.u8(v.weapon);
            w.u8(paletteCacheMode(engine, v, i));
            std::int32_t pal = 0;
            if (v.altPalettes) {
                const auto* p = v.altPalettes.get();
                auto it = std::ranges::find(palettes, p);
                if (it == palettes.end()) { palettes.push_back(p); it = palettes.end() - 1; }
                pal = static_cast<std::int32_t>(it - palettes.begin()) + 1;
            }
            w.i32(pal);
        }
        w.count(palettes.size());
        for (const auto* p : palettes)
            for (const auto& pal : *p) w.bytes(pal.data(), pal.size() * sizeof(ColorRgb8));
        std::size_t rawAt = w.size();
        w.u32(0);
        std::uint32_t raw = 0;
        for (std::size_t i = 0; i < wd.m_vids.size(); ++i)
            if (paletteCacheMode(engine, wd.m_vids[i], i) == 3) {
                w.u16(static_cast<std::uint16_t>(i));
                w.bytes(engine->m_palettes[i]->data(), 256 * sizeof(ColorRgb8));
                ++raw;
            }
        w.patch32(rawAt, raw);
        std::vector<std::uint8_t> curBuf, refBuf;
        std::size_t modifiedAt = w.size();
        w.u32(0);
        std::uint32_t modified = 0;
        for (std::size_t i = 0; i < wd.m_vids.size(); ++i) {
            const VidInfo& v = wd.m_vids[i];
            if (!v.src) continue;
            VidInfo pristine;
            World::initVidInfo(pristine, *v.src);
            state::Writer a{curBuf}, b{refBuf};
            ss::writeVidRecord(a, v);
            ss::writeVidRecord(b, pristine);
            if (a.size() == b.size() && std::ranges::equal(a.result(), b.result())) continue;
            w.u16(static_cast<std::uint16_t>(i));
            w.bytes(a.result().data(), a.size());
            ++modified;
        }
        w.patch32(modifiedAt, modified);
        w.count(wd.m_weapons.size());
        for (const Weapon& wp : wd.m_weapons) {
            w.u8(static_cast<std::uint8_t>(wp.targetCategory)); w.u8(wp.flags); w.u16(wp.alwaysZero);
            w.u16(wp.weaponRange); w.u8(wp.scatter); w.u16(wp.cooldown);
        }
    }

    static ss::WorldState decodeWorld(state::Reader& r, const World& wd) {
        ss::WorldState s;
        const std::size_t vidCount = wd.m_vids.size();
        const std::size_t n = r.count(sizeof(ss::SpriteRecord), 1u << 22);
        s.pool.resize(n);
        const auto checkRef = [&](std::uint32_t v) {
            if (v > n) throw state::Error("save state: sprite reference out of range");
            return v;
        };
        for (auto& sp : s.pool) {
            r.bytes(&sp.rec, sizeof sp.rec);
            const auto& rec = sp.rec;
            checkRef(rec.next); checkRef(rec.prev); checkRef(rec.child);
            if (rec.nvid >= vidCount) throw state::Error("save state: sprite vid out of range");
            if (rec.layer >= World::MaxLayers && rec.layer != 0xff) throw state::Error("save state: bad sprite layer");
            if (rec.cls > cls::RepairCannon) throw state::Error("save state: bad sprite class");
            if (rec.flags > 7) throw state::Error("save state: bad sprite flags");
            if (!(rec.flags & 4) && (rec.commandCount || rec.commandSize)) throw state::Error("save state: bad command stack");
            if (rec.commandSize > 0xFFFF || rec.commandCount > static_cast<std::int32_t>(rec.commandSize) || rec.commandCount < -0x8000)
                throw state::Error("save state: bad command stack");
            if (rec.itemCount > 0xFFFF) throw state::Error("save state: too many items");
            if (rec.commandSize > r.remaining() / sizeof(ss::CommandRecord)) throw state::Error("save state: truncated (commands)");
            sp.commands.resize(rec.commandSize);
            for (Command& c : sp.commands) {
                ss::CommandRecord cr;
                r.bytes(&cr, sizeof cr);
                c = Command{cr.act, cr.var1, cr.var2};
            }
            if (rec.itemCount > r.remaining() / 2) throw state::Error("save state: truncated (items)");
            sp.items.resize(rec.itemCount);
            r.bytes(sp.items.data(), sp.items.size() * 2);
        }
        const auto refs = [&](std::vector<std::uint32_t>& out, bool allowNull) {
            const std::size_t k = r.count(4, 1u << 24);
            out.resize(k);
            for (auto& v : out) {
                v = checkRef(r.u32());
                if (!v && !allowNull) throw state::Error("save state: null sprite reference");
            }
        };
        refs(s.freeSlots, false);
        refs(s.pendingFree, false);
        s.byIdSize = r.u32();
        s.live = r.u64();
        for (auto& h : s.layers) h = checkRef(r.u32());
        s.iterNext = checkRef(r.u32());
        s.orphan = checkRef(r.u32());
        s.orphanDx = r.i8(); s.orphanDy = r.i8(); s.orphanDz = r.i8();

        s.gridShiftX = r.ranged<std::int32_t>(0, 24, "grid shift");
        s.gridShiftY = r.ranged<std::int32_t>(0, 24, "grid shift");
        s.gridRows = r.ranged<std::int32_t>(0, 1 << 16, "grid rows");
        s.gridCols = r.ranged<std::int32_t>(0, 1 << 16, "grid columns");
        s.gridRowShift = r.ranged<std::int32_t>(0, 16, "grid row shift");
        s.gridStride = r.i32();
        if (s.gridStride != (1 << s.gridRowShift) || s.gridCols > s.gridStride) throw state::Error("save state: bad grid geometry");
        const std::size_t nCells = r.count(4, 1u << 24);
        if (nCells != static_cast<std::size_t>(s.gridRows) * static_cast<std::size_t>(s.gridStride))
            throw state::Error("save state: bad grid size");
        s.cells.resize(nCells);
        for (auto& c : s.cells) refs(c, false);
        refs(s.gridUnits, false);

        for (Player& p : s.players) {
            p.type = r.u32(); p.b = r.u32(); p.netIndex = r.u32();
            p.flagman = r.i32(); p.underCursor = r.i32();
            p.input = ss::readInputRecord(r);
            p.weapon = r.u8();
        }
        const std::size_t nGroups = r.count(12, 1u << 20);
        s.groups.resize(nGroups);
        for (auto& g : s.groups) {
            refs(g.members, false);
            g.centerX = r.i32(); g.centerY = r.i32();
        }
        const std::size_t nSounds = r.count(12, 1u << 20);
        s.sounds.resize(nSounds);
        for (auto& snd : s.sounds) { snd.nsfx = r.i32(); snd.x = r.i32(); snd.y = r.i32(); }
        s.localInput = ss::readInputRecord(r);
        s.netGame = r.boolean();

        s.clock = r.u32();
        s.paused = r.boolean();
        s.pauseStart = r.u32();
        s.windStrength = r.i16();
        s.windDir = r.u8();
        s.mapW = r.ranged<std::int32_t>(0, 1 << 20, "map width");
        s.mapH = r.ranged<std::int32_t>(0, 1 << 20, "map height");
        s.scaleX = r.ranged<std::int32_t>(0, 1 << 16, "map scale");
        s.scaleY = r.ranged<std::int32_t>(0, 1 << 16, "map scale");
        s.camX = r.i32(); s.camY = r.i32(); s.viewW = r.i32(); s.viewH = r.i32();
        s.lastDist = r.i32();
        s.rng = r.u32();
        s.fireCount = r.u64();

        if (r.count(14, 1u << 16) != vidCount) throw state::Error("save state: vid count differs");
        s.vids.resize(vidCount);
        for (auto& v : s.vids) {
            v.maxUnit = r.i32();
            v.count = r.i32();
            v.weapon = r.u8();
            v.paletteCache = r.ranged<std::uint8_t>(0, 3, "palette cache");
            if (v.paletteCache == 2) throw state::Error("save state: bad palette cache mode");
            v.altPalettes = r.i32();
        }
        const std::size_t nPal = r.count(4 * 256 * sizeof(ColorRgb8), 1u << 16);
        s.palettes.resize(nPal);
        for (auto& set : s.palettes)
            for (auto& pal : set) r.bytes(pal.data(), pal.size() * sizeof(ColorRgb8));
        for (const auto& v : s.vids)
            if (v.altPalettes < 0 || static_cast<std::size_t>(v.altPalettes) > nPal) throw state::Error("save state: bad palette reference");
        const std::size_t nRaw = r.count(2 + 256 * sizeof(ColorRgb8), vidCount);
        for (std::size_t i = 0; i < nRaw; ++i) {
            const std::uint16_t nvid = r.u16();
            if (nvid >= vidCount || s.vids[nvid].paletteCache != 3) throw state::Error("save state: bad palette cache entry");
            std::array<ColorRgb8, 256> pal;
            r.bytes(pal.data(), sizeof pal);
            s.rawPaletteCache.emplace_back(nvid, pal);
        }
        std::size_t wantRaw = 0;
        for (const auto& v : s.vids) wantRaw += v.paletteCache == 3 ? 1 : 0;
        if (wantRaw != nRaw) throw state::Error("save state: palette cache entries missing");
        const std::size_t nModified = r.count(2, vidCount);
        for (std::size_t i = 0; i < nModified; ++i) {
            const std::uint16_t nvid = r.u16();
            if (nvid >= vidCount || !wd.m_vids[nvid].src) throw state::Error("save state: modified vid out of range");
            const std::size_t at = r.position();
            VidInfo probe;
            ss::readVidRecord(r, probe, vidCount);
            (void)at;
            // re-encode to keep the exact record bytes
            std::vector<std::uint8_t> bytes;
            state::Writer bw{bytes};
            ss::writeVidRecord(bw, probe);
            s.modifiedVids.emplace_back(nvid, std::vector<std::uint8_t>{bw.result().begin(), bw.result().end()});
        }
        const std::size_t nWeapons = r.count(9, 1u << 16);
        if (nWeapons != wd.m_weapons.size()) throw state::Error("save state: weapon count differs");
        s.weapons.resize(nWeapons);
        for (Weapon& wp : s.weapons) {
            wp.targetCategory = static_cast<ObjectCategory>(r.u8()); wp.flags = r.u8(); wp.alwaysZero = r.u16();
            wp.weaponRange = r.u16(); wp.scatter = r.u8(); wp.cooldown = r.u16();
        }
        validateWorld(s);
        return s;
    }

    // Structural invariants the engine relies on: lists, chains, ids, grid entries.
    static void validateWorld(const ss::WorldState& s) {
        const std::size_t n = s.pool.size();
        const auto at = [&](std::uint32_t ref) -> const ss::SpriteRecord& { return s.pool[ref - 1].rec; };
        // ids: alive sprites have distinct ids in 1..byIdSize; their count is m_live
        std::vector<std::uint8_t> seenId(s.byIdSize + 1, 0);
        std::uint64_t alive = 0;
        for (const auto& sp : s.pool) {
            if (!(sp.rec.flags & 1)) continue;
            ++alive;
            if (sp.rec.id <= 0 || static_cast<std::uint32_t>(sp.rec.id) > s.byIdSize || seenId[static_cast<std::size_t>(sp.rec.id)]++)
                throw state::Error("save state: bad sprite id");
        }
        if (alive != s.live) throw state::Error("save state: live count differs");
        // z-layer lists: each sprite in at most one list, with consistent links and layer
        std::vector<std::uint8_t> inList(n + 1, 0);
        for (int l = 0; l < World::MaxLayers; ++l) {
            std::uint32_t prev = 0;
            for (std::uint32_t cur = s.layers[static_cast<std::size_t>(l)]; cur; cur = at(cur).next) {
                if (inList[cur]++) throw state::Error("save state: sprite list cycle");
                if (at(cur).layer != l || at(cur).prev != prev) throw state::Error("save state: inconsistent sprite list");
                prev = cur;
            }
        }
        for (std::uint32_t i = 1; i <= n; ++i)
            if (at(i).layer != 0xff && !inList[i]) throw state::Error("save state: sprite outside its list");
        // sub-object chains: no cycles (functional graph colouring)
        std::vector<std::uint8_t> colour(n + 1, 0);
        for (std::uint32_t i = 1; i <= n; ++i) {
            std::uint32_t cur = i;
            while (cur && colour[cur] == 0) { colour[cur] = 1; cur = at(cur).child; }
            if (cur && colour[cur] == 1) throw state::Error("save state: sub-object chain cycle");
            for (cur = i; cur && colour[cur] == 1; cur = at(cur).child) colour[cur] = 2;
        }
        // free lists hold dead slots, once
        std::vector<std::uint8_t> freed(n + 1, 0);
        for (const auto* list : {&s.freeSlots, &s.pendingFree})
            for (const std::uint32_t v : *list)
                if ((at(v).flags & 1) || freed[v]++) throw state::Error("save state: bad free list");
    }

    static void applyWorld(World& wd, ss::WorldState&& s) {
        const std::size_t n = s.pool.size();
        if (wd.m_pool.size() > n) wd.m_pool.resize(n);
        while (wd.m_pool.size() < n) wd.m_pool.push_back(std::make_unique<Sprite>());
        const auto ptr = [&](std::uint32_t ref) -> Sprite* { return ref ? wd.m_pool[ref - 1].get() : nullptr; };
        for (std::size_t i = 0; i < n; ++i) {
            Sprite& d = *wd.m_pool[i];
            const ss::SpriteRecord& r = s.pool[i].rec;
            d.next = ptr(r.next); d.prev = ptr(r.prev); d.child = ptr(r.child);
            d.layer = r.layer;
            d.x = r.x; d.y = r.y; d.z = r.z;
            d.direction = r.direction; d.action = r.action; d.nvid = r.nvid;
            d.frame = r.frame; d.firstFrame = r.firstFrame; d.lastFrame = r.lastFrame;
            d.state = r.state; d.lastTact = r.lastTact; d.speed = r.speed; d.vz = r.vz;
            d.fracX = r.fracX; d.fracY = r.fracY; d.fracZ = r.fracZ; d.stepAccum = r.stepAccum;
            d.dx = r.dx; d.dy = r.dy; d.dz = r.dz;
            d.goX = r.goX; d.goY = r.goY; d.goZ = r.goZ; d.timer = r.timer;
            d.attackX = r.attackX; d.attackY = r.attackY; d.attackZ = r.attackZ;
            if (r.flags & 4) {
                if (!d.commands) d.commands = std::make_unique<CommandStack>();
                d.commands->e = std::move(s.pool[i].commands);
                d.commands->count = r.commandCount;
            } else {
                d.commands.reset();
            }
            d.hp = r.hp; d.target = r.target; d.army = r.army; d.behave = r.behave;
            d.wpX = r.wpX; d.wpY = r.wpY; d.buildTime = r.buildTime; d.turn = r.turn;
            d.items = std::move(s.pool[i].items);
            d.buildNvid = r.buildNvid;
            for (int k = 0; k < 8; ++k) d.ammo[static_cast<std::size_t>(k)] = r.ammo[k];
            d.id = r.id; d.cls = r.cls;
            d.alive = (r.flags & 1) != 0;
            d.onGrid = (r.flags & 2) != 0;
        }
        const auto ptrs = [&](const std::vector<std::uint32_t>& v) {
            std::vector<Sprite*> out;
            out.reserve(v.size());
            for (const auto ref : v) out.push_back(ptr(ref));
            return out;
        };
        wd.m_free = ptrs(s.freeSlots);
        wd.m_pendingFree = ptrs(s.pendingFree);
        wd.m_byId.assign(s.byIdSize, nullptr);
        for (const auto& up : wd.m_pool)
            if (up->alive) wd.m_byId[static_cast<std::size_t>(up->id) - 1] = up.get();
        wd.m_live = static_cast<std::size_t>(s.live);
        for (int l = 0; l < World::MaxLayers; ++l) wd.m_layers[static_cast<std::size_t>(l)] = ptr(s.layers[static_cast<std::size_t>(l)]);
        wd.m_iterNext = ptr(s.iterNext);
        wd.m_orphan = ptr(s.orphan);
        wd.m_orphanDx = s.orphanDx; wd.m_orphanDy = s.orphanDy; wd.m_orphanDz = s.orphanDz;
        wd.m_gridShiftX = s.gridShiftX; wd.m_gridShiftY = s.gridShiftY; wd.m_gridRows = s.gridRows;
        wd.m_gridCols = s.gridCols; wd.m_gridRowShift = s.gridRowShift; wd.m_gridStride = s.gridStride;
        wd.m_cells.resize(s.cells.size());
        for (std::size_t i = 0; i < s.cells.size(); ++i) {
            auto& cell = wd.m_cells[i];
            cell.clear();
            for (const auto ref : s.cells[i]) cell.push_back(ptr(ref));
        }
        wd.m_gridUnits = ptrs(s.gridUnits);
        wd.m_players = s.players;
        wd.m_groups.clear();
        for (const auto& g : s.groups) wd.m_groups.push_back(Group{ptrs(g.members), g.centerX, g.centerY});
        wd.m_sounds = std::move(s.sounds);
        wd.m_localInput = s.localInput;
        wd.m_netGame = s.netGame;
        wd.m_clock = s.clock;
        wd.m_paused = s.paused;
        wd.m_pauseStart = s.pauseStart;
        wd.m_windStrength = s.windStrength;
        wd.m_windDir = s.windDir;
        wd.m_mapW = s.mapW; wd.m_mapH = s.mapH; wd.m_scaleX = s.scaleX; wd.m_scaleY = s.scaleY;
        wd.m_persp.build(s.scaleX, s.scaleY);
        wd.m_camX = s.camX; wd.m_camY = s.camY; wd.m_viewW = s.viewW; wd.m_viewH = s.viewH;
        wd.m_lastDist = s.lastDist;
        wd.m_rand.state = s.rng;
        wd.fireCount = s.fireCount;

        std::vector<std::shared_ptr<std::array<std::array<ColorRgb8, 256>, 4>>> palettes;
        for (auto& p : s.palettes) palettes.push_back(std::make_shared<std::array<std::array<ColorRgb8, 256>, 4>>(p));
        for (std::size_t i = 0; i < wd.m_vids.size(); ++i) {
            VidInfo& v = wd.m_vids[i];
            if (v.src) World::initVidInfo(v, *v.src);
            v.maxUnit = s.vids[i].maxUnit;
            v.count = s.vids[i].count;
            v.weapon = s.vids[i].weapon;
            v.altPalettes = s.vids[i].altPalettes ? palettes[static_cast<std::size_t>(s.vids[i].altPalettes - 1)] : nullptr;
        }
        for (const auto& [nvid, bytes] : s.modifiedVids) {
            VidInfo& v = wd.m_vids[nvid];
            const int maxUnit = v.maxUnit, count = v.count;
            const std::uint8_t weapon = v.weapon;
            state::Reader rr{bytes};
            ss::readVidRecord(rr, v, wd.m_vids.size());
            v.maxUnit = maxUnit; v.count = count; v.weapon = weapon;   // dense values win
        }
        wd.m_weapons = std::move(s.weapons);
    }

    // ---- GameEngine -----------------------------------------------------------------------
    static void saveEngine(state::Writer& w, const GameEngine& e) {
        const std::size_t chunk = w.beginChunk(EngineTag);
        saveWorld(w, e.m_world, &e);
        const MapHeaderRawData& h = e.m_header;
        w.u32(h.width); w.u32(h.height); w.i16(h.observerX); w.i16(h.observerY);
        w.u32(h.scaleX); w.u32(h.scaleY); w.u32(h.startTimer); w.u32(static_cast<std::uint32_t>(h.mapVersion));
        w.i32(e.m_camera.x); w.i32(e.m_camera.y); w.i32(e.m_camera.width); w.i32(e.m_camera.height);
        ss::writeOriginalInput(w, e.m_input);
        w.boolean(e.m_loaded);
        w.i32(e.m_realElapsed);
        w.i32(e.m_scrollVX); w.i32(e.m_scrollVY);
        w.boolean(e.m_autoScroll); w.boolean(e.m_scrollLock);
        w.i32(e.m_gamma);
        const ScreenEffects& fx = e.m_effects;
        w.i32(fx.m_effect); w.i32(fx.m_level); w.u32(fx.m_last); w.i32(fx.m_w); w.i32(fx.m_h);
        for (const auto* rc : {&fx.m_full, &fx.m_clip}) { w.i32(rc->left); w.i32(rc->right); w.i32(rc->top); w.i32(rc->bottom); }
        w.boolean(fx.m_dirty);
        // the pictures matter only while eff_openwindow composes the old screen with the new one
        const bool pictures = fx.m_effect == 6;
        w.boolean(pictures);
        if (pictures) {
            w.count(fx.m_offscreen.size());
            w.bytes(fx.m_offscreen.data(), fx.m_offscreen.size() * sizeof(RGBA8));
            w.count(fx.m_screen.size());
            w.bytes(fx.m_screen.data(), fx.m_screen.size() * sizeof(RGBA8));
        }
        w.boolean(e.m_effectsDone); w.boolean(e.m_frameLocked);
        w.u32(e.m_turretClock); w.i32(e.m_turretSteps);
        w.count(e.m_lights.size());
        for (const auto& l : e.m_lights) { w.i32(l.x); w.i32(l.y); w.i32(l.z); w.i32(l.color); }
        w.i32(e.m_iterUnit); w.i32(e.m_iterArmy); w.i32(e.m_iterType);
        w.str(e.m_commandsBuffer);
        w.endChunk(chunk);
    }

    static ss::EngineState decodeEngine(state::Reader& outer, const GameEngine& e) {
        state::Reader r = outer.chunk(EngineTag);
        ss::EngineState s;
        s.world = decodeWorld(r, e.m_world);
        MapHeaderRawData& h = s.header;
        h.width = r.u32(); h.height = r.u32(); h.observerX = r.i16(); h.observerY = r.i16();
        h.scaleX = r.u32(); h.scaleY = r.u32(); h.startTimer = r.u32();
        h.mapVersion = static_cast<MapVersion>(r.ranged<std::uint32_t>(0, 3, "map version"));
        s.camera.x = r.i32(); s.camera.y = r.i32();
        s.camera.width = r.ranged<std::int32_t>(1, 1 << 14, "view width");
        s.camera.height = r.ranged<std::int32_t>(1, 1 << 14, "view height");
        s.input = ss::readOriginalInput(r);
        s.loaded = r.boolean();
        s.realElapsed = r.i32();
        s.scrollVX = r.i32(); s.scrollVY = r.i32();
        s.autoScroll = r.boolean(); s.scrollLock = r.boolean();
        s.gamma = r.i32();
        s.effect = r.i32(); s.effectLevel = r.i32(); s.effectLast = r.u32();
        s.effectW = r.ranged<std::int32_t>(0, 4096, "effect width");
        s.effectH = r.ranged<std::int32_t>(0, 4096, "effect height");
        for (auto* rc : {&s.effectFull, &s.effectClip})
            for (int& v : *rc) v = r.i32();
        s.effectDirty = r.boolean();
        const std::size_t pixels = static_cast<std::size_t>(s.effectW) * static_cast<std::size_t>(s.effectH);
        s.hasPictures = r.boolean();
        if (s.hasPictures) {
            for (auto* pic : {&s.offscreen, &s.screen}) {
                if (r.count(sizeof(RGBA8), 1u << 24) != pixels) throw state::Error("save state: bad effect picture");
                pic->resize(pixels);
                r.bytes(pic->data(), pixels * sizeof(RGBA8));
            }
        }
        // the window rectangle indexes the pictures: it must stay inside them
        const auto inside = [&](const std::array<int, 4>& rc) {
            return rc[1] < rc[0] || rc[3] < rc[2] || (rc[0] >= 0 && rc[2] >= 0 && rc[1] < s.effectW && rc[3] < s.effectH);
        };
        if (!inside(s.effectFull) || !inside(s.effectClip)) throw state::Error("save state: bad effect rectangle");
        s.effectsDone = r.boolean(); s.frameLocked = r.boolean();
        s.turretClock = r.u32(); s.turretSteps = r.ranged<std::int32_t>(0, 64, "turret steps");
        const std::size_t nLights = r.count(16, 1u << 20);
        s.lights.resize(nLights);
        for (auto& l : s.lights) for (int& v : l) v = r.i32();
        s.iterUnit = r.i32(); s.iterArmy = r.i32(); s.iterType = r.i32();
        s.commandsBuffer = r.str(1u << 22);
        r.expectEnd("engine");
        return s;
    }

    // A palette of GameEngine's cache as paletteFor() builds it now.
    static std::array<ColorRgb8, 256> builtPalette(const GameEngine& e, std::size_t nvid) {
        const VidInfo& v = e.m_world.m_vids[nvid];
        return hicolor::expand(hicolor::palette16(v.gfx->palette, e.m_gamma,
                                                  (e.graphicsOwner(static_cast<int>(nvid)).flags & vf::IgnoreGlobalGamma) != 0));
    }
    // How a cached palette (built on first use) is restored: built again (1) when that gives
    // the same colours, else from its bytes (3, e.g. the vid's flags changed since).
    static std::uint8_t paletteCacheMode(const GameEngine* e, const VidInfo& v, std::size_t i) {
        if (!e || i >= e->m_palettes.size() || !e->m_palettes[i]) return 0;
        if (!v.gfx) return 3;
        const auto built = builtPalette(*e, i);
        return std::memcmp(built.data(), e->m_palettes[i]->data(), sizeof built) == 0 ? 1 : 3;
    }

    static void applyEngine(GameEngine& e, ss::EngineState&& s) {
        // palette cache modes are in the per-vid records of the world state
        std::vector<std::uint8_t> cache(s.world.vids.size());
        for (std::size_t i = 0; i < cache.size(); ++i) cache[i] = s.world.vids[i].paletteCache;
        auto raw = std::move(s.world.rawPaletteCache);
        applyWorld(e.m_world, std::move(s.world));
        e.m_header = s.header;
        e.m_camera = s.camera;
        e.m_input = s.input;
        e.m_loaded = s.loaded;
        e.m_realElapsed = s.realElapsed;
        e.m_scrollVX = s.scrollVX; e.m_scrollVY = s.scrollVY;
        e.m_autoScroll = s.autoScroll; e.m_scrollLock = s.scrollLock;
        e.m_gamma = s.gamma;
        ScreenEffects& fx = e.m_effects;
        fx.m_effect = s.effect; fx.m_level = s.effectLevel; fx.m_last = s.effectLast;
        fx.m_w = s.effectW; fx.m_h = s.effectH;
        fx.m_full = {s.effectFull[0], s.effectFull[1], s.effectFull[2], s.effectFull[3]};
        fx.m_clip = {s.effectClip[0], s.effectClip[1], s.effectClip[2], s.effectClip[3]};
        fx.m_dirty = s.effectDirty;
        const std::size_t pixels = static_cast<std::size_t>(s.effectW) * static_cast<std::size_t>(s.effectH);
        if (s.hasPictures) {
            fx.m_offscreen = std::move(s.offscreen);
            fx.m_screen = std::move(s.screen);
        } else if (fx.m_offscreen.size() != pixels || fx.m_screen.size() != pixels) {
            // not stored: the surface is redrawn whole at the next frame (no window effect)
            fx.m_offscreen.assign(pixels, RGBA8{{0, 0, 0}, 255});
            fx.m_screen = fx.m_offscreen;
        }
        e.m_effectsDone = s.effectsDone; e.m_frameLocked = s.frameLocked;
        e.m_turretClock = s.turretClock; e.m_turretSteps = s.turretSteps;
        e.m_lights.clear();
        for (const auto& l : s.lights) e.m_lights.push_back({l[0], l[1], l[2], l[3]});
        e.m_iterUnit = s.iterUnit; e.m_iterArmy = s.iterArmy; e.m_iterType = s.iterType;
        e.m_commandsBuffer = std::move(s.commandsBuffer);
        // The palette cache, as it was (a vid whose flags changed since keeps its palette).
        e.m_palettes.assign(e.m_world.vids().size(), std::nullopt);
        for (std::size_t i = 0; i < cache.size() && i < e.m_palettes.size(); ++i)
            if (cache[i] == 1 && e.m_world.vid(static_cast<int>(i)).gfx) e.m_palettes[i] = builtPalette(e, i);
        for (const auto& [nvid, pal] : raw)
            if (nvid < e.m_palettes.size()) e.m_palettes[nvid] = pal;
    }

    // ---- Session ----------------------------------------------------------------------------
    static void saveSession(state::Writer& w, const Session& s);
    static SessionStaged decodeSession(state::Reader& outer, Session& s, class SaveStateManager& mgr);
    static void applySession(Session& s, SessionStaged&& st);
    static std::int32_t soundRef(const Session& s, const SoundData* p);
    // helpers for SaveStateManager
    static script::Script compileScript(Session& s, const std::string& ini);
    static std::shared_ptr<const demo::DemoFile> readDemoFile(Session& s, const std::string& name, bool& current);
    static std::uint64_t fingerprint(const Session& s, const GameEngine& e) {
        return dataFingerprint(e.m_world.m_vids, s.m_sounds, e.m_world.m_weapons);
    }
    static std::uint64_t frames(const Session& s) { return s.m_frames; }
    static void tripwire();   // size checks of private types (savestate_session.hpp)
    static std::optional<double> moviePosition(const Session& s) {
        return s.m_movies ? s.m_movies->position() : std::nullopt;
    }
};

} // namespace gromada::game

#include "game/savestate_session.hpp"
