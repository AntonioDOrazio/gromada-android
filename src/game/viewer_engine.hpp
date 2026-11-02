#pragma once
// PROVISIONAL implementation of game::Engine (src/game/engine_api.hpp) used by the game
// session until the real simulation engine of issue #8 is merged. It is deliberately simple:
//  - the map's objects (and their linked sub-objects) become units with stable handles;
//    units animate, and Create/Action(act_destroy)/SetFlagman... work on them;
//  - no physics, collisions, weapons or AI: the flagman just drives towards the cursor while
//    the left button is held (or with the arrow keys), so a level can be looked around;
//  - game clock (71 ms clamp, Pause semantics of 0x40ac70), MSVC rand(), camera (SetPosition, edge scroll
//    of 0x415e20, auto-scroll, map clamp), SetGamma, a few Effect() screen effects;
//  - the other builtins answer plausible values (GetObject, FirstUnit/NextUnit, SizeTo,
//    GetX/GetY, GetUnitVid, GetVidName...) or are accepted and ignored.
// It renders like the viewer (engine/renderer.hpp): z layer, then y, z, x, creation order.
//
// Swapping it for the real engine: the core builds the engine in ONE place,
// makeGameEngine() in src/gromada_libretro.cpp. Header-only; depends on libgromada and the
// contract only.

#include "core/std.hpp"
#include "game/engine_api.hpp"
#include "libgromada/libgromada_all.hpp"

namespace gromada::game {

class ViewerEngine final : public Engine {
public:
    explicit ViewerEngine(const GameResources& resources) : m_res{resources} {}

    // --- map lifecycle -----------------------------------------------------------------------
    void loadMap(const std::filesystem::path& mapFile, std::uint32_t rngSeed) override {
        unloadMap();
        const Map map = ::loadMap(m_res.vids(), mapFile);
        m_header = map.header;
        m_clock = static_cast<int>(map.header.startTimer);
        m_rng = rngSeed;
        m_camera.x = map.header.observerX;
        m_camera.y = map.header.observerY;
        for (const auto& o : map.objects) {
            int hp = 0;
            if (const auto* m = std::get_if<Payloads::MaterialObject>(&o.payload)) hp = m->hp;
            if (const auto* a = std::get_if<Payloads::AssetObject>(&o.payload)) hp = a->hp;
            spawn(o.nvid, o.x, o.y, o.z, o.direction, hp);
        }
        clampCamera();
    }

    void unloadMap() override {
        m_units.clear();
        m_flagman.fill(0);
        m_maxUnits.clear();
        m_iter.clear();
        m_paused = false;
        m_gamma = 0x777;
        m_effect = 0;
        m_scrollLock = false;
        m_autoScroll = false;
        m_vx = m_vy = 0;
    }

    // --- frame phases --------------------------------------------------------------------------
    void beginFrame(int elapsedMs) override {
        // The clock runs during Pause() too (the menus' 700 ms timers need it); Pause(0) puts
        // it back to the value it had when the pause started (docs/re/engine.md §1.6).
        m_dt = std::clamp(elapsedMs, 0, 71);
        m_clock += m_dt;
        m_effectMs += m_dt;
    }
    void setInput(const OriginalInput& input) override { m_input = input; }

    void tactWorld() override {
        const float step = m_paused ? 0.0f : static_cast<float>(m_dt);
        driveFlagman();
        std::vector<UnitId> dead;
        for (auto& u : m_units) {
            const auto* g = graphics(u.nvid);
            if (!g || g->frameDuration == 0 || step <= 0)
                continue;
            u.clockMs += step;
            while (u.clockMs >= g->frameDuration) {
                u.clockMs -= g->frameDuration;
                if (++u.frame >= animationLength(u)) {
                    u.frame = 0;
                    if (u.action == 15) { dead.push_back(u.id); break; }
                    if (u.action == 14) u.action = 0;
                }
            }
        }
        for (const auto id : dead)
            destroy(id);
    }
    void tactPlayers() override {}
    void tactGroupAi() override {}

