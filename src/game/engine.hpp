#pragma once
// gromada::game::GameEngine: the simulation engine of gromada.exe (issue #8) behind the
// game::Engine contract. It owns the World (sprites, lists, grid, players, squads), the
// camera, drawing and the 28 engine builtins. docs/re/engine.md is the specification,
// docs/engine.md describes this implementation and its intentional divergences.
// Header-only; include once per binary (it pulls libgromada's implementations).

#include "core/std.hpp"
#include "game/engine_api.hpp"
#include "game/world.hpp"
#include "libgromada/libgromada_all.hpp"

namespace gromada::game {

namespace detail {

// Palette with the global gamma of the map (0x4072c0, three nibbles 0xRGB, 7 = neutral,
// 0 = channel off, 15 = saturated, 1..6 darker, 8..14 lighter). The original works on the
// 16-bit palette with a lookup table; this is the same rule on 8-bit channels.
inline std::uint8_t gammaChannel(std::uint8_t c, int n) {
    if (n == 7) return c;
    if (n >= 15) return 255;
    if (n < 1) return 0;
    if (n < 7) return static_cast<std::uint8_t>(c * (n + 1) / 8);
    return static_cast<std::uint8_t>(std::min(255, c * 8 / (15 - n)));
}
inline void applyGamma(std::array<ColorRgb8, 256>& pal, int gamma) {
    const int r = (gamma >> 8) & 0xf, g = (gamma >> 4) & 0xf, b = gamma & 0xf;
    if (r == 7 && g == 7 && b == 7) return;
    for (auto& c : pal) c = {gammaChannel(c.r, r), gammaChannel(c.g, g), gammaChannel(c.b, b)};
}

// Draws a frame as a shadow: every opaque pixel darkens the destination (putcadr in shadow
// mode, 0x4021f0).
struct ShadowVisitor {
    int x0, y0;
    FramebufferRef fb;
    int x = 0, y = 0;
    [[nodiscard]] ClippingInfo begin_image(BoundingBox source_rect) noexcept {
        const BoundingBox dst{-x0, fb.extent(1) - x0, -y0, fb.extent(0) - y0};
        return {source_rect, dst};
    }
    void set_cursor(int cx, int cy) noexcept { x = x0 + cx; y = y0 + cy; }
    void advance_cursor(int n) noexcept { x += n; }
    void dark(int n) noexcept {
        if (y >= 0 && y < fb.extent(0)) {
            const int e = std::min(x + n, fb.extent(1));
            for (int i = std::max(x, 0); i < e; ++i) {
                auto& p = fb[y, i];
                p = RGBA8{{static_cast<std::uint8_t>(p.r >> 1), static_cast<std::uint8_t>(p.g >> 1),
                           static_cast<std::uint8_t>(p.b >> 1)}, 255};
            }
        }
        x += n;
    }
    void draw_pixels_shadow(int n) noexcept { advance_cursor(n); }
    void draw_pixels(std::span<const CompressedColor> c) noexcept { dark(static_cast<int>(c.size())); }
    void draw_pixels_indexed(std::span<const IndexedColor> c) noexcept { dark(static_cast<int>(c.size())); }
    void draw_pixels_repeat(int n, IndexedColor) noexcept { dark(n); }
    void draw_pixels_repeat(int n, CompressedColor) noexcept { dark(n); }
    void draw_pixels_light(int n, std::uint8_t, std::uint8_t, std::uint8_t) noexcept { advance_cursor(n); }
    void draw_pixels_alpha_blend(std::uint8_t, std::span<const std::byte> c) noexcept { advance_cursor(static_cast<int>(c.size())); }
};

} // namespace detail

class GameEngine final : public Engine {
public:
    explicit GameEngine(const GameResources& resources) : m_res(resources), m_world(resources) {
        m_palettes.resize(m_world.vids().size());
    }

    // ------------------------------------------------------------ map lifecycle
    void loadMap(const std::filesystem::path& mapFile, std::uint32_t rngSeed) override {
        const Map map = ::loadMap(m_res.vids(), mapFile);
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
        m_effect = 0;
        m_effectLevel = 0;
        m_lights.clear();
        m_iterUnit = -1;
        m_loaded = true;
    }
    void unloadMap() override {
        m_world.clear();
        setGlobalGamma(0x777);   // restored at every unload (0x413543)
        m_loaded = false;
    }

