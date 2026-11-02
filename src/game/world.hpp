#pragma once
// The simulated world of gromada.exe: sprites, z-layer lists, spatial grid, per-sprite tick,
// physics, command stacks, collisions, children, players and squads.
// Faithful port of docs/re/engine.md; every function cites the original address.
// The per-class Action() functions live in world_actions.hpp. Header-only.

#include "core/std.hpp"
#include <functional>
#include "game/fixed_math.hpp"
#include "game/rng.hpp"
#include "game/sprite.hpp"
#include "libgromada/game_resources.hpp"
#include "libgromada/map.hpp"

namespace gromada::game {

struct SoundRequest {
    int nsfx = 0;
    int x = 0, y = 0;   // world coordinates
};

// The 12-byte input record of a player (docs/re/runtime.md §6).
struct InputRecord {
    std::int16_t mouseWorldX = 0, mouseWorldY = 0;
    std::int16_t mouseScreenX = 0, mouseScreenY = 0;
    std::uint16_t bits = 0;
    std::uint8_t key = 0;
};

// Player record (0x255 bytes at 0x439d78 + i*0x255, docs/re/engine.md §8.5).
struct Player {
    std::uint32_t type = 0;        // +0: 1 = human (receives input)
    std::uint32_t b = 0;
    std::uint32_t netIndex = 0;    // +8
    UnitId flagman = 0;            // +0x224 (an id: survives the sprite like a weak pointer)
    UnitId underCursor = 0;        // +0x228
    InputRecord input;             // +0x230
    std::uint8_t weapon = 0;       // +0x244: weapon index written into the flagman's vid
};

// Squad (25 bytes, list at 0x43f8e8).
struct Group {
    std::vector<Sprite*> members;
    int centerX = 0, centerY = 0;
};

class World {
public:
    friend struct SaveStateAccess; // save states (game/savestate.hpp)
    static constexpr int MaxLayers = 16;
    static constexpr int MaxPlayers = 4;
    // Where the original sometimes uses a sprite pointer as an x coordinate (D15): a typical
    // heap address of the original's sprites.
    static constexpr int kPointerX = 0x00800000;

    explicit World(const GameResources& resources) : World(resources.vids(), resources.weapons()) {}
    // Vids and weapons only (synthetic tests): vids without graphics tick every 71 ms.
    World(std::span<const Vid> vids, std::span<const Weapon> weapons) {
        m_vids.resize(vids.size());
        for (std::size_t i = 0; i < vids.size(); ++i) initVidInfo(m_vids[i], vids[i]);
        m_weapons.assign(weapons.begin(), weapons.end());
        m_persp.build(24, 16);
    }

    World(const World&) = delete;
    World& operator=(const World&) = delete;
    ~World() { clear(); }

    // ------------------------------------------------------------------ accessors
    [[nodiscard]] std::span<VidInfo> vids() noexcept { return m_vids; }
    [[nodiscard]] std::span<const VidInfo> vids() const noexcept { return m_vids; }
    [[nodiscard]] VidInfo& vid(int nvid) { return m_vids[static_cast<std::size_t>(nvid)]; }
    [[nodiscard]] const VidInfo& vid(int nvid) const { return m_vids[static_cast<std::size_t>(nvid)]; }
    [[nodiscard]] VidInfo& vidOf(const Sprite& s) { return m_vids[s.nvid]; }
    [[nodiscard]] const VidInfo& vidOf(const Sprite& s) const { return m_vids[s.nvid]; }
    [[nodiscard]] std::span<Weapon> weapons() noexcept { return m_weapons; }
    [[nodiscard]] const Weapon& weaponOf(const Sprite& s) const {
        static const Weapon none{};
        const auto w = vidOf(s).weapon;
        return w < m_weapons.size() ? m_weapons[w] : none;
    }
    [[nodiscard]] std::uint32_t clock() const noexcept { return m_clock; }
    void setClock(std::uint32_t c) noexcept { m_clock = c; }
    [[nodiscard]] bool paused() const noexcept { return m_paused; }
    [[nodiscard]] int mapWidth() const noexcept { return m_mapW; }
    [[nodiscard]] int mapHeight() const noexcept { return m_mapH; }
    [[nodiscard]] int scaleX() const noexcept { return m_scaleX; }
    [[nodiscard]] int scaleY() const noexcept { return m_scaleY; }
    MsvcRand& rng() noexcept { return m_rand; }
    int rand() noexcept { return m_rand.next(); }
    [[nodiscard]] Sprite* layerHead(int layer) const noexcept { return m_layers[layer]; }
    [[nodiscard]] Sprite* byId(UnitId id) const noexcept {
        if (id <= 0 || static_cast<std::size_t>(id) > m_byId.size()) return nullptr;
        return m_byId[static_cast<std::size_t>(id) - 1];
    }
    static UnitId idOf(const Sprite* s) noexcept { return s ? s->id : 0; }
    [[nodiscard]] std::size_t liveCount() const noexcept { return m_live; }
    std::array<Player, MaxPlayers>& players() noexcept { return m_players; }
    std::vector<Group>& groups() noexcept { return m_groups; }
    [[nodiscard]] const std::vector<Sprite*>& gridUnits() const noexcept { return m_gridUnits; }
    std::vector<SoundRequest>& sounds() noexcept { return m_sounds; }
    void setWind(int strength, int dir) noexcept { m_windStrength = static_cast<std::int16_t>(strength); m_windDir = static_cast<std::uint8_t>(dir); }
    void setCamera(int x, int y, int w, int h) noexcept { m_camX = x; m_camY = y; m_viewW = w; m_viewH = h; }
    InputRecord& localInput() noexcept { return m_localInput; }
    // Diagnostics for tools and tests: called for every act_damage (target, amount, source).
    std::function<void(const Sprite&, int, int)> damageHook;
    // Test seams (tests/engine_oracle_test.cpp), unset in the game: like the recorders of the
    // differential oracle (docs/oracle.md) they see every virtual Action / ChangeAction call and
    // intercept it by returning true (`ret` = the value Action returns).
    std::function<bool(Sprite&, int act, int v1, int v2, int& ret)> actionHook;
    std::function<bool(Sprite&, int act, int dir)> changeActionHook;
    std::uint64_t fireCount = 0;   // successful Fire() calls (diagnostics)
    [[nodiscard]] Sprite* flagman(int player) const noexcept {
        return player >= 0 && player < MaxPlayers ? byId(m_players[static_cast<std::size_t>(player)].flagman) : nullptr;
    }
    void setNetGame(bool on) noexcept { m_netGame = on; }

    // ------------------------------------------------------------------ map lifecycle
    // 0x4149b0 (Field::LoadMap): clock, seed, map size, tables, grid, objects with
    // act_restore, ids -> sprites, command stacks, armies/flagmen/squads.
    void load(const Map& map, std::uint32_t seed) {
        clear();
        for (auto& v : m_vids) v.count = 0;
        m_rand.seed(seed);
        m_clock = map.header.startTimer;
        m_paused = false;
        m_windStrength = 0;
        m_windDir = 0x40;
        m_scaleX = static_cast<int>(map.header.scaleX ? map.header.scaleX : 24);
        m_scaleY = static_cast<int>(map.header.scaleY ? map.header.scaleY : 16);
        m_mapW = static_cast<int>(map.header.width);
        m_mapH = static_cast<int>(map.header.height);
        if (m_mapW < 256 && m_mapH < 256) { m_mapW *= m_scaleX; m_mapH *= m_scaleY; }   // 0x414ef2
        m_persp.build(m_scaleX, m_scaleY);
        initGrid();

        std::vector<Sprite*> created(map.objects.size(), nullptr);
        for (std::size_t i = 0; i < map.objects.size(); ++i) {
            const auto& o = map.objects[i];
            Sprite* s = createUnit(o.nvid, o.x, o.y, o.z, 0, o.direction, nullptr);
            created[i] = s;
            if (s) restore(*s, o, map.header.mapVersion);
        }
        auto lookup = [&](std::uint32_t mapId) -> Sprite* {
            for (std::size_t i = 0; i < map.objects.size(); ++i)
                if (map.objects[i].id == mapId) return created[i];
            return nullptr;
        };
        // Commands are read as they are (0x4151d2); act_attack targets are translated.
        for (std::size_t i = 0; i < map.objects.size(); ++i) {
            const auto* asset = std::get_if<Payloads::AssetObject>(&map.objects[i].payload);
            if (!asset || asset->commands.empty() || !created[i]) continue;
            auto cs = std::make_unique<CommandStack>();
            for (const auto& c : asset->commands) {
                Command cmd{static_cast<std::uint8_t>(c.command), static_cast<std::int32_t>(c.p1), static_cast<std::int32_t>(c.p2)};
                if (cmd.act == act::attack) cmd.var1 = idOf(lookup(c.p1));
                cs->e.push_back(cmd);
            }
            cs->count = static_cast<int>(cs->e.size());
            created[i]->commands = std::move(cs);
        }
        // Armies (0x41529c): only the first two records exist in the game's maps.
        for (std::size_t a = 0; a < map.armies.size() && a < m_players.size(); ++a) {
            const auto& army = map.armies[a];
            Player& p = m_players[a];
            p.type = army.a;
            p.b = army.b;
            p.netIndex = army.c;
            p.flagman = army.flagman_id ? idOf(lookup(army.flagman_id)) : 0;
            for (const auto& squad : army.squads) {
                Group g;
                for (auto id : squad)
                    if (Sprite* s = lookup(id)) addGroupMember(g, s);
                m_groups.insert(m_groups.begin(), std::move(g));   // 0x4165f0 pushes at the head
            }
        }
    }