    void updateCamera() override {
        const float dt = static_cast<float>(m_dt);
        bool manual = false;
        if (!m_scrollLock) {
            // Edge scrolling of 0x415e20: within 5 px of a border the speed grows by 40 (x) /
            // 30 (y) per frame up to 500 / 400 px per 1024 ms.
            const int mx = m_input.mouseScreenX, my = m_input.mouseScreenY;
            if (mx <= 5) m_vx = std::max(m_vx - 40, -500);
            else if (mx >= m_camera.width - 6) m_vx = std::min(m_vx + 40, 500);
            else m_vx = 0;
            if (my <= 5) m_vy = std::max(m_vy - 30, -400);
            else if (my >= m_camera.height - 6) m_vy = std::min(m_vy + 30, 400);
            else m_vy = 0;
            manual = m_vx || m_vy;
            m_fx += m_vx * dt / 1024.0f;
            m_fy += m_vy * dt / 1024.0f;
            if (const Unit* f = alive(m_flagman[0])) {
                if (m_autoScroll && !manual) {
                    m_fx += (f->x - m_camera.width / 2 - m_camera.x) * dt / 1024.0f;
                    m_fy += (f->y - m_camera.height / 2 - m_camera.y) * dt / 1024.0f;
                }
            }
            const int ix = static_cast<int>(m_fx), iy = static_cast<int>(m_fy);
            m_camera.x += ix;
            m_camera.y += iy;
            m_fx -= ix;
            m_fy -= iy;
            if (const Unit* f = alive(m_flagman[0])) { // leash: the flagman stays near the view
                m_camera.x = std::clamp(m_camera.x, f->x - 586, std::max(f->x - 586, f->x + 586 - m_camera.width));
                m_camera.y = std::clamp(m_camera.y, f->y - 440, std::max(f->y - 440, f->y + 440 - m_camera.height));
            }
        }
        clampCamera();
    }

    void draw(FramebufferRef fb) override {
        std::ranges::fill(std::span{fb.data_handle(), static_cast<std::size_t>(fb.extent(0) * fb.extent(1))}, RGBA8{{0, 0, 0}, 255});
        struct Item { int layer, y, z, x; std::uint64_t seq; const Unit* u; };
        std::vector<Item> items;
        items.reserve(m_units.size());
        const int w = fb.extent(1), h = fb.extent(0);
        for (const auto& u : m_units) {
            const auto* g = graphics(u.nvid);
            if (!g)
                continue;
            const int sx = u.x - g->width / 2 - m_camera.x, sy = u.y - g->height / 2 - u.z - m_camera.y;
            if (sx + g->width + 128 <= 0 || sy + g->height + 128 <= 0 || sx >= w + 128 || sy >= h + 128)
                continue;
            items.push_back({m_res.vids()[u.nvid].z_layer, u.y, u.z, u.x, u.seq, &u});
        }
        std::ranges::sort(items, [](const Item& a, const Item& b) {
            return std::tie(a.layer, a.y, a.z, a.x, a.seq) < std::tie(b.layer, b.y, b.z, b.x, b.seq);
        });
        for (const auto& it : items) {
            drawSprite(*it.u, it.u->nvid, 0, 0, 0, fb);
            for (const auto& c : it.u->children)
                drawSprite(*it.u, c.nvid, c.dx, c.dy, c.dz, fb);
        }
        if (m_gamma != 0x777)
            applyGamma(fb);
    }

    void drawScreenEffects(FramebufferRef fb) override {
        if (!m_effect)
            return;
        const float t = static_cast<float>(m_effectMs);
        auto each = [&](auto fn) {
            for (int y = 0; y < fb.extent(0); ++y)
                for (int x = 0; x < fb.extent(1); ++x) fn(fb[y, x]);
        };
        if (m_effect == 5) { // eff_fade: to black in 700 ms
            const float k = std::clamp(1.0f - t / 700.0f, 0.0f, 1.0f);
            each([&](RGBA8& p) { p.r = static_cast<std::uint8_t>(p.r * k); p.g = static_cast<std::uint8_t>(p.g * k); p.b = static_cast<std::uint8_t>(p.b * k); });
        } else if (m_effect >= 1 && m_effect <= 4 && t < 600.0f) { // nukes: white/red/green/blue flash
            const float k = 1.0f - t / 600.0f;
            const int r = m_effect == 1 || m_effect == 2, g = m_effect == 1 || m_effect == 3, b = m_effect == 1 || m_effect == 4;
            auto up = [&](std::uint8_t c, int on) { return static_cast<std::uint8_t>(on ? c + (255 - c) * k : c); };
            each([&](RGBA8& p) { p.r = up(p.r, r); p.g = up(p.g, g); p.b = up(p.b, b); });
        }
    }