    // ------------------------------------------------------------ frame phases
    void beginFrame(int elapsedMs) override {
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
        advanceEffect();
    }
    void tactPlayers() override {
        if (m_world.paused()) return;
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
    void setClock(std::uint32_t c) override {
        const std::uint32_t old = m_world.clock();
        m_world.setClock(c);
        for (int l = 0; l < World::MaxLayers; ++l)
            for (Sprite* s = m_world.layerHead(l); s; s = s->next)
                for (Sprite* k = s; k; k = k->child) k->lastTact += c - old;
    }

    // ------------------------------------------------------------ extras (tools, tests)
    World& world() noexcept { return m_world; }
    const World& world() const noexcept { return m_world; }
    [[nodiscard]] int globalGamma() const noexcept { return m_gamma; }
    [[nodiscard]] int effect() const noexcept { return m_effect; }
    [[nodiscard]] bool autoScroll() const noexcept { return m_autoScroll; }

private:
    static int argInt(std::span<const script::Value> a, std::size_t i) { return i < a.size() ? a[i].toInt() : 0; }
    Sprite* unit(int id) const { return m_world.byId(id); }

    void playerTact(Player& p);
    void scrollCamera();
    void clampCamera();
    void advanceEffect();
    void applyEffect(FramebufferRef fb) const;
    void drawSprite(const Sprite& s, FramebufferRef fb);
    const std::array<ColorRgb8, 256>* paletteFor(const Sprite& s);
    void setGlobalGamma(int g) {
        if (g == m_gamma) return;
        m_gamma = g;
        for (auto& p : m_palettes) p.reset();
    }
    void saveMapFile(const std::string& name);

    const GameResources& m_res;
    World m_world;
    MapHeaderRawData m_header;
    Camera m_camera;
    OriginalInput m_input;
    bool m_loaded = false;
    int m_realElapsed = 0;
    int m_scrollVX = 0, m_scrollVY = 0;
    bool m_autoScroll = false, m_scrollLock = false;
    int m_gamma = 0x777;
    int m_effect = 0, m_effectLevel = 0;
    std::uint32_t m_effectClock = 0;
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
// mouse (one direction step per frame, like the original).
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
    if (w.flags & 4) { m_world.turnStep(*head, f->direction); return; }
    if (under) {
        m_world.turnStep(*head, fx::getDirection(f->x, f->y, mx, under->y).dir());
        return;
    }
    const int ty = my + ((w.flags & 8) ? 80 : 0);
    m_world.turnStep(*head, fx::getDirection(f->x, f->y, mx, ty).dir());
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
            if (m_camera.x < f->x - 586) m_camera.x = f->x - 586;
            if (m_camera.y < f->y - 440) m_camera.y = f->y - 440;
            if (m_camera.x + w > f->x + 586) m_camera.x = f->x - w + 586;
            if (m_camera.y + h > f->y + 440) m_camera.y = f->y - h + 440;
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

// ---------------------------------------------------------------------------- effects
// Effect(eff) (0x406e40/0x406ea0): the level rises by 2 every 71 ms of game clock up to 7,
// then fades from -32 back to 0. 1 = white flash, 2/3/4 = red/green/blue, 5 = fade.
inline void GameEngine::advanceEffect() {
    if (!m_effect || m_effect == 6) return;
    const std::uint32_t now = m_world.clock();
    while (now - m_effectClock >= 71) {
        m_effectClock += 71;
        if (m_effectLevel > 0) {
            m_effectLevel += 2;
            if (m_effectLevel > 6) m_effectLevel = -32;
        } else {
            m_effectLevel += 1;
            if (m_effectLevel >= 0) { m_effect = 0; m_effectLevel = 0; return; }
        }
    }
}

inline void GameEngine::applyEffect(FramebufferRef fb) const {
    if (!m_effect || m_effect == 6 || !m_effectLevel) return;
    const int lv = m_effectLevel > 0 ? m_effectLevel * 2 : (-m_effectLevel) >> 2;   // 0..14 / 0..8
    const int t = std::min(255, lv * 255 / 14);
    ColorRgb8 target{255, 255, 255};
    if (m_effect == 2) target = {255, 0, 0};
    else if (m_effect == 3) target = {0, 255, 0};
    else if (m_effect == 4) target = {0, 0, 255};
    else if (m_effect == 5) target = {0, 0, 0};
    for (int y = 0; y < fb.extent(0); ++y)
        for (int x = 0; x < fb.extent(1); ++x) {
            auto& p = fb[y, x];
            p.r = static_cast<std::uint8_t>((p.r * (255 - t) + target.r * t) / 255);
            p.g = static_cast<std::uint8_t>((p.g * (255 - t) + target.g * t) / 255);
            p.b = static_cast<std::uint8_t>((p.b * (255 - t) + target.b * t) / 255);
        }
}

// ---------------------------------------------------------------------------- drawing
inline const std::array<ColorRgb8, 256>* GameEngine::paletteFor(const Sprite& s) {
    const VidInfo& v = m_world.vidOf(s);
    if (!v.gfx) return nullptr;
    if (v.altPalettes) return &(*v.altPalettes)[(s.state >> 12) & 3];
    auto& cached = m_palettes[s.nvid];
    if (!cached) {
        cached = v.gfx->palette;
        if (m_gamma != 0x777 && v.gfx->dataFormat == 2 && !(v.flags & vf::IgnoreGlobalGamma)) detail::applyGamma(*cached, m_gamma);
    }
    return &*cached;
}

// 0x40b620 + 0x4021f0: one sprite, with its shadow.
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
    if ((v.flags & vf::Shadow) && z > 0) {
        const int shx = sx - (z >> 1) - (z >> 2), shy = sy + (z >> 2) - (z >> 1);
        if (!(shx + v.width <= 0 || shy + v.height <= 0 || shx >= fb.extent(1) || shy >= fb.extent(0)))
            DecodeFrame(frame, detail::ShadowVisitor{shx, shy, fb});
    }
    if (v.flags & vf::ZeroZ) z = 0;
    sy -= z;
    if (sx + v.width <= 0 || sy + v.height <= 0 || sx >= fb.extent(1) || sy >= fb.extent(0)) return;
    SoftwareRendererVisitor r{sx, sy, std::span<const ColorRgb8, 256>{*pal}, fb};
    DecodeFrame(frame, r);
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
    applyEffect(fb);
}

// ---------------------------------------------------------------------------- builtins
inline std::optional<script::Value> GameEngine::callBuiltin(int id, std::span<const script::Value> a) {
    using script::Value;
    switch (id) {
        case 65: {   // SizeTo(unit, x, y)
            Sprite* u = unit(argInt(a, 0));
            if (!u) return Value{60000};
            return Value{fx::gameDistance(u->x - argInt(a, 1), u->y - argInt(a, 2))};
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
        case 80: {   // Effect(effect)
            if (m_effect == 6) return Value{0};
            int e = argInt(a, 0);
            if (e == 255) e = m_world.rand() % 6 + 1;
            m_effect = e;
            m_effectLevel = 1;
            m_effectClock = m_world.clock();
            if (e == 6) m_effect = 0;   // eff_openwindow: transition of the session's frame (not emulated)
            return Value{0};
        }
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
            const int m = argInt(a, 0) + 1;
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
                // VID_GAMMA+k: alternative palette k of the graphics (0x406d60), shared by the
                // vids that use the same graphics.
                for (auto& o : m_world.vids())
                    if (o.gfx == v.gfx) {
                        if (!o.altPalettes) {
                            o.altPalettes = std::make_shared<std::array<std::array<ColorRgb8, 256>, 4>>();
                            o.altPalettes->fill(v.gfx->palette);
                        }
                    }
                auto alt = v.altPalettes;
                (*alt)[static_cast<std::size_t>(type - 18)] = v.gfx->palette;
                detail::applyGamma((*alt)[static_cast<std::size_t>(type - 18)], var);
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
    if (out) ::saveMap(m_res.vids(), map, out);
}

} // namespace gromada::game
