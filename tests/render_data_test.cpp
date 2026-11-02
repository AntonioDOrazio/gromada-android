// Rendering pipeline on the original data (issue #20, docs/rendering.md; skipped without
// GROMADA_DATA):
//  - every combination of internal resolution 1..4, zoom 0.5/1/2, 4:3/16:9 and 60/120 fps on
//    level 1 played with scripted input: no crash, picture size, zoom applied to the engine's
//    view once the briefing is gone, the player drawn exactly where the screen <-> world
//    mapping puts it, and a cursor placed there points at the player's world point in the
//    input record, and the picture has the player there;
//  - draw() and drawView() with the identity view give the same picture on real levels
//    (4:3, every 50th frame of 600, levels 1, 5, 12, 19);
//  - save states with zoom 1.5 and internal resolution 2: save, play, load, play again ->
//    identical pictures (zoom, its anchor and the mapping are state);
//  - a demo at internal resolution 2 in 16:9: pillarbox, not zoomed.
#include "core/std.hpp"
#include "libgromada/libgromada_all.hpp"
#include "game/savestate.hpp"
#include "test.hpp"

using namespace gromada;
using namespace gromada::game;

namespace {

// Scripted input: the cursor wanders, a left click (move order) every 1.5 s. Level 1 started
// directly is played for about 4 s, then its script ends it (a menu, then the mission map).
input::PcState inputAt(int f, int fps) {
    const int t = f * 60 / fps;
    input::PcState pc;
    pc.cursor = {static_cast<float>(320 + 200 * std::sin(t * 0.031)), static_cast<float>(240 + 150 * std::cos(t * 0.017))};
    const auto button = [](input::ButtonState& b, bool down, bool before) { b = {down, down && !before, !down && before}; };
    const auto at = [&](auto&& pred, int k) { return pred(k * 60 / fps); };
    const auto leftAt = [](int k) { return k > 20 && k % 90 < 4; };
    button(pc.left, at(leftAt, f), at(leftAt, f - 1));
    return pc;
}

std::uint64_t fnv(const void* p, std::size_t n) {
    return state::fnv1a(std::span{static_cast<const std::uint8_t*>(p), n}, 0xcbf29ce484222325ull);
}

std::unique_ptr<Session> makeSession(const GameResources& res, const std::filesystem::path& data, int width, int k, bool render = true) {
    SessionConfig cfg;
    cfg.gameDir = data;
    cfg.outputWidth = width;
    cfg.outputHeight = 480;
    cfg.renderScale = k;
    cfg.smoothMotion = true;
    cfg.render = render;
    return std::make_unique<Session>(res.vids(), res.sounds(), std::make_unique<GameEngine>(res), nullptr, nullptr, cfg);
}

int combinations(const GameResources& res, const std::filesystem::path& data) {
    int checked = 0;
    for (const int width : {640, 854})
        for (const int fps : {60, 120})
            for (const int k : {1, 2, 3, 4})
                for (const float zoom : {0.5f, 1.0f, 2.0f}) {
                    auto s = makeSession(res, data, width, k, false);
                    std::optional<DrawnSprite> player;
                    s->setWorldPass([&](const WorldPass& p) {
                        player.reset();
                        for (const auto& d : p.sprites)
                            if (d.cls == cls::Kassandra && !d.child && !player) player = d;
                        CHECK_EQ(p.world.extent(1), s->viewWidth() * k);
                        CHECK_EQ(p.world.extent(0), 480 * k);
                    });
                    s->setZoom(zoom);
                    s->start("maps\\levels\\01.map");
                    const int frames = 200 * fps / 60;
                    for (int f = 0; f < frames; ++f) {
                        if (f == frames - 8) s->setRender(true);   // only the last frames are drawn (speed)
                        s->runFrame(inputAt(f, fps), 1.0 / fps);
                    }
                    const auto tag = std::format("{}x480 {} fps k={} zoom={}", width, fps, k, zoom);
                    CHECK_EQ(s->width(), width * k);
                    CHECK_EQ(s->height(), 480 * k);
                    CHECK(!s->ui().hasMenu());
                    CHECK(std::abs(s->effectiveZoom() - zoom) < 1e-6);
                    const Camera& cam = s->engine().camera();
                    CHECK_EQ(cam.width, static_cast<int>(std::lround(width / zoom)));
                    CHECK_EQ(cam.height, static_cast<int>(std::lround(480 / zoom)));
                    CHECK(player.has_value());
                    if (!player) { std::fprintf(stderr, "%s: no player drawn\n", tag.c_str()); continue; }
                    // the player is drawn where the mapping says
                    const ScreenMapping m = s->mapping();
                    const double ox = m.outputX(player->worldX), oy = m.outputY(player->worldY);
                    CHECK(std::abs(ox - (player->fbX / k + m.viewX)) < 0.01);
                    CHECK(std::abs(oy - player->fbY / k) < 0.01);
                    // a cursor on it points at its world point (record) and the engine sees the
                    // player's unit there (unit under the cursor, half a body above the ground)
                    InputRecordBuilder b;
                    input::PcState pc;
                    pc.cursor = {static_cast<float>(ox), static_cast<float>(oy)};
                    const OriginalInput r = b.build(pc, m, 1.0f / fps, {cam.width, cam.height});
                    CHECK(std::abs(r.mouseWorldX - player->worldX) <= 1.0);
                    CHECK(std::abs(r.mouseWorldY - player->worldY) <= 1.0);
                    CHECK_EQ(r.mouseScreenX, static_cast<int>(std::floor((ox - m.viewX) / m.zoom)));
                    // the picture has the player's pixels around that point (not the black clear)
                    const int px = static_cast<int>(ox * k), py = static_cast<int>((oy - player->worldZ * m.zoom) * k);
                    int lit = 0;
                    for (int dy = -4 * k; dy <= 4 * k; ++dy)
                        for (int dx = -4 * k; dx <= 4 * k; ++dx) {
                            const int x = std::clamp(px + dx, 0, s->width() - 1), y = std::clamp(py + dy, 0, s->height() - 1);
                            lit += (s->xrgb()[static_cast<std::size_t>(y) * s->width() + x] & 0xFFFFFF) != 0;
                        }
                    CHECK(lit > 0);
                    ++checked;
                    std::fprintf(stderr, "%s: player world (%.2f, %.2f) on output (%.1f, %.1f), view %dx%d at %.2f,%.2f\n", tag.c_str(),
                                 player->worldX, player->worldY, ox, oy, cam.width, cam.height, m.originX, m.originY);
                }
    return checked;
}

// draw() == drawView(identity) on real levels.
void identity(const GameResources& res, const std::filesystem::path& data) {
    for (const int level : {1, 5, 12, 19}) {
        auto s = makeSession(res, data, 640, 1, false);
        s->start(std::format("maps\\levels\\{:02}.map", level));
        int compared = 0, differ = 0;
        std::vector<RGBA8> a(640 * 480), b(640 * 480);
        for (int f = 0; f < 600; ++f) {
            s->runFrame(inputAt(f, 60), 1.0 / 60);
            if (f % 50 != 49) continue;
            auto& e = s->engine();
            const Camera cam = e.camera();
            e.draw(FramebufferRef{a.data(), 480, 640});
            ViewRequest r;
            r.width = cam.width; r.height = cam.height;
            r.centerX = cam.x + cam.width / 2.0; r.centerY = cam.y + cam.height / 2.0;
            const auto o = e.viewOrigin(r);
            CHECK(o && o->x == cam.x && o->y == cam.y);
            e.drawView(FramebufferRef{b.data(), 480, 640}, r, *o);
            ++compared;
            for (std::size_t i = 0; i < a.size(); ++i)
                if (a[i].r != b[i].r || a[i].g != b[i].g || a[i].b != b[i].b) { ++differ; break; }
        }
        std::fprintf(stderr, "level %02d: draw() == drawView(identity) on %d/%d frames\n", level, compared - differ, compared);
        CHECK_EQ(differ, 0);
    }
}

// Save states with zoom and internal resolution: the frames after a load are the same.
void saveStates(const GameResources& res, const std::filesystem::path& data) {
    auto s = makeSession(res, data, 854, 2);
    s->setZoom(1.5f);
    s->start("maps\\levels\\01.map");
    int f = 0;
    for (; f < 200; ++f) s->runFrame(inputAt(f, 120), 1.0 / 120);
    SaveStateManager states;
    std::vector<std::uint8_t> st, st2;
    CHECK(states.save(*s, st));
    const int f0 = f;
    std::vector<std::uint64_t> first, second;
    for (int i = 0; i < 250; ++i, ++f) {
        input::PcState pc = inputAt(f, 120);
        pc.zoomRate = i < 100 ? 0.7f : i < 150 ? -1.0f : 0.0f;   // zooming in and out across the run
        s->runFrame(pc, 1.0 / 120);
        first.push_back(fnv(s->xrgb(), static_cast<std::size_t>(s->width()) * s->height() * 4));
    }
    const float zoomAfter = s->zoom();
    CHECK(states.load(*s, st));
    CHECK(states.save(*s, st2));
    CHECK(st == st2);   // load + save = the same bytes
    f = f0;
    for (int i = 0; i < 250; ++i, ++f) {
        input::PcState pc = inputAt(f, 120);
        pc.zoomRate = i < 100 ? 0.7f : i < 150 ? -1.0f : 0.0f;
        s->runFrame(pc, 1.0 / 120);
        second.push_back(fnv(s->xrgb(), static_cast<std::size_t>(s->width()) * s->height() * 4));
    }
    int differ = 0;
    for (std::size_t i = 0; i < first.size(); ++i) differ += first[i] != second[i];
    std::fprintf(stderr, "save state (zoom 1.5 -> %.3f, k=2, 16:9, 120 fps): %d of %zu frames differ after the load\n", zoomAfter, differ,
                 first.size());
    CHECK_EQ(differ, 0);
    CHECK(zoomAfter != 1.5f);
    CHECK(!s->ui().hasMenu());   // still playing
    CHECK_EQ(s->zoom(), zoomAfter);
}

// A demo at k = 2 in 16:9: 640 x 480 view centred (pillarbox), never zoomed.
void demoPlayback(const GameResources& res, const std::filesystem::path& data) {
    auto s = makeSession(res, data, 854, 2);
    s->setZoom(2.0f);
    s->start();
    s->playDemo(1);
    for (int f = 0; f < 600; ++f) s->runFrame({}, 1.0 / 120);
    CHECK(s->demoPlaying());
    CHECK_EQ(s->effectiveZoom(), 1.0);
    CHECK_EQ(s->engine().camera().width, 640);
    int border = 0, inside = 0;
    for (int y = 0; y < s->height(); y += 7) {
        border += (s->xrgb()[static_cast<std::size_t>(y) * s->width() + 10] & 0xFFFFFF) != 0;
        border += (s->xrgb()[static_cast<std::size_t>(y) * s->width() + s->width() - 10] & 0xFFFFFF) != 0;
        inside += (s->xrgb()[static_cast<std::size_t>(y) * s->width() + s->width() / 2] & 0xFFFFFF) != 0;
    }
    CHECK_EQ(border, 0);
    CHECK(inside > 10);
}

} // namespace

int main() {
    SKIP_WITHOUT_DATA();
    const auto data = *gromada_test::dataDir();
    const GameResources res{data / "fw.res"};
    identity(res, data);
    demoPlayback(res, data);
    saveStates(res, data);
    const int n = combinations(res, data);
    std::fprintf(stderr, "%d combinations checked\n", n);
    CHECK_EQ(n, 48);
    return gromada_test::finish();
}