    // --- queries ---------------------------------------------------------------------------------
    [[nodiscard]] int clock() const override { return m_clock; }
    [[nodiscard]] Camera& camera() override { return m_camera; }
    [[nodiscard]] CursorKind cursorAt(int, int) const override {
        return alive(m_flagman[0]) && !m_paused ? CursorKind::Move : CursorKind::Arrow;
    }
    [[nodiscard]] std::vector<SoundEvent> takeSounds() override { return {}; }
    void setScrollLock(bool locked) override { m_scrollLock = locked; }

    int rand() override {
        m_rng = m_rng * 214013u + 2531011u;
        return static_cast<int>((m_rng >> 16) & 0x7fff);
    }

    // --- builtins ----------------------------------------------------------------------------------
    std::optional<script::Value> callBuiltin(int id, std::span<const script::Value> args) override {
        if (!isEngineBuiltin(id))
            return std::nullopt;
        auto arg = [&](std::size_t i) { return i < args.size() ? args[i].toInt() : 0; };
        using script::Value;
        switch (id) {
        case 65: { // SizeTo
            const Unit* u = alive(arg(0));
            if (!u) return Value{60000};
            return Value{std::max(std::abs(u->x - arg(1)), (std::abs(u->y - arg(2)) * 3) >> 1)};
        }
        case 67: return Value{action(arg(0), arg(1), arg(2), arg(3))};
        case 68: return Value{create(arg(0), arg(1), arg(2), arg(3))};
        case 69: return Value{m_clock};
        case 79: m_maxUnits[arg(0)] = arg(1); return Value{0};
        case 80: setEffect(arg(0)); return Value{0};
        case 85: return Value{alive(m_flagman[arg(0) & 3]) ? m_flagman[arg(0) & 3] : 0};
        case 86: return Value{getObject(arg(0), arg(1), arg(2))};
        case 87: // AddCommand: only act_move is followed
            if (Unit* u = aliveMutable(arg(0)); u && arg(1) == 40) { u->targetX = arg(2); u->targetY = arg(3); u->moving = true; }
            return Value{0};
        case 89: pause(arg(0) != 0); return Value{0};
        case 90: { const int max = arg(0); return Value{max + 1 > 0 ? rand() % (max + 1) : 0}; }
        case 94: { const Unit* u = alive(arg(0)); return Value{u ? int{u->nvid} : 0}; }
        case 95: return Value{firstUnit(arg(0), arg(1))};
        case 96: return Value{nextUnit()};
        case 106: m_gamma = arg(0); return Value{0};
        case 107: return Value{0}; // SetWind
        case 108: return Value{0}; // CreateLight
        case 109: { const Unit* u = alive(arg(0)); return Value{u ? u->x : 0}; }
        case 110: { const Unit* u = alive(arg(0)); return Value{u ? u->y : 0}; }
        case 119: m_flagman[arg(0) & 3] = arg(1); return Value{0};
        case 120: return Value{0}; // Save (map editor)
        case 121: return Value{std::string{}};
        case 122: return Value{0};
        case 123: return Value{0}; // AskPlace: always free
        case 124: {
            const int n = arg(0);
            if (n < 0 || n >= static_cast<int>(m_res.vids().size())) return Value{std::string{}};
            return Value{std::string{m_res.vids()[n].name.data()}};
        }
        case 126: // SetPosition: centre the view
            m_camera.x = arg(0) - m_camera.width / 2;
            m_camera.y = arg(1) - m_camera.height / 2;
            clampCamera();
            return Value{0};
        case 127: return Value{0}; // SetVid: vid tables are not simulated
        case 128: m_autoScroll = arg(0) != 0; return Value{0};
        default: return Value{0};
        }
    }