    // Empty map of the given size (tests, tools).
    void loadEmpty(int width, int height, std::uint32_t seed, std::uint32_t clock = 0) {
        Map map;
        map.header.width = static_cast<std::uint32_t>(width);
        map.header.height = static_cast<std::uint32_t>(height);
        map.header.startTimer = clock;
        for (auto& a : map.armies) a = Army{};
        load(map, seed);
    }

    // Destroys every sprite (map unload).
    void clear() {
        for (auto& head : m_layers) head = nullptr;
        for (auto& p : m_pool)
            if (p->alive) p->alive = false;
        m_pool.clear();
        m_free.clear();
        m_pendingFree.clear();
        m_byId.clear();
        m_live = 0;
        m_iterNext = nullptr;
        m_gridUnits.clear();
        for (auto& c : m_cells) c.clear();
        m_groups.clear();
        for (auto& p : m_players) p = Player{};
        m_sounds.clear();
        m_orphan = nullptr;
    }

    // ------------------------------------------------------------------ the clock
    // 0x401f70: curclock += min(dt, 71). During Pause() the clock keeps running (scripts
    // measure menu timings with GetTime()); Pause(0) brings it back, see pause().
    void advanceClock(int elapsedMs) noexcept {
        if (elapsedMs < 0) elapsedMs = 0;
        m_clock += static_cast<std::uint32_t>(elapsedMs > fx::ClockStepCapMs ? fx::ClockStepCapMs : elapsedMs);
    }

    // 0x40ac70 Pause(flag). Divergence: on resume only the sprites that ticked during the
    // pause (screen sprites, state & 4) are shifted back; the others keep their lastTact, so
    // they do not get one huge tick (the "jump forward" of the original).
    void pause(bool on) {
        if (on) {
            if (!m_paused) m_pauseStart = m_clock;
            m_paused = true;
            return;
        }
        if (!m_paused) return;
        const std::uint32_t shift = m_clock - m_pauseStart;
        for (auto* head : m_layers)
            for (Sprite* s = head; s; s = s->next)
                for (Sprite* c = s; c; c = c->child)
                    if (c->lastTact > m_pauseStart) c->lastTact -= std::min(shift, c->lastTact - m_pauseStart);
        m_clock = m_pauseStart;
        m_paused = false;
    }

    // ------------------------------------------------------------------ world tick
    // 0x40b880 World::Tact, update half: per layer, in list order, each sprite whose period
    // elapsed gets one SpriteTact. rand() is consumed every frame for Randomized vids.
    int tact() {
        m_free.insert(m_free.end(), m_pendingFree.begin(), m_pendingFree.end());
        m_pendingFree.clear();
        int ticked = 0;
        for (int layer = 0; layer < MaxLayers; ++layer) {
            Sprite* s = m_layers[layer];
            m_iterNext = s ? s->next : nullptr;
            while (s) {
                const VidInfo& v = m_vids[s->nvid];
                if (!(v.flags & vf::SkipUpdate)) {
                    const bool randomized = (v.flags & vf::Randomized) != 0;
                    const int r = randomized ? rand() : 0;
                    const std::uint32_t period = fx::tickPeriod(v.frameDuration, randomized, r);
                    if (m_clock - s->lastTact >= period) {
                        spriteTact(*s);
                        ++ticked;
                    }
                }
                s = m_iterNext;
                m_iterNext = s ? s->next : nullptr;
            }
        }
        m_iterNext = nullptr;
        return ticked;
    }

    // ------------------------------------------------------------------ creation
    // 0x40e1b0 CreateUnit(nvid, x, y, z, action, dir, parent).
    Sprite* createUnit(int nvid, int x, int y, int z, int startAction, int dir, Sprite* parent) {
        if (nvid < 0 || nvid >= static_cast<int>(m_vids.size())) return nullptr;
        VidInfo& v = m_vids[static_cast<std::size_t>(nvid)];
        if (v.maxUnit >= 0 && v.count >= v.maxUnit) return nullptr;
        if (v.flags & vf::Randomized) {
            x += 8 - rand() % 17;
            y += 8 - rand() % 17;
        }
        if (x < 0 || y < 0 || x > m_mapW || y > m_mapH) return nullptr;
        if (v.cls > 20) return nullptr;
        Sprite* s = allocSprite(v.cls);
        v.count++;
        if (v.animLen[act::birth] == 0) {
            initSprite(*s, nvid, startAction, dir, x, y, z);
            if (parent) s->state |= parent->state & st::GammaMask;
            if (v.childNvid[act::birth] && !m_paused) {
                const auto a = s->action;
                s->action = act::birth;
                createChildren(*s);
                s->action = a;
            }
        } else {
            initSprite(*s, nvid, act::birth, dir, x, y, z);
            if (parent) s->state |= parent->state & st::GammaMask;
        }
        if (v.zLayer == 5) pushFront(s); else insertSorted(s);
        if (v.flags & vf::PresentOnGrid) gridAdd(s);
        return s;
    }

    // 0x40bf30 SPRITE::Init.
    void initSprite(Sprite& s, int nvid, int startAction, int dir, int x, int y, int z) {
        const VidInfo& v = m_vids[static_cast<std::size_t>(nvid)];
        s.nvid = static_cast<std::uint16_t>(nvid);
        s.y = s.goY = static_cast<std::int16_t>(y);
        s.z = s.goZ = static_cast<std::int16_t>(z);
        s.lastTact = m_clock;
        s.action = static_cast<std::uint8_t>(startAction);
        s.direction = static_cast<std::uint8_t>(dir);
        s.lastFrame = static_cast<std::int16_t>(v.animLen[0] - 1);
        s.dx = v.linkX; s.dy = v.linkY; s.dz = v.linkZ;
        s.x = s.goX = static_cast<std::int16_t>(x);
        s.commands.reset();
        s.speed = s.vz = 0;
        s.timer = 0;
        s.stepAccum = 0;
        s.fracX = s.fracY = s.fracZ = 0;
        s.frame = s.firstFrame = 0;
        s.state = 0;
        if (v.linkedObjectVid) {
            Sprite* c = allocSprite(cls::Superstructure);
            s.child = c;
            initSprite(*c, v.linkedObjectVid, 0, dir, x + s.dx, y + s.dy, z + s.dz);
            if (v.flags & vf::InvisibleSubobjects) insertSorted(c);
            c->state |= st::Sub;
        } else {
            s.child = nullptr;
        }
        setAnimation(s);
        if (s.action == act::stand) {
            const int span = s.lastFrame - s.firstFrame;
            if (span > 0 && !(v.cls == cls::Image && span <= 1))
                s.frame = static_cast<std::int16_t>(s.frame + rand() % (span + 1));
        }
        action(s, act::init, 0, 0);
    }

    // Map objects: act_restore with the payload read by libgromada (0x412580 case 52 and
    // the class overrides, e.g. Vehicle 0x40f57e).
    void restore(Sprite& s, const GameObject& o, MapVersion version = MapVersion::V3) {
        const VidInfo& v = vidOf(s);
        const auto material = [&](std::uint8_t hp) {
            if (s.action == act::birth) {
                changeAction(s, act::stand, -1);
                const int span = s.lastFrame - s.firstFrame;
                if (span > 0) s.frame = static_cast<std::int16_t>(s.frame + rand() % (span + 1));
            }
            s.hp = hp;
            if (s.hp < (v.maxHP >> 1)) changeAction(s, act::wound, -1);
        };
        std::visit(overloaded{
            [&](const Payloads::VisualObject&) {},
            [&](const Payloads::MaterialObject& p) { if (hasMaterial(s.cls)) material(p.hp); },
            [&](const Payloads::AssetObject& p) {
                if (!hasMaterial(s.cls)) return;
                material(p.hp);
                if (isUnitClass(s.cls)) {   // Vehicle restore 0x40f57e: fields by map version
                    if (version >= MapVersion::V3) s.buildTime = p.buildTime;
                    if (version >= MapVersion::V2) s.army = p.army;
                    s.behave = p.behave;
                    if (version != MapVersion::V0)
                        for (auto it : p.items) addItem(s, it);
                }
            },
        }, o.payload);
    }

