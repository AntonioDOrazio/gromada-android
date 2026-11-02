// Graphics enhancements (issue #21) on the real game (needs GROMADA_DATA): a battle of level
// 19 (laser, explosions, buildings) and the base of level 8, drawn every frame by engines that
// run the same input:
//  - no settings / settings all off / effects switched on and off again: the pictures with
//    the effects off are identical bit for bit (frame hashes) to the plain engine's;
//  - with every effect on the simulation is untouched: the engine state (save-state bytes)
//    is identical after the run, and the pictures do change (lights and contact shadows found);
//  - the light sources come from the data: they include the shots of the flagman's laser and
//    their explosions (classes and act_fight/act_death chains), not smoke or wrecks;
//  - cost per frame of the two passes at 1x (printed; generous bound against regressions);
//  - the pipeline of issue #20 (drawView: internal resolution, zoom, smooth motion, bands on
//    threads): effects off = the picture without settings; effects on = the same picture for
//    1 band, 4 bands on 4 threads and 3 bands run in reverse order; simulation untouched.
#include "game/engine.hpp"
#include "game/savestate.hpp"
#include "test.hpp"

#include <chrono>
#include <thread>

using namespace gromada;
using namespace gromada::game;

namespace {

std::uint64_t hashPixels(const std::vector<RGBA8>& px) {
    std::uint64_t h = 1469598103934665603ull;
    for (const RGBA8& p : px)
        for (const std::uint8_t c : {p.r, p.g, p.b, p.a}) h = (h ^ c) * 1099511628211ull;
    return h;
}

struct Run {
    std::unique_ptr<GameEngine> e;
    enhance::Settings fx;
    std::vector<RGBA8> px;
    int flagman = 0;