    // --- inspection --------------------------------------------------------------------------------
    [[nodiscard]] std::size_t unitCount() const noexcept { return m_units.size(); }
    [[nodiscard]] bool paused() const noexcept { return m_paused; }
    [[nodiscard]] int effect() const noexcept { return m_effect; }
    [[nodiscard]] int gamma() const noexcept { return m_gamma; }

private:
    struct Child {
        std::uint16_t nvid;
        int dx, dy, dz;
    };
    struct Unit {
        UnitId id = 0;
        std::uint16_t nvid = 0;
        int x = 0, y = 0, z = 0;
        std::uint8_t dir = 0;
        int action = 0;
        int frame = 0;
        float clockMs = 0;
        int hp = 0;
        std::uint64_t seq = 0;
        std::vector<Child> children;
        int targetX = 0, targetY = 0;
        bool moving = false;
        float fx = 0, fy = 0; // sub-pixel movement
    };

    [[nodiscard]] const VidGraphics* graphics(int nvid) const {
        if (nvid < 0 || nvid >= static_cast<int>(m_res.vids().size()))
            return nullptr;
        const auto* g = std::get_if<Vid::Graphics>(&m_res.vids()[nvid].graphicsData);
        return g && *g ? g->get() : nullptr;
    }

    [[nodiscard]] int animationLength(const Unit& u) const {
        const Vid& v = m_res.vids()[u.nvid];
        const int a = v.animationLengths[static_cast<std::size_t>(u.action)] ? u.action : 0;
        return std::max<int>(1, v.animationLengths[static_cast<std::size_t>(a)]);
    }

    UnitId spawn(int nvid, int x, int y, int z, std::uint8_t dir, int hp) {
        if (nvid < 0 || nvid >= static_cast<int>(m_res.vids().size()))
            return 0;
        const Vid& v = m_res.vids()[nvid];
        Unit u;
        u.id = m_nextId++;
        u.nvid = static_cast<std::uint16_t>(nvid);
        u.x = x;
        u.y = y;
        u.z = z;
        u.dir = dir;
        u.action = v.animationLengths[14] ? 14 : 0;
        u.hp = hp ? hp : v.maxHP;
        u.seq = m_seq++;
        m_seedAnim = m_seedAnim * 1664525u + 1013904223u; // desynchronise identical animations
        u.frame = static_cast<int>((m_seedAnim >> 8) % static_cast<std::uint32_t>(std::max(1, animationLength(u))));
        int lx = 0, ly = 0, lz = 0;
        const Vid* p = &v;
        for (int depth = 0; depth < 8 && p->linkedObjectVid > 0 && p->linkedObjectVid < m_res.vids().size(); ++depth) {
            lx += p->linkX;
            ly += p->linkY;
            lz += p->linkZ;
            u.children.push_back({p->linkedObjectVid, lx, ly, lz});
            p = &m_res.vids()[p->linkedObjectVid];
        }
        m_units.push_back(std::move(u));
        return m_units.back().id;
    }

    UnitId create(int nvid, int x, int y, int z) {
        if (nvid < 0 || nvid >= static_cast<int>(m_res.vids().size()))
            return 0;
        const Vid& v = m_res.vids()[nvid];
        if (static_cast<unsigned>(v.flags) & 0x100u) { // Randomized: CreateUnit moves it by +-8
            x += rand() % 17 - 8;
            y += rand() % 17 - 8;
        }
        if (x < 0 || y < 0 || x >= static_cast<int>(m_header.width) || y >= static_cast<int>(m_header.height))
            return 0;
        if (const auto it = m_maxUnits.find(nvid); it != m_maxUnits.end() && it->second >= 0) {
            const auto count = std::ranges::count(m_units, static_cast<std::uint16_t>(nvid), &Unit::nvid);
            if (count >= it->second)
                return 0;
        }
        return spawn(nvid, x, y, z, 0, 0);
    }