    // 0x40a950 SPRITE::CreateChildren: generators of the current action.
    void createChildren(Sprite& s) {
        createChildren(
            s, [this](int x, int y, int z, int nv) { return askPlaceVid(x, y, z, nv) != nullptr; },
            [this](int nv, int x, int y, int z, int dir, Sprite* parent) { return createUnit(nv, x, y, z, 0, dir, parent); },
            [this](Sprite& c, int x, int y, int z) { setDestination(c, x, y, z); });
    }
    // The same with AskPlaceVid 0x40a2d0, CreateUnit 0x40e1b0 and SetDestination 0x409770 passed
    // in (oracle tests, tools/oracle/gen_weapons.py).
    template <class Occupied, class Create, class Dest>
    void createChildren(Sprite& s, Occupied&& occupied, Create&& create, Dest&& dest) {
        const int nv = std::abs(static_cast<int>(vidOf(s).childNvid[s.action]));
        if (!nv || nv >= static_cast<int>(m_vids.size())) return;
        for (int i = 0; i < vidOf(s).childCount[s.action]; ++i) {
            const VidInfo& v = vidOf(s);
            const int a = s.action;
            const int dir = (m_vids[static_cast<std::size_t>(nv)].flags & vf::RandomDirection) ? (rand() & 0xff) : s.direction;
            const int offX = v.childOff[0][a], offY = v.childOff[1][a], offZ = v.childOff[2][a];
            int ox, oy, tx, ty;   // creation offset, offset added to the aim point
            if (v.childNvid[a] < 0) {
                ox = offX - rand() % (2 * offX + 1);
                oy = offY - rand() % (2 * offY + 1);
                tx = ox; ty = oy;
            } else {
                const int d = s.direction;
                if (i == 1) {
                    ox = (fx::Sin[d] * offY - fx::NegCos[d] * offX) >> 10;
                    oy = (m_persp.negCosS[d] * offY + m_persp.sinS[d] * offX) >> 10;
                    tx = (-fx::NegCos[d] * offX) >> 10;
                    ty = (m_persp.sinS[d] * offX) >> 10;
                } else if (i == 2) {
                    ox = (fx::Sin[d] * offY) >> 10;
                    oy = (-(m_persp.negCosS[d] * offY)) >> 10;
                    tx = 0; ty = 0;
                } else {
                    ox = (fx::Sin[d] * offY + fx::NegCos[d] * offX) >> 10;
                    oy = (m_persp.negCosS[d] * offY - m_persp.sinS[d] * offX) >> 10;
                    tx = (fx::NegCos[d] * offX) >> 10;
                    ty = (-(m_persp.sinS[d] * offX)) >> 10;
                }
            }
            const int cx = s.x + ox, cy = s.y + oy, cz = s.z + offZ;
            Sprite* c = nullptr;
            if (m_vids[static_cast<std::size_t>(nv)].cls != cls::Vehicle || !occupied(cx, cy, cz, nv))
                c = create(nv, cx, cy, cz, dir, &s);
            if (!c) continue;
            if (s.action == act::fight) {
                const int sc = weaponOf(s).scatter;
                const int yy = s.attackY - rand() % (2 * sc + 1) + sc + ty;   // y first (0x40abd9)
                const int xx = s.attackX - rand() % (2 * sc + 1) + sc + tx;
                dest(*c, xx, yy, static_cast<int>(s.attackZ));
            }
            action(*c, act::child, s.id, i);
        }
    }

    // ------------------------------------------------------------------ animation
    // 0x40b330 SetAnimation.
    void setAnimation(Sprite& s) const {
        const VidInfo& v = vidOf(s);
        int a = s.action < 16 ? s.action : 0;
        if (!v.animLen[a]) a = 0;
        int first = 0;
        for (int i = 0; i < a; ++i) first += v.animLen[i] * v.dirs;
        first += fx::directionIndex(v.dirs, s.direction) * v.animLen[a];
        s.frame = s.firstFrame = static_cast<std::int16_t>(first);
        s.lastFrame = static_cast<std::int16_t>(first + v.animLen[a] - 1);
    }

    // 0x40b3e0 ChangeAction(act, dir = -1).
    void changeAction(Sprite& s, int a, int dir) {
        if (changeActionHook && changeActionHook(s, a, dir)) return;
        s.state &= ~st::SoundDone;
        a &= 0xff;
        if (s.child) {
            if (a == act::death) changeAction(*s.child, act::death, -1);
            if ((a == act::open || a == act::close || a == act::stand_open) && a < 16 && vidOf(*s.child).animLen[a])
                changeAction(*s.child, a, -1);
        }
        if (dir == -1) dir = s.direction;
        if (s.action == a && s.direction == dir) return;
        const VidInfo& v = vidOf(s);
        if (s.action == act::stand_open && (a >= 16 || !v.animLen[a])) {
            s.direction = static_cast<std::uint8_t>(dir);
            s.action = static_cast<std::uint8_t>(a);
        } else {
            s.direction = static_cast<std::uint8_t>(dir);
            s.action = static_cast<std::uint8_t>(a);
            setAnimation(s);
        }
        if (s.action == act::death && !v.animLen[act::death]) s.frame = s.lastFrame;
        if (s.action == act::wound && !v.animLen[act::wound]) s.frame = s.lastFrame;
        if (s.action == act::birth && !v.animLen[act::birth]) s.frame = s.lastFrame;
    }

    // 0x40b610: direction = d, SetAnimation.
    void setDirection(Sprite& s, int d) const {
        s.direction = static_cast<std::uint8_t>(d);
        setAnimation(s);
    }

    // 0x40b540: turn by one step keeping the animation phase, rotating the offsets of the
    // sub-object chain (perspective tables).
    void rotateChain(Sprite& s, std::uint8_t newDir) {
        const int phase = s.frame - s.firstFrame;
        if (newDir != s.direction) {
            const std::uint8_t delta = static_cast<std::uint8_t>(newDir - s.direction);
            for (Sprite* p = &s; p->child; p = p->child) {
                if (!p->dx && !p->dy) continue;
                const int dx = p->dx, dy = p->dy;
                const int ndx = (fx::NegCos[delta] * dx + fx::Sin[delta] * dy) >> 10;
                const int ndy = (m_persp.negCosS[delta] * dy - m_persp.sinS[delta] * dx) >> 10;
                p->dx = static_cast<std::int8_t>(-ndx);
                p->dy = static_cast<std::int8_t>(-ndy);
            }
        }
        s.direction = newDir;
        setAnimation(s);
        s.frame = static_cast<std::int16_t>(s.frame + phase);
    }

    // 0x40b0b0 TurnTo(dir): 1 when aligned, otherwise starts a rotation animation.
    int turnTo(Sprite& s, int target) {
        target &= 0xff;
        if (s.direction == target) return 1;
        const VidInfo& v = vidOf(s);
        if (v.dirs == 1 || (v.flags & vf::RandomDirection)) {
            s.direction = static_cast<std::uint8_t>(target);
            return 1;
        }
        const int dirs = v.dirs;
        const int half = v.dirStep >> 1;
        const int qc = (((half + s.direction) & 0xff) * dirs) >> 8;
        const int qt = (((half + target) & 0xff) * dirs) >> 8;
        const int diff = std::abs(qc - qt), other = dirs - diff;
        bool left;
        if (qc > qt) left = diff < other;
        else if (qc < qt) left = diff > other;
        else { s.direction = static_cast<std::uint8_t>(target); return 1; }
        if (left) {
            if (s.action == act::r_rotate) { changeAction(s, act::go, -1); return 0; }
            changeAction(s, act::l_rotate, fx::rotateStep(dirs, s.direction, false));
        } else {
            if (s.action == act::l_rotate) { changeAction(s, act::go, -1); return 0; }
            changeAction(s, act::r_rotate, fx::rotateStep(dirs, s.direction, true));
        }
        return 0;
    }

    // 0x40b220: one rotation step of a turret (no animation), used every frame by the
    // player and by the "fire in the current direction" weapons.
    int turnStep(Sprite& s, int target) {
        target &= 0xff;
        const VidInfo& v = vidOf(s);
        if (v.dirs == 1 || (v.flags & vf::RandomDirection)) { s.direction = static_cast<std::uint8_t>(target); return 1; }
        const int dirs = v.dirs;
        const int half = v.dirStep >> 1;
        const int qc = (((half + s.direction) & 0xff) * dirs) >> 8;
        const int qt = (((half + target) & 0xff) * dirs) >> 8;
        const int diff = std::abs(qc - qt), other = dirs - diff;
        if (qc == qt) return 1;
        const bool left = qc > qt ? diff < other : diff > other;
        rotateChain(s, fx::rotateStep(dirs, s.direction, !left));
        return 0;
    }

