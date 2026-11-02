#pragma once
// gromada::game::GameEngine: the simulation engine of gromada.exe (issue #8) behind the
// game::Engine contract. It owns the World (sprites, lists, grid, players, squads), the
// camera, drawing and the 28 engine builtins. docs/re/engine.md is the specification,
// docs/engine.md describes this implementation and its intentional divergences.
// Header-only; include once per binary (it pulls libgromada's implementations).

#include "core/std.hpp"
#include <cstring>
#include <unordered_map>
#include "game/engine_api.hpp"
#include "game/hicolor.hpp"
#include "game/screen_effects.hpp"
#include "game/world.hpp"
#include "libgromada/libgromada_all.hpp"

namespace gromada::game {


class GameEngine final : public Engine {
public:
    friend struct SaveStateAccess; // save states (game/savestate.hpp)
    explicit GameEngine(const GameResources& resources) : m_res(&resources), m_world(resources) {
        m_palettes.resize(m_world.vids().size());
    }
    // Without fw.res (tests): only loadMap(const Map&) and loadEmptyMap() are usable.
    GameEngine(std::span<const Vid> vids, std::span<const Weapon> weapons) : m_world(vids, weapons) {
        m_palettes.resize(m_world.vids().size());
    }

    // ------------------------------------------------------------ map lifecycle
    void loadMap(const std::filesystem::path& mapFile, std::uint32_t rngSeed) override {
        if (!m_res) throw std::logic_error("GameEngine::loadMap: built without GameResources");
        const Map map = ::loadMap(m_res->vids(), mapFile);
        loadMap(map, rngSeed);
    }
    void loadMap(const Map& map, std::uint32_t rngSeed) {
        m_world.load(map, rngSeed);
        m_header = map.header;
        m_camera.x = map.header.observerX;
        m_camera.y = map.header.observerY;
        m_scrollVX = m_scrollVY = 0;
        m_autoScroll = false;
        m_scrollLock = false;
        m_lights.clear();   // CreateLight records (never drawn by the original either)
        m_frameLocked = false;
        m_turretClock = m_world.clock();
        m_iterUnit = -1;
        m_loaded = true;
    }
    void loadEmptyMap(int width, int height, std::uint32_t seed) {
        Map map;
        map.header.width = static_cast<std::uint32_t>(width);
        map.header.height = static_cast<std::uint32_t>(height);
        for (auto& a : map.armies) a = Army{};
        map.armies[0].a = 1;
        loadMap(map, seed);
    }
    void unloadMap() override {
        m_world.clear();
        setGlobalGamma(0x777);   // restored at every unload (0x413543)
        m_loaded = false;
    }

    // ------------------------------------------------------------ frame phases
    void beginFrame(int elapsedMs) override {
        // A frame that was not drawn still steps Effect() (EndFrame with draw == 0).
        if (!std::exchange(m_effectsDone, false) && m_loaded) m_effects.endFrame(std::nullopt, m_world.clock(), m_frameLocked);
        m_realElapsed = std::max(0, elapsedMs);
        m_world.advanceClock(elapsedMs);
    }
    void setInput(const OriginalInput& in) override {
        m_input = in;
        InputRecord& r = m_world.localInput();
        r.mouseWorldX = in.mouseWorldX;
        r.mouseWorldY = in.mouseWorldY;
        r.mouseScreenX = in.mouseScreenX;
        r.mouseScreenY = in.mouseScreenY;
        r.bits = in.bits;
        r.key = in.key;
        for (auto& p : m_world.players()) p.input = r;
    }
    void tactWorld() override {
        m_world.setCamera(m_camera.x, m_camera.y, m_camera.width, m_camera.height);
        m_world.tact();
    }
    void tactPlayers() override {
        if (m_world.paused()) return;
        m_turretSteps = turretSteps();
        for (auto& p : m_world.players()) playerTact(p);
    }
    void tactGroupAi() override {
        if (m_world.paused()) return;
        m_world.groupsTact();
    }
    void updateCamera() override {
        scrollCamera();
        clampCamera();
    }

    void draw(FramebufferRef fb) override;

    // ------------------------------------------------------------ queries
    [[nodiscard]] int clock() const override { return static_cast<int>(m_world.clock()); }
    [[nodiscard]] Camera& camera() override { return m_camera; }
    [[nodiscard]] CursorKind cursorAt(int screenX, int screenY) const override;
    [[nodiscard]] PlayerAim playerAim() const override;
    [[nodiscard]] std::vector<SoundEvent> takeSounds() override {
        std::vector<SoundEvent> out;
        auto& s = m_world.sounds();
        out.reserve(s.size());
        for (const auto& r : s) out.push_back(SoundEvent{r.nsfx, r.x, r.y});
        s.clear();
        return out;
    }
    std::optional<script::Value> callBuiltin(int id, std::span<const script::Value> args) override;
    int rand() override { return m_world.rand(); }