    [[nodiscard]] const Unit* alive(UnitId id) const {
        if (!id)
            return nullptr;
        const auto it = std::ranges::find(m_units, id, &Unit::id);
        return it == m_units.end() ? nullptr : &*it;
    }
    Unit* aliveMutable(UnitId id) { return const_cast<Unit*>(alive(id)); }

    void destroy(UnitId id) {
        std::erase_if(m_units, [&](const Unit& u) { return u.id == id; });
    }

    int action(UnitId id, int act, int v1, int v2) {
        Unit* u = aliveMutable(id);
        if (!u)
            return 0;
        if (act <= 15) {
            if (u->action != act) { u->action = act; u->frame = 0; u->clockMs = 0; }
            return 0;
        }
        switch (act) {
        case 54: destroy(id); return 0;                                   // act_destroy
        case 37: u->action = std::clamp(v1, 0, 15); u->dir = static_cast<std::uint8_t>(v2); u->frame = 0; return 0;
        case 65: return u->hp;                                            // act_gethp
        case 43: u->hp = std::max(0, u->hp - v1); return 0;               // act_damage
        case 64: return 100;                                              // act_getammo: unlimited
        case 35: return 1;                                                // act_changeweapon: ok
        case 82: return u->action;                                        // act_getaction
        case 76: return m_res.vids()[u->nvid].army;                       // act_getarmy
        case 68: u->x = v1; u->y = v2; return 0;                          // act_changecoor
        case 40: u->targetX = v1; u->targetY = v2; u->moving = true; return 0; // act_move
        case 66: u->moving = false; return 0;                             // act_clearcom
        default: return 0;
        }
    }

    UnitId getObject(int nvid, int x, int y) const {
        for (const auto& u : m_units) {
            if (u.nvid != nvid)
                continue;
            const Vid& v = m_res.vids()[u.nvid];
            if (std::abs(x - u.x) <= v.sizeX / 2 && std::abs(y - u.y) <= v.sizeY / 2)
                return u.id;
        }
        return 0;
    }

    // FirstUnit: units from the last to the first, army of the vid, category mask.
    UnitId firstUnit(int army, int typeMask) {
        m_iter.clear();
        for (auto it = m_units.rbegin(); it != m_units.rend(); ++it) {
            const Vid& v = m_res.vids()[it->nvid];
            if (v.army == army && (static_cast<int>(v.category) & typeMask))
                m_iter.push_back(it->id);
        }
        m_iterPos = 0;
        return nextUnit();
    }
    UnitId nextUnit() {
        while (m_iterPos < m_iter.size()) {
            const UnitId id = m_iter[m_iterPos++];
            if (alive(id))
                return id;
        }
        return 0;
    }

    void pause(bool on) {
        if (on && !m_paused)
            m_pauseClock = m_clock;
        if (!on && m_paused)
            m_clock = m_pauseClock;
        m_paused = on;
    }

    void setEffect(int e) {
        if (e == 255) e = 1 + rand() % 6;
        if (m_effect == 6) return; // eff_openwindow is running
        m_effect = e;
        m_effectMs = 0;
    }