    // ------------------------------------------------------------------ the sprite tick
    // 0x409a20 SPRITE::Tact. Returns 1 when the sprite has been destroyed.
    int spriteTact(Sprite& s) {
        if (s.child && spriteTact(*s.child)) {   // the child died: re-attach its orphans
            s.child = m_orphan;
            s.dx = static_cast<std::int8_t>(s.dx + m_orphanDx);
            s.dy = static_cast<std::int8_t>(s.dy + m_orphanDy);
            s.dz = static_cast<std::int8_t>(s.dz + m_orphanDz);
        }
        if (!(s.state & st::SoundDone)) {
            const auto nsfx = vidOf(s).nsfx[s.action & 15];
            if (nsfx) {
                s.state |= st::SoundDone;
                SoundRequest r{nsfx, s.x, s.y};
                if (s.state & st::Screen) { r.x += m_camX; r.y += m_camY; }
                m_sounds.push_back(r);
            }
        }
        s.frame++;
        const std::uint32_t dt = m_clock - s.lastTact;
        if (s.timer) s.timer = dt > s.timer ? 0 : static_cast<std::uint16_t>(s.timer - dt);
        if (!m_paused && vidOf(s).childNvid[s.action & 15] &&
            (s.frame > s.lastFrame || (vidOf(s).flags & vf::SpawnChildren)))
            createChildren(s);
        if (s.frame > s.lastFrame && s.action == act::death) {
            bool die = true;
            if (vidOf(s).cls == cls::Effect2) {
                m_orphan = s.child;
                m_orphanDx = s.dx; m_orphanDy = s.dy; m_orphanDz = s.dz;
                s.child = nullptr;
            } else if (s.state & st::Sub) {
                die = false;   // sub-objects die with their parent
            } else {
                m_orphan = nullptr;
            }
            if (die) {
                action(s, act::death, 0, 0);
                action(s, act::destroy, 0, 0);
                return 1;
            }
        }
        const VidInfo& v = vidOf(s);
        if ((!m_paused || (s.state & st::Screen)) && !(v.category <= cat::Object && !(s.state & st::Sub)) &&
            !(s.state & st::Passive)) {
            const int step = physics(s);
            if (!s.alive) return 1;
            if (commandsDue(s)) nextCommand(s, step);
            if (!s.alive) return 1;
            if (vidOf(s).flags & vf::Calltact) action(s, act::calltact, 0, 0);
            if (!s.alive) return 1;
        } else if (s.action == act::birth && s.frame > s.lastFrame) {
            changeAction(s, act::stand, -1);
        }
        s.lastTact = m_clock;
        if (s.frame > s.lastFrame) {
            s.frame = s.firstFrame;
            if (!m_paused && vidOf(s).cls != cls::Image) s.state &= ~st::SoundDone;
        }
        for (Sprite *p = &s, *c = s.child; c; p = c, c = c->child) {
            c->x = static_cast<std::int16_t>(p->x + p->dx);
            c->y = static_cast<std::int16_t>(p->y + p->dy);
            c->z = static_cast<std::int16_t>(p->z + p->dz);
        }
        return 0;
    }

    // 0x409cbb..0x40a116: gravity, speed, altitude, horizontal move, grid. Returns the
    // pixels travelled this tick (0x7fff when standing), used by NextCommand.
    int physics(Sprite& s) {
        return physics(s, [this](Sprite& m, int x, int y, int z) { return checkMove(m, x, y, z) != nullptr; },
                       [this](Sprite& m, int x, int y, int z) { return firstObstacleOnLine(m, x, y, z); });
    }
    // The same with the collaborators passed in (the oracle tests replace CheckMove 0x40a5c0 and
    // FirstObstacleOnLine 0x40a770 like tools/oracle/gen_physics.py does).
    template <class Blocked, class Obstacle>
    int physics(Sprite& s, Blocked&& blocked, Obstacle&& obstacle) {
        const VidInfo& v = vidOf(s);
        const std::uint32_t dt = m_clock - s.lastTact;
        const bool gravity = (v.flags & vf::Gravity) != 0;
        if (gravity && s.z) s.vz = fx::applyGravity(s.vz, dt);
        s.speed = fx::integrateSpeed(s.speed, v.acceleration, v.speedX, dt, (s.state & st::Accelerate) != 0);
        if (s.vz) {
            const auto zs = fx::verticalStep(s.vz, dt, s.fracZ);
            s.fracZ = zs.frac;
            if (s.z + zs.delta < 0 && s.z >= 0) {
                action(s, act::path_block, 0, 0);   // touches the ground
                if (!s.alive) return 0x7fff;
            } else if (gravity) {
                s.z = static_cast<std::int16_t>(s.z + zs.delta);
            } else if (s.goZ > s.z) {
                if (s.goZ - s.z <= zs.delta) { s.z = s.goZ; s.vz = 0; } else s.z = static_cast<std::int16_t>(s.z + zs.delta);
            } else if (s.goZ < s.z) {
                if (zs.delta < s.goZ - s.z) { s.z = s.goZ; s.vz = 0; } else s.z = static_cast<std::int16_t>(s.z + zs.delta);
            } else {
                s.vz = 0;
            }
        }
        const bool wind = m_windStrength != 0 && (v.flags & vf::Wind);
        if (s.speed == 0 && !wind) return 0x7fff;
        const int oldCx = s.x >> m_gridShiftX, oldCy = s.y >> m_gridShiftY;
        int step = 0;
        if (s.speed != fx::Instant) {
            const auto m = fx::moveStep(s.speed, dt, s.direction, s.fracX, s.fracY, m_windStrength, m_windDir,
                                        (v.flags & vf::Wind) != 0);
            s.fracX = m.fracX;
            s.fracY = m.fracY;
            step = m.step + s.stepAccum;
            const int rp = v.rotationPeriod;
            s.stepAccum = static_cast<std::uint8_t>(step >= rp ? step - rp : step);
            if (s.state & st::Move) {
                const int nx = s.x + m.mx, ny = s.y + m.my;
                if ((s.goX >= s.x && s.goX <= nx) || (s.goX >= nx && s.goX <= s.x)) s.state |= st::ArrivedX;
                if ((s.goY >= s.y && s.goY <= ny) || (s.goY >= ny && s.goY <= s.y)) s.state |= st::ArrivedY;
            }
            const int nx = s.x + m.mx, ny = s.y + m.my;
            if (nx >= 0 && ny >= 0 && nx <= m_mapW && ny <= m_mapH) {
                if (blocked(s, nx, ny, static_cast<int>(s.z))) action(s, act::path_block, m.mx, m.my);
                else { s.x = static_cast<std::int16_t>(nx); s.y = static_cast<std::int16_t>(ny); }
            } else {
                action(s, act::path_limit, m.mx, m.my);
            }
            if (!s.alive) return step;
        } else {
            const std::uint32_t p = obstacle(s, static_cast<int>(s.goX), static_cast<int>(s.goY), static_cast<int>(s.goZ));
            if (p) { s.x = static_cast<std::int16_t>(p & 0xffff); s.y = static_cast<std::int16_t>(p >> 16); }
            else { s.x = s.goX; s.y = s.goY; }
            s.state |= st::ArrivedX | st::ArrivedY;
            s.z = s.goZ;
        }
        if (vidOf(s).flags & vf::PresentOnGrid) {
            const int cx = s.x >> m_gridShiftX, cy = s.y >> m_gridShiftY;
            if (cx != oldCx || cy != oldCy) {
                cellRemove(oldCx, oldCy, &s);
                cellAdd(cx, cy, &s);
            }
        }
        return step;
    }

    // 0x40a116: commands are evaluated at the end of the animation or while the sprite stands,
    // moves or rotates (the sprite's *action* < 6, docs/oracle.md §3.1).
    [[nodiscard]] static bool commandsDue(const Sprite& s) noexcept { return s.frame > s.lastFrame || s.action < 6; }

    // 0x40a12a: the command stack or act_nextcommand.
    void nextCommand(Sprite& s, int step) {
        auto* cs = s.commands.get();
        if (cs && cs->count > 0 &&
            (!(s.state & st::CommandMask) || (!(s.state & st::Move) && cs->top().act == act::submove))) {
            cs->count--;
            int idx = cs->count;
            if (cs->e[idx].act == act::cyclecommand) {
                idx = static_cast<std::int16_t>(cs->e[idx].var1);
                cs->count = idx;
                if (idx < 0) idx = 0;
                cs->ensure(idx + 1);
            }
            const Command c = cs->e[static_cast<std::size_t>(idx)];
            if (c.act > 15) action(s, c.act, c.var1, c.var2);
            else changeAction(s, c.act, -1);
            if (s.alive && s.commands && s.commands->count <= 0) s.commands.reset();
            return;
        }
        if ((s.state & 3) == 3 || (s.action != act::go && s.action != act::l_rotate && s.action != act::r_rotate) ||
            step >= vidOf(s).rotationPeriod)
            action(s, act::nextcommand, 0, 0);
    }