    // ScrollLock(flag) is a session builtin that acts on the engine's camera.
    void setScrollLock(bool on) override { m_scrollLock = on; }
    // Demo playback: the recorded initial clock. Sprites created by loadMap() keep ticking
    // from the new clock (their lastTact is moved with it).
    // The clock then comes from a recording: the original's per-frame behaviours (turret
    // turning, eff_openwindow) run once per recorded frame instead of being paced on time.
    void setClock(std::uint32_t c) override {
        const std::uint32_t old = m_world.clock();
        m_world.setClock(c);
        m_frameLocked = true;
        m_turretClock = c;
        for (int l = 0; l < World::MaxLayers; ++l)
            for (Sprite* s = m_world.layerHead(l); s; s = s->next)
                for (Sprite* k = s; k; k = k->child) k->lastTact += c - old;
    }

    std::optional<StateBarInfo> stateBarInfo() override;

    // ------------------------------------------------------------ extras (tools, tests)
    World& world() noexcept { return m_world; }
    const World& world() const noexcept { return m_world; }
    [[nodiscard]] int globalGamma() const noexcept { return m_gamma; }
    [[nodiscard]] int effect() const noexcept { return m_effects.effect(); }
    [[nodiscard]] const ScreenEffects& screenEffects() const noexcept { return m_effects; }
    // Effect() on the finished picture (menus, texts and status bar included), 0x406ea0.
    void drawScreenEffects(FramebufferRef fb) override {
        if (!m_loaded) return;
        m_effects.endFrame(fb, m_world.clock(), m_frameLocked);
        m_effectsDone = true;
    }
    [[nodiscard]] bool autoScroll() const noexcept { return m_autoScroll; }

private:
    static int argInt(std::span<const script::Value> a, std::size_t i) { return i < a.size() ? a[i].toInt() : 0; }
    Sprite* unit(int id) const { return m_world.byId(id); }

    void playerTact(Player& p);
    int turretSteps();
    void turnTurret(Sprite& head, int dir) {
        for (int i = 0; i < m_turretSteps; ++i)
            if (m_world.turnStep(head, dir)) break;
    }
    void scrollCamera();
    void clampCamera();
    void drawSprite(const Sprite& s, FramebufferRef fb);
    const std::array<ColorRgb8, 256>* paletteFor(const Sprite& s);
    const VidInfo& graphicsOwner(int nvid) const;
    void setGlobalGamma(int g) {
        if (g == m_gamma) return;
        m_gamma = g;
        for (auto& p : m_palettes) p.reset();
    }
    void saveMapFile(const std::string& name);