    Run(const GameResources& res, int level, int x, int y, bool withSettings) : px(640 * 480) {
        e = std::make_unique<GameEngine>(res);
        e->camera().width = 640;
        e->camera().height = 480;
        e->loadMap(res.mapsPath() / "levels" / std::format("{:02}.map", level), 4321);
        if (withSettings) e->setEnhancements(&fx);
        // the player's Kassandra like HEADER.INI's Tact (see tools/engine_run.cpp), laser head
        call(127, {81, 1, 60});
        flagman = call(68, {81, x, y, 0});
        call(119, {0, flagman});
        e->world().players()[0].type = 1;
        call(67, {flagman, 35, 88, 0});
        for (int k = 0; k < 4; ++k) call(67, {flagman, 63, 50, k});
        call(126, {x, y});
        call(128, {1});
    }
    int call(int id, std::initializer_list<script::Value> a) {
        std::vector<script::Value> v(a);
        return e->callBuiltin(id, v).value_or(script::Value{0}).toInt();
    }
    // drawView instead of draw() (issue #20): internal resolution k, zoom, bands.
    struct ViewMode {
        int k = 1;
        double zoom = 1;
        int bands = 1;
        bool threaded = false, reversed = false;
    };
    std::optional<ViewMode> view;
    void setView(ViewMode m) {
        view = m;
        px.assign(static_cast<std::size_t>(640 * m.k) * static_cast<std::size_t>(480 * m.k), RGBA8{});
    }
    void drawViewFrame(int n) {
        const ViewMode& m = *view;
        ViewRequest r;
        r.width = 640 / m.zoom;
        r.height = 480 / m.zoom;
        r.centerX = e->camera().x + 320;
        r.centerY = e->camera().y + 240;
        r.scale = m.k * m.zoom;
        r.screenScale = m.k;
        r.renderClock = e->clock() + (n % 4) * 2.0;   // between the ticks: extrapolated positions
        r.extrapolate = true;
        r.fitMap = true;
        r.threads = m.bands;
        if (m.bands > 1)
            r.parallel = [&](int count, const std::function<void(int)>& fn) {
                if (m.threaded) {
                    std::vector<std::thread> th;
                    for (int i = 0; i < count; ++i) th.emplace_back(fn, i);
                    for (auto& t : th) t.join();
                } else if (m.reversed) {
                    for (int i = count - 1; i >= 0; --i) fn(i);
                } else {
                    for (int i = 0; i < count; ++i) fn(i);
                }
            };
        const auto o = e->viewOrigin(r);
        e->drawView(FramebufferRef{px.data(), 480 * m.k, 640 * m.k}, r, o.value_or(ViewOrigin{static_cast<double>(e->camera().x),
                                                                                               static_cast<double>(e->camera().y)}));
    }
    // One frame: fire at a point 300 px to the right of the flagman from frame 10.
    std::uint64_t frame(int n) {
        OriginalInput in;
        const Sprite* f = e->world().byId(flagman);
        const int fx0 = f ? f->x : e->camera().x + 320, fy0 = f ? f->y : e->camera().y + 240;
        in.mouseWorldX = static_cast<std::int16_t>(fx0 + 300);
        in.mouseWorldY = static_cast<std::int16_t>(fy0 - 80);
        in.mouseScreenX = 320;
        in.mouseScreenY = 240;
        if (n >= 10) in.bits |= OriginalInput::RDown | (n == 10 ? OriginalInput::RClick : 0);
        e->beginFrame(n % 3 ? 17 : 16);
        e->setInput(in);
        e->tactWorld();
        if (view) drawViewFrame(n);
        else e->draw(FramebufferRef{px.data(), 480, 640});
        e->tactPlayers();
        e->tactGroupAi();
        e->updateCamera();
        e->takeSounds();
        return hashPixels(px);
    }
    std::vector<std::uint8_t> state() const {
        std::vector<std::uint8_t> buf;
        state::Writer w{buf};
        game::SaveStateAccess::saveEngine(w, *e);
        buf.resize(w.size());
        return buf;
    }
};

void scene(const GameResources& res, int level, int x, int y, int frames, bool expectLights) {
    std::fprintf(stderr, "level %d at (%d, %d), %d frames\n", level, x, y, frames);
    Run plain(res, level, x, y, false), off(res, level, x, y, true), toggled(res, level, x, y, true), on(res, level, x, y, true);
    on.fx = {3, 3};
    int sameOff = 0, sameToggled = 0, toggledOffFrames = 0, changed = 0, maxLights = 0, maxOccluders = 0;
    std::vector<double> cost;
    for (int n = 0; n < frames; ++n) {
        toggled.fx = (n / 30) % 2 ? enhance::Settings{2, 2} : enhance::Settings{};
        const auto h = plain.frame(n);
        sameOff += off.frame(n) == h;
        const auto ht = toggled.frame(n);
        if (!toggled.fx.any()) {
            ++toggledOffFrames;
            sameToggled += ht == h;
        }
        changed += on.frame(n) != h;
        const auto& st = on.e->enhancements()->stats();
        cost.push_back(st.groundMs + st.lightMs);
        maxLights = std::max(maxLights, st.lights);
        maxOccluders = std::max(maxOccluders, st.occluders);
    }
    CHECK_EQ(sameOff, frames);                    // settings present but off: bit-identical
    CHECK_EQ(sameToggled, toggledOffFrames);      // switched off again: no residue
    CHECK(changed > frames / 2);                  // on: the picture changes
    CHECK(maxOccluders > 10);
    if (expectLights) CHECK(maxLights > 0);
    CHECK(on.state() == plain.state());           // the simulation is untouched
    CHECK(off.state() == plain.state());
    std::ranges::sort(cost);
    const double p50 = cost[cost.size() / 2], p99 = cost[cost.size() * 99 / 100], mx = cost.back();
    std::fprintf(stderr, "  effects 3,3 at 1x: p50 %.3f ms, p99 %.3f ms, max %.3f ms (max %d occluders, %d lights)\n", p50, p99, mx,
                 maxOccluders, maxLights);
    CHECK(p50 < 8.0);   // the 120 fps frame; the measured cost is ~0.5-1 ms on a desktop (docs/enhancements.md)
}

// The same through drawView (issue #20 pipeline) at internal resolution k and zoom.
void viewScene(const GameResources& res, int k, double zoom, int frames) {
    std::fprintf(stderr, "drawView: level 19 battle, %dx, zoom %.1f, %d frames\n", k, zoom, frames);
    const int x = 4520, y = 1720;
    Run plain(res, 19, x, y, false), off(res, 19, x, y, true), on1(res, 19, x, y, true), on4(res, 19, x, y, true), on3(res, 19, x, y, true);
    plain.setView({k, zoom, 1});
    off.setView({k, zoom, 4, true});
    on1.setView({k, zoom, 1});
    on4.setView({k, zoom, 4, true});
    on3.setView({k, zoom, 3, false, true});
    on1.fx = on4.fx = on3.fx = {2, 2};
    int sameOff = 0, sameBands = 0, changed = 0, maxLights = 0, maxOccluders = 0;
    for (int n = 0; n < frames; ++n) {
        const auto h = plain.frame(n);
        sameOff += off.frame(n) == h;
        const auto h1 = on1.frame(n);
        sameBands += on4.frame(n) == h1 && on3.frame(n) == h1;
        changed += h1 != h;
        const auto st = on1.e->enhancements()->stats();
        maxLights = std::max(maxLights, st.lights);
        maxOccluders = std::max(maxOccluders, st.occluders);
    }
    CHECK_EQ(sameOff, frames);     // off: bit-identical
    CHECK_EQ(sameBands, frames);   // the same picture for any band split and thread
    CHECK(changed > frames / 2);
    CHECK(maxOccluders > 10 && maxLights > 0);
    CHECK(on1.state() == plain.state());
    CHECK(on4.state() == plain.state());
    std::fprintf(stderr, "  off identical %d/%d, bands identical %d/%d, changed %d (max %d occluders, %d lights)\n", sameOff, frames,
                 sameBands, frames, changed, maxOccluders, maxLights);
}

// The emitter classification from the data includes the laser chain of the flagman's head
// (HEPHEST: act_fight -> muzzle light -> laser ray -> laser blast), and per-frame emission
// keeps smoke dark.
void emitters(const GameResources& res) {
    std::vector<const Vid*> ptr;
    for (const Vid& v : res.vids()) ptr.push_back(&v);
    const auto e = enhance::classifyEmitters(ptr);
    const Vid& head = res.vids()[88];
    const int fight = std::abs(static_cast<int>(head.childNvid[enhance::ActFight]));
    CHECK(fight > 0 && e[static_cast<std::size_t>(fight)]);
    int lightFrames = 0, darkFrames = 0;
    for (std::size_t i = 0; i < e.size(); ++i) {
        if (!e[i]) continue;
        const auto* g = std::get_if<Vid::Graphics>(&res.vids()[i].graphicsData);
        if (!g || !*g) continue;
        for (const auto& f : (*g)->frames) {
            const auto em = enhance::frameEmission(f, static_cast<int>(res.vids()[i].type));
            (em.power >= enhance::MinLightPower ? lightFrames : darkFrames)++;
        }
    }
    std::fprintf(stderr, "emitter candidates: %d vids; frames that give light %d, dark %d\n",
                 static_cast<int>(std::ranges::count(e, 1)), lightFrames, darkFrames);
    CHECK(lightFrames > 100 && darkFrames > 100);
}

} // namespace

int main() {
    SKIP_WITHOUT_DATA();
    const GameResources res{*gromada_test::dataDir() / "fw.res"};
    emitters(res);
    scene(res, 19, 4520, 1720, 240, true);   // battle: laser tanks, turrets, explosions
    scene(res, 8, 1400, 1450, 90, false);    // base with buildings
    viewScene(res, 1, 1.0, 90);
    viewScene(res, 2, 1.5, 90);
    viewScene(res, 2, 0.5, 60);
    return gromada_test::finish();
}