    // ------------------------------------------------------------------ commands
    // 0x40c7c0: script AddCommand, at the bottom (e[0]); refused after act_cyclecommand.
    void pushCommandBottom(Sprite& s, int a, int v1, int v2) {
        if (!s.commands) s.commands = std::make_unique<CommandStack>();
        auto& cs = *s.commands;
        if (cs.count && cs.e[0].act == act::cyclecommand) return;
        cs.ensure(cs.count + 1);
        for (int i = cs.count; i > 0; --i) cs.e[static_cast<std::size_t>(i)] = cs.e[static_cast<std::size_t>(i - 1)];
        cs.e[0] = Command{static_cast<std::uint8_t>(a), v1, v2};
        cs.count++;
    }
    // 0x40c890: engine push on top (executed first).
    void pushCommandTop(Sprite& s, int a, int v1, int v2) {
        if (!s.commands) s.commands = std::make_unique<CommandStack>();
        auto& cs = *s.commands;
        cs.ensure(cs.count + 1);
        cs.e[static_cast<std::size_t>(cs.count)] = Command{static_cast<std::uint8_t>(a), v1, v2};
        cs.count++;
    }
    // 0x40c630: remove entry i.
    void removeCommand(Sprite& s, int i) {
        if (!s.commands || i >= s.commands->count) return;
        auto& cs = *s.commands;
        for (int k = i; k + 1 < cs.count; ++k) cs.e[static_cast<std::size_t>(k)] = cs.e[static_cast<std::size_t>(k + 1)];
        if (--cs.count == 0) s.commands.reset();
    }
    // 0x40c690: waypoint of the player/editor, closes a patrol loop when near.
    int addWaypoint(Sprite& s, int a, int x, int y) {
        auto* cs = s.commands.get();
        if (cs && cs->count && cs->e[0].act == act::cyclecommand) return 0;
        if (cs && (a == act::move || a == act::coor_attack)) {
            if (std::abs(x - s.x) < 24 && std::abs(y - s.y) < 16) {
                pushCommandBottom(s, a, s.x, s.y);
                pushCommandBottom(s, act::cyclecommand, s.commands->count, 0);
                return 1;
            }
            for (int i = 1; i < cs->count; ++i) {
                const Command& c = cs->e[static_cast<std::size_t>(i)];
                if ((c.act == act::move || c.act == act::coor_attack) && std::abs(x - c.var1) < 24 &&
                    std::abs(y - c.var2) < 16) {
                    const Command copy = c;
                    pushCommandBottom(s, a, copy.var1, copy.var2);
                    pushCommandBottom(s, act::cyclecommand, i + 1, 0);
                    return 1;
                }
            }
        }
        pushCommandBottom(s, a, x, y);
        return 0;
    }
    // 0x40c550 GetCommands / 0x40c5b0 SetCommands.
    std::string getCommands(const Sprite& s) const {
        std::string out;
        if (s.commands)
            for (int i = 0; i < s.commands->count; ++i) {
                const auto& c = s.commands->e[static_cast<std::size_t>(i)];
                out += static_cast<char>(c.act + 60);
                out += std::to_string(c.var1) + "," + std::to_string(c.var2);
            }
        return out;
    }
    void setCommands(Sprite& s, std::string_view text) {
        std::size_t i = 0;
        while (i < text.size() && text[i]) {
            const int a = static_cast<unsigned char>(text[i]) - 60;
            std::size_t j = i + 1;
            auto readInt = [&](int& out) {
                bool neg = false;
                if (j < text.size() && (text[j] == '-' || text[j] == '+')) neg = text[j++] == '-';
                int v = 0;
                bool any = false;
                while (j < text.size() && text[j] >= '0' && text[j] <= '9') { v = v * 10 + (text[j++] - '0'); any = true; }
                out = neg ? -v : v;
                return any;
            };
            int v1 = 0, v2 = 0;
            if (!readInt(v1)) return;
            if (j >= text.size() || text[j] != ',') return;
            ++j;
            if (!readInt(v2)) return;
            pushCommandTop(s, a, v1, v2);
            i = j;
            // skip to the next character >= '<' (0x40c602)
            while (i < text.size() && text[i] && static_cast<unsigned char>(text[i]) < 0x3c) ++i;
        }
    }

    // ------------------------------------------------------------------ movement helpers
    // 0x409770 SetDestination(x, y, z).
    void setDestination(Sprite& s, int x, int y, int z) {
        const VidInfo& v = vidOf(s);
        s.state &= ~(st::ArrivedX | st::ArrivedY);
        int dist = 0;
        if (v.speedX) {
            const auto d = fx::getDirection(s.x, s.y, x, y);
            if (v.dirs == 1 || (v.flags & vf::RandomDirection)) s.direction = d.dir();
            dist = d.distance;
            m_lastDist = dist;
            if (!dist) return;
            s.state |= st::Move | st::Accelerate;
        } else {
            m_lastDist = 0;
        }
        s.goX = static_cast<std::int16_t>(x);
        s.goY = static_cast<std::int16_t>(y);
        s.goZ = static_cast<std::int16_t>(z);
        s.fracX = s.fracY = s.fracZ = 0;
        if ((v.category == cat::Cannon || v.cls == cls::AviaMissile) && dist && v.speedX) {
            int vz;
            if (v.flags & vf::Gravity)
                vz = static_cast<std::int16_t>(v.speedX * (s.goZ - s.z) / dist + (((dist * fx::Gravity) / v.speedX) >> 1));
            else
                vz = static_cast<std::int16_t>((s.goZ - s.z) * v.speedX / dist);
            if (vz > v.speedY) vz = v.speedY;
            if (vz < -v.speedY) vz = -v.speedY;
            s.vz = static_cast<std::int16_t>(vz);
        } else {
            const auto gz = static_cast<std::int16_t>(z);
            s.vz = gz > s.z ? v.speedY : gz < s.z ? static_cast<std::int16_t>(-v.speedY) : std::int16_t{0};
        }
        if (s.x != static_cast<std::int16_t>(x) || s.y != static_cast<std::int16_t>(y))
            changeAction(s, s.speed ? act::go : act::start_move, -1);
    }

    // 0x409950 Stop.
    static void stop(Sprite& s) noexcept {
        s.state &= ~(st::ArrivedX | st::ArrivedY | st::Move | st::Accelerate);
        s.goX = s.x; s.goY = s.y; s.goZ = s.z;
        s.vz = 0;
    }

    // 0x40e780 ClearWaypoint.
    void clearWaypoint(Sprite& s) {
        s.wpX = s.wpY = -1;
        stop(s);
        changeAction(s, act::stand, -1);
    }

    // 0x40e9a0 Approach(x, y): waypoint + destination if inside the map.
    void approach(Sprite& s, int x, int y) {
        const VidInfo& v = vidOf(s);
        if (!v.speedX) return;
        if (x - v.halfX < 0 || y - v.halfY < 0 || x + v.halfX >= m_mapW || y + v.halfY >= m_mapH) return;
        s.wpX = static_cast<std::int16_t>(x);
        s.wpY = static_cast<std::int16_t>(y);
        setDestination(s, s.wpX, s.wpY, s.z);
    }

    [[nodiscard]] static int distance(const Sprite& a, const Sprite& b) noexcept { return fx::gameDistance(a.x - b.x, a.y - b.y); }
    [[nodiscard]] static int distanceTo(const Sprite& a, int x, int y) noexcept {   // 0x40afc0: int32 point
        return fx::gameDistance(fx::wrapSub(a.x, x), fx::wrapSub(a.y, y));
    }
    fx::DirectionResult getDirection(int x1, int y1, int x2, int y2) {
        const auto d = fx::getDirection(x1, y1, x2, y2);
        m_lastDist = d.distance;
        return d;
    }

    // ------------------------------------------------------------------ collisions
    // 0x40a5c0 CheckMove (crush = true) and 0x40a430 AskPlace (crush = false).
    Sprite* checkMove(Sprite& s, int x, int y, int z, bool crush = true) {
        const VidInfo& v = vidOf(s);
        if (v.category != cat::Vehicle && v.category != cat::Avia && v.category != cat::Cannon) return nullptr;
        if (z < 0) return &s;
        if (v.flags & vf::NoCollision) return nullptr;
        Sprite* found = nullptr;
        forEachInRect(x - v.halfX, y - v.halfY, x + v.halfX, y + v.halfY, [&](Sprite* o) {
            const VidInfo& ov = vidOf(*o);
            if ((ov.flags & vf::NoCollision) || o->action == act::death) return true;
            if (std::abs(x - o->x) >= ov.halfX + v.halfX || std::abs(y - o->y) >= ov.halfY + v.halfY) return true;
            if ((ov.category & cat::Terrain) && z == 0) {
                if (!(ov.collisionMask & v.collisionMask)) { found = o; return false; }
            } else if (o != &s) {
                if (std::abs(2 * (z - o->z) - ov.sizeZ + v.sizeZ) < ov.absSizeZ + v.absSizeZ) {
                    if (crush && (ov.flags & vf::CrushOnCollision)) action(*o, act::damage, 2, 0);
                    else { found = o; return false; }
                }
            }
            return true;
        });
        return found;
    }
    Sprite* askPlace(Sprite& s, int x, int y, int z) { return checkMove(s, x, y, z, false); }

    // 0x40e7b0: AskPlace that reports the sprite itself outside the map.
    Sprite* askPlaceBounded(Sprite& s, int x, int y) {
        if (x < 0 || y < 0 || x > m_mapW || y > m_mapH) return &s;
        return askPlace(s, x, y, s.z);
    }