    // The flagman drives towards the cursor while the left button is held, or with the arrows.
    void driveFlagman() {
        Unit* f = aliveMutable(m_flagman[0]);
        if (!f || m_paused)
            return;
        float dx = 0, dy = 0;
        const std::uint16_t b = m_input.bits;
        if (b & OriginalInput::LDown) {
            dx = static_cast<float>(m_input.mouseWorldX - f->x);
            dy = static_cast<float>(m_input.mouseWorldY - f->y);
        } else {
            dx = static_cast<float>(((b & OriginalInput::Right) ? 1 : 0) - ((b & OriginalInput::Left) ? 1 : 0)) * 100;
            dy = static_cast<float>(((b & OriginalInput::Down) ? 1 : 0) - ((b & OriginalInput::Up) ? 1 : 0)) * 100;
        }
        const float len = std::hypot(dx, dy);
        if (len < 4) {
            if (f->action == 2) f->action = 0;
            return;
        }
        const Vid& v = m_res.vids()[f->nvid];
        const float speed = v.speedX ? static_cast<float>(v.speedX) : 150.0f; // px per 1024 ms
        const float stepPx = std::min(len, speed * m_dt / 1024.0f);
        f->fx += dx / len * stepPx;
        f->fy += dy / len * stepPx;
        const int ix = static_cast<int>(f->fx), iy = static_cast<int>(f->fy);
        f->x = std::clamp(f->x + ix, 0, static_cast<int>(m_header.width) - 1);
        f->y = std::clamp(f->y + iy, 0, static_cast<int>(m_header.height) - 1);
        f->fx -= ix;
        f->fy -= iy;
        // direction byte: 0 = up, 64 = right (clockwise)
        const double angle = std::atan2(static_cast<double>(dx), static_cast<double>(-dy));
        f->dir = static_cast<std::uint8_t>(static_cast<int>(std::lround(angle * 128.0 / 3.14159265358979)) & 0xff);
        if (f->action == 0) f->action = v.animationLengths[2] ? 2 : 0;
    }

    void drawSprite(const Unit& u, int nvid, int dx, int dy, int dz, FramebufferRef fb) const {
        const auto* g = graphics(nvid);
        if (!g || g->frames.empty())
            return;
        const Vid& v = m_res.vids()[nvid];
        const auto range = getAnimationFrameRange(v, static_cast<Action>(u.action), u.dir);
        std::size_t frame = 0;
        if (range)
            frame = range->first + static_cast<std::size_t>(u.frame) % (range->second - range->first + 1);
        if (frame >= g->frames.size())
            return;
        const int x = u.x + dx, y = u.y + dy, z = u.z + dz;
        DrawSprite(g->frames[frame], x - g->width / 2 - m_camera.x, y - g->height / 2 - z - m_camera.y, fb);
    }

    // SetGamma: 0xRGB nibbles, 7 = neutral, 0 = channel off, 15 = saturated (docs/re/engine.md §9.4).
    void applyGamma(FramebufferRef fb) const {
        auto gain = [](std::uint8_t c, int g) -> std::uint8_t {
            if (g <= 7) return static_cast<std::uint8_t>(c * g / 7);
            return static_cast<std::uint8_t>(c + (255 - c) * (g - 7) / 8);
        };
        const int gr = (m_gamma >> 8) & 15, gg = (m_gamma >> 4) & 15, gb = m_gamma & 15;
        for (int y = 0; y < fb.extent(0); ++y)
            for (int x = 0; x < fb.extent(1); ++x) {
                RGBA8& p = fb[y, x];
                p.r = gain(p.r, gr);
                p.g = gain(p.g, gg);
                p.b = gain(p.b, gb);
            }
    }

    void clampCamera() {
        const int maxX = std::max(0, static_cast<int>(m_header.width) - m_camera.width);
        const int maxY = std::max(0, static_cast<int>(m_header.height) - m_camera.height);
        m_camera.x = std::clamp(m_camera.x, 0, maxX);
        m_camera.y = std::clamp(m_camera.y, 0, maxY);
    }

    const GameResources& m_res;
    MapHeaderRawData m_header{};
    std::vector<Unit> m_units;
    std::array<UnitId, 4> m_flagman{};
    std::map<int, int> m_maxUnits;
    std::vector<UnitId> m_iter;
    std::size_t m_iterPos = 0;
    Camera m_camera;
    OriginalInput m_input;
    int m_clock = 0, m_pauseClock = 0, m_dt = 0;
    bool m_paused = false, m_scrollLock = false, m_autoScroll = false;
    int m_vx = 0, m_vy = 0;
    float m_fx = 0, m_fy = 0;
    int m_gamma = 0x777;
    int m_effect = 0, m_effectMs = 0;
    std::uint32_t m_rng = 1, m_seedAnim = 0x9E3779B9u;
    UnitId m_nextId = 1;
    std::uint64_t m_seq = 0;
};

} // namespace gromada::game