    const GameResources* m_res = nullptr;
    World m_world;
    MapHeaderRawData m_header;
    Camera m_camera;
    OriginalInput m_input;
    bool m_loaded = false;
    int m_realElapsed = 0;
    int m_scrollVX = 0, m_scrollVY = 0;
    bool m_autoScroll = false, m_scrollLock = false;
    int m_gamma = 0x777;
    ScreenEffects m_effects;
    bool m_effectsDone = true;
    bool m_frameLocked = false;          // demo playback: one original frame per frame
    std::uint32_t m_turretClock = 0;     // turret pacing (D9)
    int m_turretSteps = 1;
    struct Light { int x, y, z, color; };
    std::vector<Light> m_lights;
    int m_iterUnit = -1, m_iterArmy = 0, m_iterType = 0;
    std::string m_commandsBuffer;
    // palettes with the global gamma, per vid (built on first use)
    std::vector<std::optional<std::array<ColorRgb8, 256>>> m_palettes;
    struct SortItem { const Sprite* s; int index; };
    std::vector<SortItem> m_sorted;
};

// ---------------------------------------------------------------------------- players
// 0x4169b0 Player::Tact: cursor, left click = move, right click = fire, turret follows the
// mouse (one direction step per original frame, see turretSteps()).
inline void GameEngine::playerTact(Player& p) {
    if (p.type != 1) return;
    Sprite* f = m_world.byId(p.flagman);
    if (!f) { p.flagman = 0; return; }
    VidInfo& fv = m_world.vidOf(*f);
    fv.weapon = p.weapon;
    const int mx = p.input.mouseWorldX, my = p.input.mouseWorldY;
    Sprite* under = m_world.unitAt(mx, my);
    p.underCursor = World::idOf(under);
    const auto bits = p.input.bits;
    if (bits & OriginalInput::LClick) {
        f->commands.reset();
        m_world.action(*f, act::move, mx, my);
        if (!f->alive) return;
    }
    Sprite* head = f->child;
    if ((bits & (OriginalInput::RClick | OriginalInput::RDown)) && head && head->action != act::fight &&
        head->action != act::load && head->action != act::unload) {
        m_world.action(*f, act::coor_attack, mx, my);
        if (!f->alive) return;
        head = f->child;
    }
    if (!m_world.vidOf(*f).linkedObjectVid || !head || head->action == act::fight) return;
    const Weapon& w = m_world.weaponOf(*f);
    if (under && !(m_world.vidOf(*under).category & static_cast<std::uint8_t>(w.targetCategory))) under = nullptr;
    // the head's own weapon decides the aiming (0x416b8b: flag 4; 0x416be1: anti-aircraft =
    // targetCategory & Avia, 80 px higher)
    const Weapon& hw = m_world.weaponOf(*head);
    if (hw.flags & 4) { turnTurret(*head, f->direction); return; }
    if (under) {
        turnTurret(*head, m_world.getDirection(f->x, f->y, mx, under->y).dir());
        return;
    }
    const int ty = my + ((static_cast<std::uint8_t>(hw.targetCategory) & cat::Avia) ? 80 : 0);
    turnTurret(*head, m_world.getDirection(f->x, f->y, mx, ty).dir());
}

// D9: Player::Tact turns the turret by one direction step per call (0x40b220), i.e. per frame
// of the original. Here the steps are paced on the original's mean frame time
// (ScreenEffects::ReferenceFrameMs of game clock per step, at most 2 per frame), so the turret
// turns at the same speed at 60 and 120 fps as gromada.exe at ~27 fps. Demo playback (recorded
// frames, setClock) keeps one step per frame.
inline int GameEngine::turretSteps() {
    if (m_frameLocked) return 1;
    constexpr std::uint32_t period = ScreenEffects::ReferenceFrameMs;
    const std::uint32_t now = m_world.clock();
    const auto elapsed = static_cast<std::int32_t>(now - m_turretClock);
    if (elapsed < 0 || elapsed > static_cast<std::int32_t>(4 * period)) {   // Pause(0) rewound the clock, or a long stall
        m_turretClock = now - (elapsed < 0 ? 0 : period);
        return elapsed < 0 ? 0 : 1;
    }
    const int steps = static_cast<int>(static_cast<std::uint32_t>(elapsed) / period);
    m_turretClock += static_cast<std::uint32_t>(steps) * period;
    return std::min(steps, 2);
}

// StateBar::Tact 0x416de0 (called by Player::Tact for the local player): flagman hp and
// ammo, the weapon head, the radar dots (grid objects within +-464 px, 0x40d6d0) and the
// nearest enemy (grid unit list, army 1, distance <= 60000, 0x417209).
inline std::optional<StateBarInfo> GameEngine::stateBarInfo() {
    if (!m_loaded || m_world.paused()) return std::nullopt;
    const Player& p = m_world.players()[0];
    Sprite* f = p.type == 1 ? m_world.byId(p.flagman) : nullptr;
    if (!f) return std::nullopt;
    StateBarInfo out;
    out.hp = f->hp;
    out.maxHp = m_world.vidOf(*f).maxHP;
    if (const Sprite* c = f->child) {
        out.turretNvid = c->nvid;
        if (const Vid* src = m_world.vidOf(*c).src) out.turretName.assign(src->name.data(), strnlen(src->name.data(), src->name.size()));
    }
    for (std::size_t i = 0; i < out.ammo.size(); ++i) out.ammo[i] = f->ammo[i];
    // Colours initialised once by 0x416e16 (0x405560) and the grey of SetMode (+0x1a82).
    constexpr auto own = hicolor::fromRgb8(0x53, 0xa5, 0x36), avia1 = hicolor::fromRgb8(0x38, 0x91, 0xa5),
                   avia = hicolor::fromRgb8(0x23, 0x58, 0x64), mine = hicolor::fromRgb8(0xc8, 0x32, 0x32),
                   missile = hicolor::fromRgb8(0xc8, 0xc8, 0x32);
    m_world.forEachInRect(f->x - 0x1d0, f->y - 0x1d0, f->x + 0x1d0, f->y + 0x1d0, [&](Sprite* o) {
        if (o == f) return true;
        const VidInfo& v = m_world.vidOf(*o);
        std::uint16_t c = 0;
        if (v.cls == cls::AviaMissile) c = missile;
        else if (v.category & cat::Avia) c = v.army == 1 ? avia1 : avia;
        else if (v.cls == cls::Mine) c = mine;
        else if (v.category & cat::Vehicle) c = v.army == 1 ? own : hicolor::Grey;
        if (c) out.radar.push_back({o->x - f->x, o->y - f->y, c});
        return true;
    });
    int best = 60001;
    const Sprite* nearest = nullptr;
    for (const Sprite* u : m_world.gridUnits()) {
        const VidInfo& v = m_world.vidOf(*u);
        if (!(v.category & (cat::Vehicle | cat::Avia)) || u == f || v.army != 1) continue;   // single player: no net Kassandra
        const int d = World::distance(*f, *u);
        if (d <= 60000 && d < best) { best = d; nearest = u; }
    }
    if (nearest) out.enemyDirection = fx::getDirection(f->x, f->y, nearest->x, nearest->y).dir() & 0xff;
    return out;
}

inline CursorKind GameEngine::cursorAt(int screenX, int screenY) const {
    const Player& p = const_cast<World&>(m_world).players()[0];
    Sprite* f = m_world.byId(p.flagman);
    if (!f || p.type != 1) return CursorKind::Arrow;
    const int wx = screenX + m_camera.x, wy = screenY + m_camera.y;
    auto& w = const_cast<World&>(m_world);
    if (Sprite* u = w.unitAt(wx, wy); u && f->army != w.vidOf(*u).army) return CursorKind::Fight;
    if (w.askPlace(*f, wx, wy, f->z)) return CursorKind::NoMove;
    return CursorKind::Move;
}

// Controls query (issue #16): the units FindTarget (0x40c970) would accept for the flagman's
// current weapon, wherever they are, with the point that selects them under the cursor
// (unitAt's box centre), so that a right click there attacks them.
inline PlayerAim GameEngine::playerAim() const {
    PlayerAim out;
    auto& w = const_cast<World&>(m_world);
    const Player& p = w.players()[0];
    Sprite* f = w.byId(p.flagman);
    if (!f || p.type != 1 || !f->alive) return out;
    out.valid = true;
    out.flagmanX = f->x;
    out.flagmanY = f->y;
    const Weapon& wp = w.weaponOf(*f);
    out.weaponRange = static_cast<std::int16_t>(wp.weaponRange);
    const auto tcat = static_cast<std::uint8_t>(wp.targetCategory);
    const int myArmy = w.action(*f, act::getarmy, 0, 0);
    for (Sprite* u : w.gridUnits()) {
        if (u == f || !u->alive || (u->state & st::Invisible)) continue;
        const VidInfo& uv = w.vidOf(*u);
        if (!(uv.category & tcat)) continue;
        const int ua = w.action(*u, act::getarmy, 0, 0);
        if (ua == 2 || ua == myArmy) continue;
        out.targets.push_back({World::idOf(u), u->x, u->y - u->z - (uv.sizeZ >> 1), u->x, u->y, u->hp});
    }
    return out;
}

// ---------------------------------------------------------------------------- camera
// 0x415e8d edge scroll and auto-scroll, 0x416004 leash around the flagman.
inline void GameEngine::scrollCamera() {
    const int w = m_camera.width, h = m_camera.height;
    Sprite* f = m_world.flagman(0);
    if (!m_scrollLock) {
        const int sx = m_input.mouseScreenX, sy = m_input.mouseScreenY;
        if (sx <= 5) { if (m_scrollVX > -500) m_scrollVX -= 40; }
        else if (sx >= w - 6) { if (m_scrollVX < 500) m_scrollVX += 40; }
        else m_scrollVX = 0;
        if (sy <= 5) { if (m_scrollVY > -400) m_scrollVY -= 30; }
        else if (sy >= h - 6) { if (m_scrollVY < 400) m_scrollVY += 30; }
        else m_scrollVY = 0;
        if (f && m_autoScroll && m_scrollVX == 0 && m_scrollVY == 0) {
            m_scrollVX = f->x - (w >> 1) - m_camera.x;
            m_scrollVY = f->y - (h >> 1) - m_camera.y;
        }
        const int dt = m_realElapsed;
        m_camera.x = static_cast<std::int16_t>(m_camera.x + ((dt * m_scrollVX) >> 10));
        m_camera.y = static_cast<std::int16_t>(m_camera.y + ((dt * m_scrollVY) >> 10));
        if (f) {
            // 586 x 440 = a 640 x 480 view minus 54 x 40: the flagman stays at least 54 x 40 px
            // inside the view. Wider views (16:9) keep the same margin (issue #16).
            const int lx = std::max(586, w - 54), ly = std::max(440, h - 40);
            if (m_camera.x < f->x - lx) m_camera.x = f->x - lx;
            if (m_camera.y < f->y - ly) m_camera.y = f->y - ly;
            if (m_camera.x + w > f->x + lx) m_camera.x = f->x - w + lx;
            if (m_camera.y + h > f->y + ly) m_camera.y = f->y - h + ly;
        }
    }
}

// 0x414838: keep the view inside the map.
inline void GameEngine::clampCamera() {
    if (!m_loaded) return;
    if (m_camera.x < 0) m_camera.x = 0;
    if (m_camera.y < 0) m_camera.y = 0;
    if (m_camera.x > m_world.mapWidth() - m_camera.width - 1) m_camera.x = m_world.mapWidth() - m_camera.width - 1;
    if (m_camera.y > m_world.mapHeight() - m_camera.height - 1) m_camera.y = m_world.mapHeight() - m_camera.height - 1;
}

// ---------------------------------------------------------------------------- drawing
// The vid whose graphics a vid uses (0x130 chain, 0x402346): its flags decide the gamma.
inline const VidInfo& GameEngine::graphicsOwner(int nvid) const {
    const auto& vids = m_world.vids();
    const VidInfo* v = &vids[static_cast<std::size_t>(nvid)];
    for (int guard = 0; guard < 8 && v->src; ++guard) {
        const auto* from = std::get_if<std::int32_t>(&v->src->graphicsData);
        if (!from || *from < 0 || *from >= static_cast<int>(vids.size())) break;
        v = &vids[static_cast<std::size_t>(*from)];
    }
    return *v;
}

// The palette a sprite is drawn with: an alternative palette (st_gamma, 0x40b660) or the
// graphics' 16-bit palette with the global gamma (0x406830), as displayed 565 colours.
inline const std::array<ColorRgb8, 256>* GameEngine::paletteFor(const Sprite& s) {
    const VidInfo& v = m_world.vidOf(s);
    if (!v.gfx) return nullptr;
    if (v.altPalettes) return &(*v.altPalettes)[(s.state >> 12) & 3];
    auto& cached = m_palettes[s.nvid];
    if (!cached)
        cached = hicolor::expand(hicolor::palette16(v.gfx->palette, m_gamma, (graphicsOwner(s.nvid).flags & vf::IgnoreGlobalGamma) != 0));
    return &*cached;
}

// 0x40b620 + 0x4021f0: one sprite. With vid flag 0x80 (Shadow) and z > 0 putcadr first draws
// the same frame as a cast shadow at (x - z/2 - z/4, y + z/4 - z/2), then the body (at y - z
// unless ZeroZ), whose own shadow pixels are then skipped (hicolor::FrameVisitor).
inline void GameEngine::drawSprite(const Sprite& s, FramebufferRef fb) {
    const VidInfo& v = m_world.vidOf(s);
    if (!v.gfx || s.frame < 0 || static_cast<std::size_t>(s.frame) >= v.gfx->frames.size()) return;
    const auto& frame = v.gfx->frames[static_cast<std::size_t>(s.frame)];
    if (frame.data.empty()) return;
    const auto* pal = paletteFor(s);
    if (!pal) return;
    int sx = s.x - (v.width >> 1), sy = s.y - (v.height >> 1);
    if (!(s.state & st::Screen)) { sx -= m_camera.x; sy -= m_camera.y; }
    int z = s.z;
    const bool cast = (v.flags & vf::Shadow) && z > 0;
    if (cast)
        hicolor::drawFrame(frame, sx - (z >> 1) - (z >> 2), sy + (z >> 2) - (z >> 1), *pal, fb, hicolor::FrameVisitor::Pass::CastShadow);
    if (v.flags & vf::ZeroZ) z = 0;
    hicolor::drawFrame(frame, sx, sy - z, *pal, fb, hicolor::FrameVisitor::Pass::Normal, cast);
}

// 0x40b880 draw half: layers 0..15 in list order, culling at +-512 px from the view centre,
// sub-objects with dz < 0 before the sprite and the others after it. Layer 5 (the unsorted
// layer of the units, head 0x439d0c) is re-sorted every frame: its visible sprites are
// inserted with InsertSorted (y, z, x; later insertions first on ties) into a temporary
// list that is drawn and then discarded, the update order is untouched.
inline void GameEngine::draw(FramebufferRef fb) {
    for (int y = 0; y < fb.extent(0); ++y)
        for (int x = 0; x < fb.extent(1); ++x) fb[y, x] = RGBA8{{0, 0, 0}, 255};
    const int cx = m_camera.x + (m_camera.width >> 1), cy = m_camera.y + (m_camera.height >> 1);
    const auto outside = [&](const Sprite* s) {
        return ((s->x - cx + 512) & ~0x3ff) || ((s->y - s->z - cy + 512) & ~0x3ff);
    };
    const auto drawWithChain = [&](const Sprite* s) {
        const auto chain = [&] {
            const Sprite* parent = s;
            for (const Sprite* c = s->child; c; parent = c, c = c->child) {
                if (m_world.vidOf(*parent).flags & vf::InvisibleSubobjects) break;
                drawSprite(*c, fb);
            }
        };
        if (s->dz < 0) chain();
        drawSprite(*s, fb);
        if (s->dz >= 0) chain();
    };
    for (int layer = 0; layer < World::MaxLayers; ++layer) {
        if (layer == 5) {
            m_sorted.clear();
            int index = 0;
            for (const Sprite* s = m_world.layerHead(5); s; s = s->next, ++index)
                if (!(s->state & (st::Invisible | st::Screen)) && !outside(s)) m_sorted.push_back({s, index});
            std::ranges::sort(m_sorted, [](const SortItem& a, const SortItem& b) {
                if (a.s->y != b.s->y) return a.s->y < b.s->y;
                if (a.s->z != b.s->z) return a.s->z < b.s->z;
                if (a.s->x != b.s->x) return a.s->x < b.s->x;
                return a.index > b.index;
            });
            for (const auto& it : m_sorted) drawWithChain(it.s);
            continue;
        }
        for (const Sprite* s = m_world.layerHead(layer); s; s = s->next) {
            if (s->state & st::Invisible) continue;
            if (!(s->state & st::Screen) && outside(s)) {
                if (s->y - cy >= 512) break;
                continue;
            }
            drawWithChain(s);
        }
    }
}

// ---------------------------------------------------------------------------- builtins
inline std::optional<script::Value> GameEngine::callBuiltin(int id, std::span<const script::Value> a) {
    using script::Value;
    switch (id) {
        case 65: {   // SizeTo(unit, x, y)
            Sprite* u = unit(argInt(a, 0));
            if (!u) return Value{60000};
            // the point is truncated to s16 (0x412bcc, docs/oracle.md §3.14)
            return Value{fx::gameDistance(u->x - static_cast<std::int16_t>(argInt(a, 1)),
                                          u->y - static_cast<std::int16_t>(argInt(a, 2)))};
        }
        case 67: {   // Action(unit, act, var1, var2)
            Sprite* u = unit(argInt(a, 0));
            if (!u) return Value{0};
            const int act = argInt(a, 1);
            if (act <= 15) { m_world.changeAction(*u, act, -1); return Value{0}; }
            return Value{m_world.action(*u, act, argInt(a, 2), argInt(a, 3))};
        }
        case 68: {   // Create(nvid, x, y, z)
            const int nvid = argInt(a, 0);
            if (nvid < 0 || nvid >= static_cast<int>(m_world.vids().size())) return Value{0};
            Sprite* s = m_world.createUnit(nvid, argInt(a, 1), argInt(a, 2), argInt(a, 3), 0, 0, nullptr);
            return Value{World::idOf(s)};
        }
        case 69: return Value{clock()};   // GetTime()
        case 79: {   // MaxUnit(nvid, max)
            const int nvid = argInt(a, 0);
            if (nvid >= 0 && nvid < static_cast<int>(m_world.vids().size())) m_world.vid(nvid).maxUnit = argInt(a, 1);
            return Value{0};
        }
        case 80:   // Effect(effect): 0x406e40, drawn by drawScreenEffects()
            m_effects.start(argInt(a, 0), [this] { return m_world.rand(); });
            return Value{0};
        case 85: {   // Flagman(army)
            const int army = argInt(a, 0);
            if (army < 0 || army >= World::MaxPlayers) return Value{0};
            return Value{World::idOf(m_world.flagman(army))};
        }
        case 86:   // GetObject(nvid, x, y)
            return Value{World::idOf(m_world.getObject(argInt(a, 0), argInt(a, 1), argInt(a, 2)))};
        case 87: {   // AddCommand(unit, act, var1, var2)
            if (Sprite* u = unit(argInt(a, 0))) m_world.pushCommandBottom(*u, argInt(a, 1), argInt(a, 2), argInt(a, 3));
            return Value{0};
        }
        case 89: m_world.pause(argInt(a, 0) != 0); return Value{0};   // Pause(flag)
        case 90: {   // Random(max) = rand() % (max + 1)
            const int m = fx::wrapAdd(argInt(a, 0), 1);
            const int r = m_world.rand();
            return Value{m ? r % m : 0};
        }
        case 94: {   // GetUnitVid(unit)
            Sprite* u = unit(argInt(a, 0));
            return Value{u ? static_cast<int>(u->nvid) : 0};
        }
        case 95: {   // FirstUnit(narmy, typeunit): grid unit list from the last
            m_iterArmy = argInt(a, 0);
            m_iterType = argInt(a, 1);
            const auto& units = m_world.gridUnits();
            for (int i = static_cast<int>(units.size()) - 1; i >= 0; --i) {
                const VidInfo& v = m_world.vidOf(*units[static_cast<std::size_t>(i)]);
                if (v.army == m_iterArmy && (v.category & m_iterType)) { m_iterUnit = i; return Value{units[static_cast<std::size_t>(i)]->id}; }
            }
            m_iterUnit = -1;
            return Value{0};
        }
        case 96: {   // NextUnit()
            const auto& units = m_world.gridUnits();
            int i = m_iterUnit;
            if (i >= static_cast<int>(units.size())) i = static_cast<int>(units.size()) - 1;
            for (--i; i >= 0; --i) {
                const VidInfo& v = m_world.vidOf(*units[static_cast<std::size_t>(i)]);
                if (v.army == m_iterArmy && (v.category & m_iterType)) { m_iterUnit = i; return Value{units[static_cast<std::size_t>(i)]->id}; }
            }
            m_iterUnit = -1;
            return Value{0};
        }
        case 106: setGlobalGamma(argInt(a, 0)); return Value{0};   // SetGamma(g)
        case 107: m_world.setWind(argInt(a, 0), argInt(a, 1)); return Value{0};   // SetWind(wind, dir)
        case 108:   // CreateLight(x, y, z, color): renderer light (stored, not drawn yet)
            m_lights.push_back({argInt(a, 0), argInt(a, 1), argInt(a, 2), argInt(a, 3)});
            return Value{0};
        case 109: { Sprite* u = unit(argInt(a, 0)); return Value{u ? static_cast<int>(u->x) : 0}; }   // GetX
        case 110: { Sprite* u = unit(argInt(a, 0)); return Value{u ? static_cast<int>(u->y) : 0}; }   // GetY
        case 119: {   // SetFlagman(narmy, unit)
            const int army = argInt(a, 0);
            if (army >= 0 && army < World::MaxPlayers) m_world.players()[static_cast<std::size_t>(army)].flagman = World::idOf(unit(argInt(a, 1)));
            return Value{0};
        }
        case 120:   // Save(filename): the current map as a .map file
            saveMapFile(a.empty() ? std::string{} : a[0].toString());
            return Value{0};
        case 121: {   // GetCommands(unit)
            if (Sprite* u = unit(argInt(a, 0))) m_commandsBuffer = m_world.getCommands(*u);
            return Value{m_commandsBuffer};
        }
        case 122: {   // SetCommands(unit, text)
            if (Sprite* u = unit(argInt(a, 0)); u && a.size() > 1) m_world.setCommands(*u, a[1].toString());
            return Value{0};
        }
        case 123:   // AskPlace(vid, x, y, z)
            return Value{World::idOf(m_world.askPlaceVid(argInt(a, 1), argInt(a, 2), argInt(a, 3), argInt(a, 0)))};
        case 124: {   // GetVidName(nvid)
            const int nvid = argInt(a, 0);
            if (nvid < 0 || nvid >= static_cast<int>(m_world.vids().size())) return Value{std::string{}};
            const auto& n = m_world.vid(nvid).src->name;
            return Value{std::string{n.data(), strnlen(n.data(), n.size())}};
        }
        case 126:   // SetPosition(x, y): centres the view
            m_camera.x = argInt(a, 0) - (m_camera.width >> 1);
            m_camera.y = argInt(a, 1) - (m_camera.height >> 1);
            return Value{0};
        case 127: {   // SetVid(nvid, type, var)
            const int nvid = argInt(a, 0), type = argInt(a, 1), var = argInt(a, 2);
            if (type == 23) {   // VID_WEAPONRANGE: nvid is a weapon index
                if (nvid >= 0 && nvid < static_cast<int>(m_world.weapons().size()))
                    m_world.weapons()[static_cast<std::size_t>(nvid)].weaponRange = static_cast<std::uint16_t>(var);
                return Value{0};
            }
            if (nvid < 0 || nvid >= static_cast<int>(m_world.vids().size())) return Value{0};
            VidInfo& v = m_world.vid(nvid);
            if (type == 1) v.maxHP = static_cast<std::uint8_t>(var);
            else if (type == 22) v.flags = static_cast<std::uint16_t>(var);
            else if (type >= 2 && type < 18) v.childNvid[static_cast<std::size_t>(type - 2)] = static_cast<std::int16_t>(var);
            else if (type >= 18 && type < 22 && v.gfx) {
                // VID_GAMMA+k (0x406d60): the palette of the graphics owner is reloaded (so it
                // carries the global gamma), copied into the 4 alternative palettes the first
                // time, and palette k becomes that copy with the gamma `var` (0x4072c0). Shared
                // by every vid that uses the same graphics.
                const auto base = hicolor::palette16(v.gfx->palette, m_gamma, (graphicsOwner(nvid).flags & vf::IgnoreGlobalGamma) != 0);
                auto alt = v.altPalettes;
                if (!alt) {
                    alt = std::make_shared<std::array<std::array<ColorRgb8, 256>, 4>>();
                    alt->fill(hicolor::expand(base));
                }
                auto k = base;
                hicolor::applyGamma(k, var);
                (*alt)[static_cast<std::size_t>(type - 18)] = hicolor::expand(k);
                for (auto& o : m_world.vids())
                    if (o.gfx == v.gfx) o.altPalettes = alt;
            }
            return Value{0};
        }
        case 128: m_autoScroll = argInt(a, 0) != 0; return Value{0};   // SetAutoScroll(flag)
        default: return std::nullopt;
    }
}

// Save(filename) (0x415620): writes the live objects as a .map (editor feature).
inline void GameEngine::saveMapFile(const std::string& name) {
    if (name.empty()) return;
    Map map;
    map.header = m_header;
    map.header.startTimer = m_world.clock();
    map.header.observerX = static_cast<std::int16_t>(m_camera.x);
    map.header.observerY = static_cast<std::int16_t>(m_camera.y);
    std::uint32_t nextId = 1;
    std::unordered_map<const Sprite*, std::uint32_t> ids;
    for (int layer = 0; layer < World::MaxLayers; ++layer)
        for (const Sprite* s = m_world.layerHead(layer); s; s = s->next) {
            if (s->state & (st::Screen | st::Sub)) continue;
            GameObject o{.nvid = s->nvid, .x = s->x, .y = s->y, .z = s->z, .direction = s->direction, .action = 0,
                         .payload = getPayloadPrototype(*m_world.vidOf(*s).src), .id = nextId};
            std::visit(overloaded{
                [](Payloads::VisualObject&) {},
                [&](Payloads::MaterialObject& p) { p.hp = s->hp; },
                [&](Payloads::AssetObject& p) {
                    p.hp = s->hp; p.buildTime = s->buildTime; p.army = s->army; p.behave = s->behave;
                    p.items = s->items;
                    if (s->commands)
                        for (int i = 0; i < s->commands->count; ++i) {
                            const auto& c = s->commands->e[static_cast<std::size_t>(i)];
                            p.commands.push_back({Action{c.act}, static_cast<std::uint32_t>(c.var1), static_cast<std::uint32_t>(c.var2)});
                        }
                },
            }, o.payload);
            ids[s] = nextId++;
            map.objects.push_back(std::move(o));
        }
    for (std::size_t a = 0; a < map.armies.size(); ++a) {
        const Player& p = m_world.players()[a];
        map.armies[a] = Army{p.type, p.b, p.netIndex, ids.contains(m_world.byId(p.flagman)) ? ids[m_world.byId(p.flagman)] : 0, {}};
    }
    std::ofstream out{name, std::ios::binary};
    std::vector<Vid> vids;
    vids.reserve(m_world.vids().size());
    for (const auto& v : m_world.vids()) vids.push_back(v.src ? *v.src : Vid{});
    if (out) ::saveMap(vids, map, out);
}

} // namespace gromada::game