    // 0x40a2d0 AskPlaceVid(x, y, z, nvid): the same test for a vid that does not exist yet.
    Sprite* askPlaceVid(int x, int y, int z, int nvid) {
        if (nvid < 0 || nvid >= static_cast<int>(m_vids.size())) return nullptr;
        const VidInfo& v = m_vids[static_cast<std::size_t>(nvid)];
        if (v.category != cat::Vehicle && v.category != cat::Avia && v.category != cat::Cannon) return nullptr;
        if (v.flags & vf::NoCollision) return nullptr;
        Sprite* found = nullptr;
        forEachInRect(x - v.halfX, y - v.halfY, x + v.halfX, y + v.halfY, [&](Sprite* o) {
            const VidInfo& ov = vidOf(*o);
            if ((ov.flags & vf::NoCollision) || o->action == act::death) return true;
            if (std::abs(x - o->x) >= ov.halfX + v.halfX || std::abs(y - o->y) >= ov.halfY + v.halfY) return true;
            if ((ov.category & cat::Terrain) && z == 0) {
                if (!(ov.collisionMask & v.collisionMask)) { found = o; return false; }
            } else if (std::abs(2 * (z - o->z) + v.sizeZ - ov.sizeZ) < ov.absSizeZ + v.absSizeZ) {
                found = o;
                return false;
            }
            return true;
        });
        return found;
    }

    // 0x40a770 FirstObstacleOnLine(x2, y2, z2): 16 px steps from the sprite; returns
    // (y << 16) + x of the first blocked probe as a 32-bit sum (a negative x borrows from y, like
    // the original), 0 if the line is free.
    std::uint32_t firstObstacleOnLine(Sprite& s, int x2, int y2, int z2) {
        return firstObstacleOnLine(s, x2, y2, z2, [this](Sprite& m, int x, int y, int z) { return askPlace(m, x, y, z) != nullptr; });
    }
    // The same with the probe (AskPlace 0x40a430) passed in (oracle tests).
    template <class Probe>
    std::uint32_t firstObstacleOnLine(Sprite& s, int x2, int y2, int z2, Probe&& blocked) {
        const VidInfo& v = vidOf(s);
        if (v.category != cat::Vehicle && v.category != cat::Avia && v.category != cat::Cannon) return 0;
        if (v.flags & vf::NoCollision) return 0;
        int major = s.x, minor = s.y;
        int sMajor = x2 > s.x ? 1 : -1, sMinor = y2 > s.y ? 1 : -1;
        std::uint32_t lMajor = static_cast<std::uint32_t>(fx::wrapAbs(fx::wrapSub(x2, s.x)));
        std::uint32_t lMinor = static_cast<std::uint32_t>(fx::wrapAbs(fx::wrapSub(y2, s.y)));
        bool swapped = false;
        if (lMinor > lMajor) {
            std::swap(major, minor);
            std::swap(lMajor, lMinor);
            std::swap(sMajor, sMinor);
            swapped = true;
        }
        int z = s.z;
        int zStep = 0;
        if (z2 > z && (lMajor & ~0xfu)) zStep = static_cast<int>(static_cast<std::uint32_t>(z2 - z) / (lMajor >> 4));
        int err = static_cast<int>(2 * lMinor) - static_cast<int>(lMajor);
        const int twoMinor = static_cast<int>(2 * lMinor), twoMajor = static_cast<int>(2 * lMajor);
        for (std::uint32_t t = 0; t < lMajor; t += 16) {
            if (t > 16) {
                const int px = swapped ? minor : major, py = swapped ? major : minor;
                if (blocked(s, px, py, z))
                    return (static_cast<std::uint32_t>(py) << 16) + static_cast<std::uint32_t>(px);   // 32-bit sum (0x40a91b)
            }
            if (err >= 0) {
                do { minor += sMinor * 16; err -= twoMajor; } while (err >= 0);
            }
            major += sMajor * 16;
            z += zStep;
            err += twoMinor;
        }
        if (blocked(s, x2, y2, z2))
            return (static_cast<std::uint32_t>(y2) << 16) + static_cast<std::uint32_t>(x2);
        return 0;
    }

    // 0x416840 GetObject(nvid, x, y): grid only.
    Sprite* getObject(int nvid, int x, int y) {
        if (nvid < 0 || nvid >= static_cast<int>(m_vids.size())) return nullptr;
        const VidInfo& v = m_vids[static_cast<std::size_t>(nvid)];
        Sprite* found = nullptr;
        forEachInRect(x - v.halfX, y - v.halfY, x + v.halfX, y + v.halfY, [&](Sprite* o) {
            if (o->nvid != nvid) return true;
            const VidInfo& ov = vidOf(*o);
            if (x < o->x - ov.halfX || x > o->x + ov.halfX || y < o->y - ov.halfY || y > o->y + ov.halfY) return true;
            found = o;
            return false;
        });
        return found;
    }

    // 0x40acf0: unit (grid unit list) under the mouse.
    Sprite* unitAt(int mx, int my) const {
        for (Sprite* u : m_gridUnits) {
            const VidInfo& v = vidOf(*u);
            if (mx > u->x - v.halfX && mx < u->x + v.halfX && my > u->y - v.sizeZ - u->z - v.halfY &&
                my < u->y - u->z + v.halfY)
                return u;
        }
        return nullptr;
    }

    // ------------------------------------------------------------------ weapons
    // 0x40c970 FindTarget.
    Sprite* findTarget(Sprite& s) {
        const VidInfo& v = vidOf(s);
        const Weapon& w = weaponOf(s);
        const int radius = v.visibilityRadius;
        const auto tcat = static_cast<std::uint8_t>(w.targetCategory);
        if (!radius || !tcat) return nullptr;
        const bool directional = (w.flags & 2) != 0;
        const bool antiAir = (w.flags & 8) != 0;
        const int myArmy = action(s, act::getarmy, 0, 0);
        Sprite* best = nullptr;
        int bestDist = radius + 1;
        for (std::size_t i = 0; i < m_gridUnits.size(); ++i) {
            Sprite* u = m_gridUnits[i];
            const int ua = action(*u, act::getarmy, 0, 0);
            const VidInfo& uv = vidOf(*u);
            if (!(uv.category & tcat) || ua == 2 || ua == myArmy || (u->state & st::Invisible)) continue;
            const int d = distance(s, *u);
            if (d > radius) continue;
            if (antiAir && best && rand() % 3 == 0) return best;
            bool take = false;
            if (best) {
                const auto bc = vidOf(*best).category;
                if (bc == cat::Avia) { if (uv.category != cat::Avia) continue; }
                else if (uv.category == cat::Avia) take = true;
            }
            if (!take) {
                if (bestDist <= d) continue;
                if (directional) {
                    int dd = std::abs(static_cast<int>(getDirection(s.x, s.y, u->x, u->y).dir()) - s.direction);
                    if (dd > 128) dd = 255 - dd;
                    if (dd >= 32) continue;
                }
            }
            bestDist = d;
            best = u;
        }
        return best;
    }

    // 0x40ada0 Aim(x, y, z).
    int aim(Sprite& s, int x, int y, int z) {
        const VidInfo& v = vidOf(s);
        const Weapon& w = weaponOf(s);
        const int range = static_cast<std::int16_t>(w.weaponRange);
        Sprite* c = s.child;
        if (c && v.linkedObjectVid) {
            if (!vidOf(*c).childNvid[act::fight]) return 0;
            int d = y;   // flags & 4: the original reuses a stale value here
            if (!(w.flags & 4)) {
                d = getDirection(s.x, s.y, x, y).dir();
                if (!turnTo(*c, d) && !(w.flags & 1)) return 0;
            }
            int ax = x, ay = y;
            if (m_lastDist > range) {
                ax = s.x + ((fx::Sin[d & 0xff] * range) >> 10);
                ay = s.y + ((fx::NegCos[d & 0xff] * range) >> 10);
            }
            c->attackX = static_cast<std::int16_t>(ax);
            c->attackY = static_cast<std::int16_t>(ay);
            c->attackZ = static_cast<std::int16_t>(z);
            changeAction(*c, act::fight, -1);
            return 1;
        }
        if (!v.childNvid[act::fight]) return 0;
        const int d = getDirection(s.x, s.y, x, y).dir();
        if (!turnTo(s, d) && !(w.flags & 1)) return 0;
        int ax = x, ay = y;
        if (m_lastDist > range) {
            ax = s.x + ((fx::Sin[d] * range) >> 10);
            ay = s.y + ((fx::NegCos[d] * range) >> 10);
        }
        s.attackX = static_cast<std::int16_t>(ax);
        s.attackY = static_cast<std::int16_t>(ay);
        s.attackZ = static_cast<std::int16_t>(z);
        return 1;
    }

    // 0x40e6b0 Fire(x, y, z).
    int fire(Sprite& s, int x, int y, int z) {
        const VidInfo& v = vidOf(s);
        if (s.timer) {
            if (s.child && v.linkedObjectVid) turnTo(*s.child, getDirection(s.x, s.y, x, y).dir());
            return 0;
        }
        if (!aim(s, x, y, z)) return 0;
        s.timer = static_cast<std::uint16_t>(weaponOf(s).cooldown * 10);
        if (!v.linkedObjectVid) changeAction(s, act::fight, -1);
        ++fireCount;
        return 1;
    }

    // 0x40c0e0: removes the sub-object of a given vid from the chain (wound effects).
    void removeChildNvid(Sprite& s, int nvid) {
        for (Sprite* p = &s; p->child; p = p->child) {
            Sprite* c = p->child;
            if (c->nvid == nvid) {
                p->child = c->child;
                c->child = nullptr;
                action(*c, act::destroy, 0, 0);
                return;
            }
        }
    }

    // ------------------------------------------------------------------ items (0x40d0b0..)
    void addItem(Sprite& s, int nvid) {
        if (nvid < 0 || nvid >= static_cast<int>(m_vids.size())) return;
        s.items.push_back(static_cast<std::int16_t>(nvid));
    }
    static int deleteItem(Sprite& s, int nvid) {
        for (std::size_t i = 0; i < s.items.size(); ++i)
            if (s.items[i] == static_cast<std::int16_t>(nvid)) {
                s.items[i] = s.items.back();
                s.items.pop_back();
                return 1;
            }
        return 0;
    }
    static int askItem(const Sprite& s, int nvid) {
        return std::ranges::find(s.items, static_cast<std::int16_t>(nvid)) != s.items.end() ? 1 : 0;
    }

    // ------------------------------------------------------------------ groups (0x4165f0..)
    static void addGroupMember(Group& g, Sprite* s) {   // 0x4166e0
        if (g.members.empty()) { g.centerX = s->x; g.centerY = s->y; }
        else { g.centerX = (g.centerX + s->x) >> 1; g.centerY = (g.centerY + s->y) >> 1; }
        g.members.push_back(s);
    }
    void removeFromGroups(Sprite* s) {   // 0x4166a0
        for (auto it = m_groups.begin(); it != m_groups.end();) {
            auto& m = it->members;
            auto f = std::ranges::find(m, s);
            if (f != m.end()) {
                *f = m.back();
                m.pop_back();
                if (m.empty()) { it = m_groups.erase(it); continue; }
            }
            ++it;
        }
    }
    // 0x416730 Groups::Tact.
    void groupsTact() {
        for (auto& g : m_groups) {
            int target = 0;
            for (Sprite* m : g.members)
                if (m->state & st::Attack) { target = action(*m, act::gettarget, 0, 0); break; }
            for (std::size_t i = 0; i < g.members.size(); ++i) {
                Sprite* m = g.members[i];
                if (m->state & st::CommandMask) continue;
                if (distanceTo(*m, g.centerX, g.centerY) > 640 && (m->behave & 2)) {
                    const int y = g.centerY - rand() % 201 + 100;
                    const int x = g.centerX - rand() % 201 + 100;
                    action(*m, act::move, x, y);
                } else if (target) {
                    action(*m, act::attack, target, 0);
                }
            }
        }
    }

    // ------------------------------------------------------------------ grid (0x431ab0)
    // Visits the sprites of the cells covering the rectangle (extended by one cell) in the
    // original order; fn returns false to stop.
    template <class Fn>
    void forEachInRect(int x1, int y1, int x2, int y2, Fn&& fn) {
        if (m_cells.empty()) return;
        int cx1 = (x1 >> m_gridShiftX) - 1, cy1 = (y1 >> m_gridShiftY) - 1;
        int cx2 = (x2 >> m_gridShiftX) + 1, cy2 = (y2 >> m_gridShiftY) + 1;
        if (cx1 < 0) cx1 = 0;
        if (cy1 < 0) cy1 = 0;
        if (cx2 >= m_gridStride) cx2 = m_gridStride - 1;
        if (cy2 >= m_gridRows) cy2 = m_gridRows - 1;
        for (int cy = cy1; cy <= cy2; ++cy)
            for (int cx = cx1; cx <= cx2; ++cx) {
                const auto& cell = m_cells[static_cast<std::size_t>((cy << m_gridRowShift) + cx)];
                for (std::size_t i = 0; i < cell.size(); ++i)
                    if (!fn(cell[i])) return;
            }
    }
    // Same visit, collecting first (for callbacks that create or destroy sprites).
    std::vector<Sprite*> collectInRect(int x1, int y1, int x2, int y2) {
        std::vector<Sprite*> out;
        forEachInRect(x1, y1, x2, y2, [&](Sprite* s) { out.push_back(s); return true; });
        return out;
    }
    [[nodiscard]] int gridShiftX() const noexcept { return m_gridShiftX; }
    [[nodiscard]] int gridShiftY() const noexcept { return m_gridShiftY; }
    // Contents of one cell (nullptr outside the grid), in insertion order (tests).
    [[nodiscard]] const std::vector<Sprite*>* gridCell(int cx, int cy) const noexcept {
        if (cx < 0 || cy < 0 || cx >= m_gridCols || cy >= m_gridRows) return nullptr;
        return &m_cells[static_cast<std::size_t>((cy << m_gridRowShift) + cx)];
    }
    // Map bounds without rebuilding the grid (tests: the oracle vectors vary them per case).
    void setMapSize(int w, int h) noexcept { m_mapW = w; m_mapH = h; }
    // Global 0x439d4c: the distance left by the last GetDirection.
    [[nodiscard]] int lastDistance() const noexcept { return m_lastDist; }
    void setLastDistance(int d) noexcept { m_lastDist = d; }

    void gridAdd(Sprite* s) {   // 0x40d870
        cellAdd(s->x >> m_gridShiftX, s->y >> m_gridShiftY, s);
        if (vidOf(*s).category & (cat::Vehicle | cat::Avia)) m_gridUnits.push_back(s);
    }
    void gridRemove(Sprite* s) {   // 0x40d8f0
        cellRemove(s->x >> m_gridShiftX, s->y >> m_gridShiftY, s);
        if (vidOf(*s).category & (cat::Vehicle | cat::Avia)) {
            auto it = std::ranges::find(m_gridUnits, s);
            if (it != m_gridUnits.end()) { *it = m_gridUnits.back(); m_gridUnits.pop_back(); }
        }
    }
    void gridMoveTo(Sprite& s, int x, int y) {   // act_changecoor (0x40db78)
        const int ocx = s.x >> m_gridShiftX, ocy = s.y >> m_gridShiftY;
        s.x = static_cast<std::int16_t>(x);
        s.y = static_cast<std::int16_t>(y);
        const int cx = s.x >> m_gridShiftX, cy = s.y >> m_gridShiftY;
        if (cx == ocx && cy == ocy) return;
        cellRemove(ocx, ocy, &s);
        cellAdd(cx, cy, &s);
    }

    // ------------------------------------------------------------------ destruction
    // Base act_destroy (0x40dac1): count, grid, commands, list, sub-object, free.
    void destroyBase(Sprite& s) {
        VidInfo& v = vidOf(s);
        if (v.count) v.count--;
        if (v.flags & vf::PresentOnGrid) gridRemove(&s);
        s.commands.reset();
        listRemove(&s);
        if (Sprite* c = s.child) { s.child = nullptr; action(*c, act::destroy, 0, 0); }
        freeSprite(&s);
    }

    // ------------------------------------------------------------------ dispatch
    // Defined in world_actions.hpp.
    int action(Sprite& s, int act, int var1, int var2);

    [[nodiscard]] static bool hasMaterial(std::uint8_t c) noexcept {
        return c != cls::Superstructure && c != cls::Image && c != cls::Effect2 && c != cls::Font;
    }
    [[nodiscard]] static bool isUnitClass(std::uint8_t c) noexcept {
        return c == cls::Vehicle || c == cls::Building || c == cls::Avia || c == cls::Mine || c == cls::Kassandra;
    }

private:
    // class actions (world_actions.hpp)
    int actionBase(Sprite& s, int a, int v1, int v2);
    int actionMaterial(Sprite& s, int a, int v1, int v2);
    int actionVehicle(Sprite& s, int a, int v1, int v2);
    int actionBuilding(Sprite& s, int a, int v1, int v2);
    int actionAvia(Sprite& s, int a, int v1, int v2);
    int actionProjectile(Sprite& s, int a, int v1, int v2);
    int actionCannonMissile(Sprite& s, int a, int v1, int v2);
    int actionDownedAvia(Sprite& s, int a, int v1, int v2);
    int actionNA(Sprite& s, int a, int v1, int v2);
    int actionImage(Sprite& s, int a, int v1, int v2);
    int actionEffect1(Sprite& s, int a, int v1, int v2);
    int actionEffect2(Sprite& s, int a, int v1, int v2);
    int actionMine(Sprite& s, int a, int v1, int v2);
    int actionShell(Sprite& s, int a, int v1, int v2);
    int actionAviaMissile(Sprite& s, int a, int v1, int v2);
    int actionDebris(Sprite& s, int a, int v1, int v2);
    int actionKassandra(Sprite& s, int a, int v1, int v2);
    int actionBonus(Sprite& s, int a, int v1, int v2);
    int actionFont(Sprite& s, int a, int v1, int v2);
    int actionRepairCannon(Sprite& s, int a, int v1, int v2);
    void vehicleNextCommand(Sprite& s);
    void removeAttackCommands(Sprite& s, UnitId t);
    void unitDestroyCommon(Sprite& s);

    static void initVidInfo(VidInfo& out, const Vid& v) {
        out.src = &v;
        if (const auto* g = std::get_if<Vid::Graphics>(&v.graphicsData); g && *g) out.gfx = g->get();
        out.category = static_cast<std::uint8_t>(v.category);
        out.cls = static_cast<std::uint8_t>(v.type);
        out.flags = static_cast<std::uint16_t>(v.flags);
        out.collisionMask = v.collisionMask;
        out.sizeX = static_cast<std::int16_t>(v.sizeX);
        out.sizeY = static_cast<std::int16_t>(v.sizeY);
        out.sizeZ = static_cast<std::int16_t>(v.sizeZ);
        out.halfX = static_cast<std::int16_t>(out.sizeX >> 1);
        out.halfY = static_cast<std::int16_t>(out.sizeY >> 1);
        out.absSizeZ = static_cast<std::int16_t>(std::abs(static_cast<int>(out.sizeZ)));
        out.maxHP = v.maxHP;
        out.visibilityRadius = static_cast<std::int16_t>(v.visibilityRadius);
        out.speedX = static_cast<std::int16_t>(v.speedX);
        out.speedY = static_cast<std::int16_t>(v.speedY);
        out.acceleration = v.acceleration;
        out.dirs = v.directionsCount ? v.directionsCount : 1;
        // 0x406705; dirs == 0 divides by zero in the original (D10: the raw value is kept)
        out.rotationPeriod = v.directionsCount
            ? static_cast<std::int8_t>(static_cast<std::int8_t>(v.rotationPeriod) * 6 / v.directionsCount)
            : static_cast<std::int8_t>(v.rotationPeriod);
        out.army = v.army;
        out.weapon = v.someWeaponIndex;
        out.deathDamageRadius = static_cast<std::int16_t>(v.deathDamageRadius);
        out.deathDamage = v.deathDamage;
        out.linkX = v.linkX; out.linkY = v.linkY; out.linkZ = v.linkZ;
        out.linkedObjectVid = v.linkedObjectVid;
        out.zLayer = v.z_layer < 16 ? v.z_layer : 15;
        out.dirStep = static_cast<std::uint8_t>(256 / out.dirs);
        out.animLen = v.animationLengths;
        out.nsfx = v.nsfx;
        out.childOff = v.childrenOffsets;
        out.childNvid = v.childNvid;
        out.childCount = v.childrenCount;
        if (out.gfx) {
            out.frameDuration = out.gfx->frameDuration;
            out.numFrames = out.gfx->numOfFrames;
            out.width = out.gfx->width;
            out.height = out.gfx->height;
        }
    }

    Sprite* allocSprite(std::uint8_t cls) {
        Sprite* s;
        if (!m_free.empty()) { s = m_free.back(); m_free.pop_back(); *s = Sprite{}; }
        else { m_pool.push_back(std::make_unique<Sprite>()); s = m_pool.back().get(); }
        s->cls = cls;
        s->alive = true;
        m_byId.push_back(s);
        s->id = static_cast<UnitId>(m_byId.size());
        ++m_live;
        return s;
    }
    void freeSprite(Sprite* s) {
        if (!s->alive) return;
        s->alive = false;
        if (s->id > 0 && static_cast<std::size_t>(s->id) <= m_byId.size()) m_byId[static_cast<std::size_t>(s->id) - 1] = nullptr;
        s->commands.reset();
        s->items.clear();
        m_pendingFree.push_back(s);   // reused from the next World::tact (no reuse within a frame)
        --m_live;
    }

    // z-layer lists: 0x40bcf0 (push front), 0x40bd40 (sorted by y, z, x), 0x40be60 (remove).
    void pushFront(Sprite* s) {
        const int l = vidOf(*s).zLayer;
        s->layer = static_cast<std::uint8_t>(l);
        s->next = m_layers[l];
        s->prev = nullptr;
        if (s->next) s->next->prev = s;
        m_layers[l] = s;
    }
    void insertSorted(Sprite* s) {
        const int l = vidOf(*s).zLayer;
        s->layer = static_cast<std::uint8_t>(l);
        Sprite* h = m_layers[l];
        if (!h || h->y > s->y || (h->y == s->y && h->z > s->z) || (h->y == s->y && h->z == s->z && h->x >= s->x)) {
            s->next = h;
            s->prev = nullptr;
            if (h) h->prev = s;
            m_layers[l] = s;
            return;
        }
        Sprite* prev = h;
        Sprite* cur = h->next;
        while (cur && cur->y < s->y) { prev = cur; cur = cur->next; }
        while (cur && cur->y == s->y && cur->z < s->z) { prev = cur; cur = cur->next; }
        while (cur && cur->y == s->y && cur->z == s->z && cur->x < s->x) { prev = cur; cur = cur->next; }
        s->next = prev->next;
        s->prev = prev;
        if (s->next) s->next->prev = s;
        prev->next = s;
        if (m_iterNext == s->next) m_iterNext = s;   // 0x40be3f (also at the tail: the new sprite is visited)
    }
    void listRemove(Sprite* s) {
        if (m_iterNext == s) m_iterNext = s->next;
        if (s->layer >= MaxLayers) return;   // not in a list (sub-object chain only)
        const int l = s->layer;
        if (s->prev) s->prev->next = s->next;
        else m_layers[l] = s->next;
        if (s->next) s->next->prev = s->prev;
        s->next = s->prev = nullptr;
        s->layer = 0xff;
    }

public:
    // Effect2 act_child needs to take a sprite out of the lists.
    void detachFromList(Sprite* s) { listRemove(s); }

private:
    void initGrid() {   // 0x40d580
        int mx = 0, my = 0;
        for (const auto& v : m_vids)
            if (v.flags & vf::PresentOnGrid) { mx = std::max<int>(mx, v.sizeX); my = std::max<int>(my, v.sizeY); }
        mx >>= 1; my >>= 1;
        m_gridShiftX = 0;
        if (mx > 1) do { ++m_gridShiftX; } while ((1 << m_gridShiftX) < mx);
        m_gridShiftY = 0;
        if (my > 1) do { ++m_gridShiftY; } while ((1 << m_gridShiftY) < my);
        m_gridRows = ((m_mapH - 1) >> m_gridShiftY) + 1;
        const int cols = ((m_mapW - 1) >> m_gridShiftX) + 1;
        m_gridCols = cols;
        m_gridRowShift = 0;
        if (cols > 1) do { ++m_gridRowShift; } while ((1 << m_gridRowShift) < cols);
        m_gridStride = 1 << m_gridRowShift;
        m_cells.assign(static_cast<std::size_t>(m_gridRows) * static_cast<std::size_t>(m_gridStride), {});
        m_gridUnits.clear();
    }
    void cellAdd(int cx, int cy, Sprite* s) {
        if (cx < 0 || cy < 0 || cx >= m_gridCols || cy >= m_gridRows) return;
        m_cells[static_cast<std::size_t>((cy << m_gridRowShift) + cx)].push_back(s);
    }
    void cellRemove(int cx, int cy, Sprite* s) {
        if (cx < 0 || cy < 0 || cx >= m_gridCols || cy >= m_gridRows) return;
        auto& cell = m_cells[static_cast<std::size_t>((cy << m_gridRowShift) + cx)];
        for (auto& p : cell)
            if (p == s) { p = cell.back(); cell.pop_back(); return; }
    }

    std::vector<VidInfo> m_vids;
    std::vector<Weapon> m_weapons;
    fx::PerspectiveTables m_persp;
    MsvcRand m_rand;

    std::uint32_t m_clock = 0;
    bool m_paused = false;
    std::uint32_t m_pauseStart = 0;
    std::int16_t m_windStrength = 0;
    std::uint8_t m_windDir = 0x40;
    int m_mapW = 0, m_mapH = 0, m_scaleX = 24, m_scaleY = 16;
    int m_camX = 0, m_camY = 0, m_viewW = 640, m_viewH = 480;
    int m_lastDist = 0;   // global 0x439d4c

    std::array<Sprite*, MaxLayers> m_layers{};
    Sprite* m_iterNext = nullptr;   // 0x439d48
    std::vector<std::unique_ptr<Sprite>> m_pool;
    std::vector<Sprite*> m_free, m_pendingFree;
    std::vector<Sprite*> m_byId;
    std::size_t m_live = 0;

    // orphan chain of a dead Effect2 (0x431ce4, 0x42e590, 0x431ce1, 0x4306a4)
    Sprite* m_orphan = nullptr;
    std::int8_t m_orphanDx = 0, m_orphanDy = 0, m_orphanDz = 0;

    int m_gridShiftX = 0, m_gridShiftY = 0, m_gridRows = 0, m_gridCols = 0, m_gridRowShift = 0, m_gridStride = 1;
    std::vector<std::vector<Sprite*>> m_cells;
    std::vector<Sprite*> m_gridUnits;

    std::array<Player, MaxPlayers> m_players{};
    std::vector<Group> m_groups;
    std::vector<SoundRequest> m_sounds;
    InputRecord m_localInput;
    bool m_netGame = false;   // 0x44e694: flagmen read their own player's record
};

} // namespace gromada::game

#include "game/world_actions.hpp"
