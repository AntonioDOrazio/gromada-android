// Rendering pipeline of the game view (issue #20, docs/rendering.md), synthetic (no game data):
//  - the scaled rasterizer: at scale 1 and integer positions it is putcadr (hicolor::drawFrame)
//    bit for bit on the oracle's synthetic frames of formats 2/3/4/8 with and without cast
//    shadow; at integer scale k it is the k x k nearest-neighbour enlargement of that picture;
//    at fractional scales and sub-pixel positions adjacent frames meet without seams;
//  - the engine: drawView() with the identity view is draw(); zoomed it is the enlargement;
//    the view origin (map limits, small maps centred, leash on the drawn flagman);
//  - smooth motion: the drawn position is continuous across the ticks of a moving unit, a
//    teleport is not interpolated, rays and paused worlds are not extrapolated;
//  - the screen <-> world mapping of the input record (identity = the original's), zoomed;
//  - the internal-resolution composite: UI as k x k blocks, Effect() on the world, windows.
#include "game/engine.hpp"
#include "game/render_view.hpp"
#include "game/session_input.hpp"
#include "oracle_vectors.hpp"
#include "test.hpp"

using namespace gromada;
using namespace gromada::game;
namespace hc = gromada::game::hicolor;

namespace {

std::vector<std::int64_t> ints(const oracle_vectors::Json& j) {
    std::vector<std::int64_t> v;
    for (const auto& x : j.arr) v.push_back(x.num);
    return v;
}

bool same(const RGBA8& a, const RGBA8& b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

// ---------------------------------------------------------------------------- rasterizer
void testRasterIdentityAndScale() {
    const auto f = oracle_vectors::load("hicolor_putcadr");
    if (!f.loaded) return;
    const auto& a = f.root.find("arrays")->obj;
    constexpr int W = 40, H = 12, FBW = 56, FBH = 18;
    std::array<hc::Color16, 256> pal16{};
    const auto p = ints(a.at("palette"));
    for (std::size_t i = 0; i < 256; ++i) pal16[i] = static_cast<hc::Color16>(p[i]);
    const auto palette = hc::expand(pal16);
    const auto background = ints(a.at("background"));
    std::map<int, VidGraphics> gfx;
    std::map<int, std::vector<std::byte>> data;
    for (const int fmt : {2, 3, 4, 8}) {
        for (const auto b : ints(a.at("frame" + std::to_string(fmt)))) data[fmt].push_back(static_cast<std::byte>(b));
        auto& g = gfx[fmt];
        g.dataFormat = static_cast<std::uint8_t>(fmt);
        g.width = W;
        g.height = H;
        g.numOfFrames = 1;
    }
    view::ScaleScratch scratch;
    int cases = 0, bad1 = 0, badK = 0;
    for (const auto& c : f.cases()) {
        const auto v = ints(c);
        const int fmt = static_cast<int>(v[0]);
        const bool cast = v[1] != 0 && v[4] > 0;
        const int x = static_cast<int>(v[2]), y = static_cast<int>(v[3]), z = static_cast<int>(v[4]);
        const VidGraphics::Frame frame{std::span<const std::byte>{data[fmt]}, &gfx[fmt]};
        const auto draw = [&](auto&& one) {
            if (cast) one(x - (z >> 1) - (z >> 2), y + (z >> 2) - (z >> 1), hc::FrameVisitor::Pass::CastShadow, false);
            one(x, y - z, hc::FrameVisitor::Pass::Normal, cast);
        };
        // reference: putcadr
        std::vector<RGBA8> ref(FBW * FBH);
        for (std::size_t i = 0; i < ref.size(); ++i) ref[i] = hc::toPixel(static_cast<hc::Color16>(background[i]));
        FramebufferRef rfb{ref.data(), FBH, FBW};
        draw([&](int px, int py, auto pass, bool skip) { hc::drawFrame(frame, px, py, palette, rfb, pass, skip); });
        // scale 1 through the view transform
        std::vector<RGBA8> got(FBW * FBH);
        for (std::size_t i = 0; i < got.size(); ++i) got[i] = hc::toPixel(static_cast<hc::Color16>(background[i]));
        const view::Raster r1{FramebufferRef{got.data(), FBH, FBW}};
        draw([&](int px, int py, auto pass, bool skip) {
            view::drawFrameScaled(frame, std::int64_t{px} << view::SubBits, std::int64_t{py} << view::SubBits, palette, r1, scratch, pass, skip);
        });
        for (std::size_t i = 0; i < ref.size(); ++i)
            if (!same(ref[i], got[i])) { ++bad1; break; }
        // scale k: the k x k enlargement (background enlarged too: every operation is per pixel)
        for (const int k : {2, 3}) {
            std::vector<RGBA8> big(static_cast<std::size_t>(FBW * k * FBH * k));
            for (int yy = 0; yy < FBH * k; ++yy)
                for (int xx = 0; xx < FBW * k; ++xx)
                    big[static_cast<std::size_t>(yy * FBW * k + xx)] = hc::toPixel(static_cast<hc::Color16>(background[static_cast<std::size_t>((yy / k) * FBW + xx / k)]));
            const view::Raster rk{FramebufferRef{big.data(), FBH * k, FBW * k}, 0, 0, view::toScale(k)};
            draw([&](int px, int py, auto pass, bool skip) {
                view::drawFrameScaled(frame, std::int64_t{px} << view::SubBits, std::int64_t{py} << view::SubBits, palette, rk, scratch, pass, skip);
            });
            bool ok = true;
            for (int yy = 0; yy < FBH * k && ok; ++yy)
                for (int xx = 0; xx < FBW * k && ok; ++xx)
                    ok = same(big[static_cast<std::size_t>(yy * FBW * k + xx)], ref[static_cast<std::size_t>((yy / k) * FBW + xx / k)]);
            if (!ok) ++badK;
        }
        ++cases;
    }
    std::fprintf(stderr, "raster: %d putcadr cases, %d differ at scale 1, %d differ at scale 2/3\n", cases, bad1, badK);
    CHECK(cases > 0);
    CHECK_EQ(bad1, 0);
    CHECK_EQ(badK, 0);
}

// A solid format-2 frame (w x h, one palette index): rows "repeat w times colour", end.
struct SolidFrame {
    VidGraphics g;
    std::vector<std::byte> bytes;
    VidGraphics::Frame frame() const { return {std::span<const std::byte>{bytes}, &g}; }
    SolidFrame(int w, int h, std::uint8_t colour) {
        g.dataFormat = 2;
        g.width = static_cast<std::uint16_t>(w);
        g.height = static_cast<std::uint16_t>(h);
        g.numOfFrames = 16;   // every animation frame the same picture
        g.frameDuration = 71;
        for (auto& c : g.palette) c = {0, 0, 0};
        g.palette[1] = {255, 0, 0};
        g.palette[2] = {0, 255, 0};
        g.palette[3] = {0, 0, 255};
        auto put = [&](int b) { bytes.push_back(static_cast<std::byte>(b)); };
        put(0); put(0); put(h & 0xff); put(h >> 8);   // startY, rows
        for (int y = 0; y < h; ++y) {
            put(0xC0 | w); put(colour);                  // command 3 (repeat), count w
            put(0);                                      // end of row
        }
        g.frames.assign(16, frame());
    }
};

void testRasterSeams() {
    // Two 7 x 5 tiles side by side and one below, at fractional scales and sub-pixel origins:
    // together they cover a rectangle without gaps or overlaps.
    const SolidFrame red{7, 5, 1}, green{7, 5, 2}, blue{7, 5, 3};
    const auto pal = hc::expand(hc::palette16(red.g.palette, 0x777, true));
    view::ScaleScratch scratch;
    int checked = 0;
    for (const double scale : {0.5, 0.73, 1.0, 1.37, 2.0, 2.61, 4.0, 8.0})
        for (const double ox : {0.0, 0.3, 0.77, -1.41}) {
            constexpr int FW = 160, FH = 120;
            std::vector<RGBA8> px(FW * FH, RGBA8{{0, 0, 0}, 255});
            const view::Raster r{FramebufferRef{px.data(), FH, FW}, view::toSub(ox), view::toSub(ox * 0.5), view::toScale(scale)};
            const std::int64_t L = view::toSub(2.25), T = view::toSub(1.5);   // sub-pixel world position
            view::drawFrameScaled(red.frame(), L, T, pal, r, scratch);
            view::drawFrameScaled(green.frame(), L + (7 << view::SubBits), T, pal, r, scratch);
            view::drawFrameScaled(blue.frame(), L, T + (5 << view::SubBits), pal, r, scratch);
            const int x0 = r.column(L), x1 = r.column(L + (7 << view::SubBits)), x2 = r.column(L + (14 << view::SubBits));
            const int y0 = r.row(T), y1 = r.row(T + (5 << view::SubBits)), y2 = r.row(T + (10 << view::SubBits));
            for (int y = std::max(y0, 0); y < std::min(y2, FH); ++y)
                for (int x = std::max(x0, 0); x < std::min(x2, FW); ++x) {
                    const RGBA8 c = px[static_cast<std::size_t>(y * FW + x)];
                    const bool top = y < y1, left = x < x1;
                    if (top) CHECK(left ? c.r == 255 : c.g == 255);
                    else if (left) CHECK(c.b == 255);
                    else CHECK(c.r == 0 && c.g == 0 && c.b == 0);
                }
            // nothing outside the union
            for (int y = 0; y < FH; ++y)
                for (int x = 0; x < FW; ++x)
                    if (x < x0 || x >= x2 || y < y0 || y >= y2) CHECK(px[static_cast<std::size_t>(y * FW + x)].r == 0);
            // the frame covers round(7 * scale) +-1 columns
            CHECK(std::abs((x1 - x0) - 7 * scale) <= 1.0);
            ++checked;
        }
    CHECK_EQ(checked, 32);
}

// ---------------------------------------------------------------------------- engine
struct World3 {
    std::vector<Weapon> weapons{1};
    std::vector<Vid> vids;
    std::vector<SolidFrame> frames;
    std::unique_ptr<GameEngine> e;

    World3() {
        frames.reserve(4);
        frames.emplace_back(32, 32, 1);   // ground tile
        frames.emplace_back(12, 10, 2);   // vehicle (shadow, moving)
        frames.emplace_back(6, 6, 3);     // projectile
        frames.emplace_back(40, 8, 1);    // screen sprite
        const auto vid = [&](std::size_t f, std::uint8_t cls, std::uint8_t cat, std::uint16_t flags, int layer, int speed) {
            Vid v;
            v.category = static_cast<ObjectCategory>(cat);
            v.type = static_cast<ObjectClass>(cls);
            v.flags = static_cast<ObjectFlags>(flags);
            v.collisionMask = 1;   // terrain passable for everyone
            v.sizeX = v.sizeY = 8;
            v.sizeZ = 4;
            v.speedX = static_cast<std::uint16_t>(speed);
            v.acceleration = 9999;
            v.directionsCount = 1;
            v.z_layer = static_cast<std::uint8_t>(layer);
            v.animationLengths.fill(1);
            v.graphicsData = std::shared_ptr<VidGraphics>(std::shared_ptr<VidGraphics>{}, const_cast<VidGraphics*>(&frames[f].g));
            return v;
        };
        vids.push_back(vid(0, cls::Terrain, cat::Terrain, 0, 0, 0));
        vids.push_back(vid(1, cls::Vehicle, cat::Vehicle, vf::Shadow | vf::NoCollision, 5, 60));
        vids.push_back(vid(2, cls::Projectile, cat::Object, vf::NoCollision, 6, 400));
        vids.push_back(vid(3, cls::Image, cat::Object, 0, 9, 0));
        vids.push_back(vid(1, cls::Vehicle, cat::Vehicle, vf::PresentOnGrid, 5, 60));   // collides
        e = std::make_unique<GameEngine>(vids, weapons);
        e->loadEmptyMap(1200, 900, 7);
        e->camera() = {100, 80, 640, 480};
        World& w = e->world();
        for (int ty = 0; ty < 900; ty += 32)
            for (int tx = 0; tx < 1200; tx += 32) w.createUnit(0, tx + 16, ty + 16, 0, 0, 0, nullptr);
    }
    World& world() { return e->world(); }
    void step(int ms) {
        e->beginFrame(ms);
        e->tactWorld();
    }
};

void testEngineDrawView() {
    World3 t;
    World& w = t.world();
    Sprite* tank = w.createUnit(1, 400, 300, 0, 0, 0, nullptr);
    Sprite* high = w.createUnit(1, 520, 330, 20, 0, 0, nullptr);   // cast shadow
    w.createUnit(3, 50, 30, 0, 0, 0, nullptr);                       // screen sprite (st_screen)
    CHECK(tank && high);
    w.action(*tank, act::move, 700, 500);
    for (int i = 0; i < 20; ++i) t.step(17);
    const Camera cam = t.e->camera();
    std::vector<RGBA8> ref(640 * 480), got(640 * 480);
    t.e->draw(FramebufferRef{ref.data(), 480, 640});
    ViewRequest r;
    r.width = 640; r.height = 480;
    r.centerX = cam.x + 320; r.centerY = cam.y + 240;
    const auto o = t.e->viewOrigin(r);
    CHECK(o.has_value());
    CHECK_EQ(o->x, static_cast<double>(cam.x));
    CHECK_EQ(o->y, static_cast<double>(cam.y));
    std::vector<DrawnSprite> drawn;
    r.drawn = &drawn;
    CHECK(t.e->drawView(FramebufferRef{got.data(), 480, 640}, r, *o));
    int diff = 0;
    for (std::size_t i = 0; i < ref.size(); ++i) diff += !same(ref[i], got[i]);
    CHECK_EQ(diff, 0);
    CHECK(drawn.size() > 300);
    const auto it = std::ranges::find_if(drawn, [&](const DrawnSprite& d) { return d.id == tank->id; });
    CHECK(it != drawn.end());
    if (it != drawn.end()) {
        CHECK_EQ(it->fbX, static_cast<float>(tank->x - cam.x));
        CHECK_EQ(it->fbY, static_cast<float>(tank->y - cam.y));
        CHECK_EQ(it->cls, cls::Vehicle);
    }

    // zoom 2 (a 320 x 240 view drawn at scale 2) = the 2 x 2 enlargement of draw() on that view
    t.e->camera() = {cam.x + 160, cam.y + 120, 320, 240};
    std::vector<RGBA8> small(320 * 240), big(640 * 480);
    t.e->draw(FramebufferRef{small.data(), 240, 320});
    ViewRequest z;
    z.width = 320; z.height = 240; z.scale = 2; z.screenScale = 1;
    z.centerX = cam.x + 320; z.centerY = cam.y + 240;
    const auto oz = t.e->viewOrigin(z);
    CHECK(oz && oz->x == cam.x + 160 && oz->y == cam.y + 120);
    t.e->drawView(FramebufferRef{big.data(), 480, 640}, z, *oz);
    diff = 0;
    for (int y = 0; y < 480; ++y)
        for (int x = 0; x < 640; ++x) {
            // the screen sprite is not zoomed (screenScale 1): compare the world only
            if (y < 40 && x < 80) continue;
            diff += !same(big[static_cast<std::size_t>(y * 640 + x)], small[static_cast<std::size_t>((y / 2) * 320 + x / 2)]);
        }
    CHECK_EQ(diff, 0);
    // the screen sprite stays at its screen position with screenScale 1
    CHECK(big[static_cast<std::size_t>(30 * 640 + 50)].r == 255);
}

void testViewOrigin() {
    World3 t;   // map 1200 x 900
    ViewRequest r;
    r.fitMap = true;
    // zoom 0.5 on a 640 x 480 output: 1280 x 960 visible, wider and taller than the map: centred
    r.width = 1280; r.height = 960; r.centerX = 300; r.centerY = 200;
    auto o = t.e->viewOrigin(r);
    CHECK(o && o->x == -40 && o->y == -30);
    // zoom 2: 320 x 240, clamped at the map borders
    r.width = 320; r.height = 240; r.centerX = 50; r.centerY = 890;
    o = t.e->viewOrigin(r);
    CHECK(o && o->x == 0 && o->y == 900 - 240);
    r.centerX = 600.25; r.centerY = 450.5;
    o = t.e->viewOrigin(r);
    CHECK(o && o->x == 440.25 && o->y == 330.5);   // sub-pixel centre kept
    // without fitMap nothing moves
    r.fitMap = false; r.centerX = 50;
    o = t.e->viewOrigin(r);
    CHECK(o && o->x == 50 - 160);
    // leash on the flagman where it is drawn (extrapolated): the view keeps it 54 px inside
    World& w = t.world();
    Sprite* f = w.createUnit(1, 600, 450, 0, 0, 64, nullptr);   // direction 64: east
    w.players()[0].type = 1;
    w.players()[0].flagman = f->id;
    w.action(*f, act::move, 1100, 450);
    for (int i = 0; i < 10; ++i) t.step(17);
    const double now = t.e->clock() + 5.0;
    const auto m = t.e->motion(*f, now);
    CHECK(m.x > 0.2);   // moving east, extrapolated
    r = {};
    r.width = 640; r.height = 480; r.extrapolate = true; r.renderClock = now;
    r.centerX = f->x - 586 + 320 - 10; r.centerY = 450;   // flagman in the right margin
    o = t.e->viewOrigin(r);
    CHECK(o && std::abs(o->x - (f->x + m.x - 586)) < 1e-9);
    r.centerX = f->x + 586 - 320 + 10;                      // in the left margin
    o = t.e->viewOrigin(r);
    CHECK(o && std::abs(o->x - (f->x + m.x - 639 + 586)) < 1e-9);
}

void testMotion() {
    World3 t;
    World& w = t.world();
    Sprite* s = w.createUnit(1, 200, 200, 0, 0, 64, nullptr);   // east
    w.action(*s, act::move, 900, 200);
    // 120 fps (8/8/9 ms); the drawn position at the frame clock, sampled before the tick as the
    // session does (draw, then tactWorld)
    std::vector<double> drawnX, rawX;
    int ms[3] = {8, 8, 9};
    for (int i = 0; i < 240; ++i) {
        t.e->beginFrame(ms[i % 3]);
        const double now = t.e->clock();
        drawnX.push_back(s->x + t.e->motion(*s, now).x);
        rawX.push_back(s->x);
        t.e->tactWorld();
    }
    // after the start (speed reached), steps are regular: within 15% of the mean step
    double maxDev = 0, mean = 0;
    int n = 0;
    for (std::size_t i = 60; i < drawnX.size(); ++i) { mean += drawnX[i] - drawnX[i - 1]; ++n; }
    mean /= n;
    for (std::size_t i = 60; i < drawnX.size(); ++i) {
        const double step = drawnX[i] - drawnX[i - 1];
        const double expect = mean * ms[i % 3] / (25.0 / 3);
        maxDev = std::max(maxDev, std::abs(step - expect));
    }
    int rawStill = 0;
    for (std::size_t i = 60; i < rawX.size(); ++i) rawStill += rawX[i] == rawX[i - 1];
    std::fprintf(stderr, "motion: mean step %.3f px/frame, max deviation %.4f px; without it %d of %d frames stand still\n",
                 mean, maxDev, rawStill, n);
    CHECK(mean > 0.3);
    CHECK(maxDev < 0.15 * mean + 0.02);
    CHECK(rawStill > n / 2);

    // at most one tick period ahead: a world that stops ticking stops the extrapolation
    const double before = s->x + t.e->motion(*s, t.e->clock() + 500).x;
    CHECK(before - s->x <= s->speed * (71 * 9.0 / 8 + 20) / 1048576.0 * 4096 + 1.0);
    // a teleport is drawn where the unit is (no interpolation across it)
    s->x = static_cast<std::int16_t>(s->x + 300);
    const double after = s->x + t.e->motion(*s, t.e->clock()).x;
    CHECK(after >= s->x && after - s->x < 8);
    // rays (instantaneous moves) and passive sprites are not extrapolated
    s->speed = fx::Instant;
    CHECK(t.e->motion(*s, t.e->clock() + 20).x == s->fracX / 128.0);
    s->speed = 60;
    s->state |= st::Passive;
    CHECK(t.e->motion(*s, t.e->clock() + 20).x == s->fracX / 128.0);
    s->state &= static_cast<std::uint16_t>(~st::Passive);
    // a new projectile is extrapolated from its first frame (its speed and direction)
    Sprite* p = w.createUnit(2, 300, 600, 10, 0, 0, nullptr);   // north
    p->speed = 400;
    const auto mp = t.e->motion(*p, t.e->clock() + 10);
    CHECK(mp.y < -3 && mp.y > -5);   // 400 * 4096 / 2^20 px/ms * 10 ms
    // a vehicle that will be refused its next step (another unit in front, the map's edge) is
    // drawn where it is, not pushed forward and pulled back at the tick
    Sprite* a = w.createUnit(4, 600, 700, 0, 0, 64, nullptr);
    w.createUnit(4, 609, 700, 0, 0, 0, nullptr);   // right in front (sizes 8)
    a->speed = 60;
    a->state |= st::Accelerate;   // driving (without it the physics brakes to 0)
    a->direction = 64;
    CHECK(t.e->motion(*a, t.e->clock() + 20).x == a->fracX / 128.0);
    a->direction = 192;   // west: free
    CHECK(t.e->motion(*a, t.e->clock() + 20).x < a->fracX / 128.0 - 0.5);
    Sprite* edge = w.createUnit(4, 1199, 100, 0, 0, 64, nullptr);
    edge->speed = 60;
    edge->state |= st::Accelerate;
    edge->direction = 64;   // east, out of the 1200 px map
    CHECK(t.e->motion(*edge, t.e->clock() + 20).x == edge->fracX / 128.0);
    edge->direction = 192;  // west: free
    CHECK(t.e->motion(*edge, t.e->clock() + 20).x < edge->fracX / 128.0 - 0.5);
    edge->direction = 64;
    CHECK(t.e->motion(*edge, t.e->clock() + 20).x == edge->fracX / 128.0);
    // pause: nothing moves
    t.e->callBuiltin(89, std::array{script::Value{1}});
    CHECK(t.e->motion(*p, t.e->clock() + 10).y == p->fracY / 128.0);
}

// ---------------------------------------------------------------------------- input mapping
void testMapping() {
    InputRecordBuilder a, b;
    input::PcState pc;
    for (const auto cursor : {input::Vec2{0, 0}, input::Vec2{17.6f, 300.2f}, input::Vec2{639.9f, 479.9f}, input::Vec2{120.5f, 3.5f}})
        for (const bool order : {false, true})
            for (const bool edge : {true, false}) {
                pc.cursor = cursor;
                pc.moveOrder = order;
                pc.moveTarget = {cursor.x + 40.7f, cursor.y - 900.2f};
                pc.edgeScroll = edge;
                pc.left = {true, true, false};
                // identity: the original's build(cursor - viewX, camera)
                const int viewX = 107, camX = 1234, camY = 567;
                const OriginalInput old = a.build(pc, pc.cursorX() - viewX, pc.cursorY(), camX, camY, 0.016f, {640, 480});
                const OriginalInput now = b.build(pc, ScreenMapping{double(viewX), double(camX), double(camY), 1.0}, 0.016f, {640, 480});
                CHECK_EQ(old.mouseWorldX, now.mouseWorldX);
                CHECK_EQ(old.mouseWorldY, now.mouseWorldY);
                CHECK_EQ(old.mouseScreenX, now.mouseScreenX);
                CHECK_EQ(old.mouseScreenY, now.mouseScreenY);
                CHECK_EQ(old.bits, now.bits);
            }
    // zoom 2 from the sub-pixel origin (100.5, 50.25): the cursor at output (300, 200) points at
    // world (250.5, 150.25); the engine's view sees it at (150, 100)
    InputRecordBuilder c;
    pc = {};
    pc.cursor = {300, 200};
    const ScreenMapping z2{0, 100.5, 50.25, 2.0};
    OriginalInput r = c.build(pc, z2, 0.016f, {320, 240});
    CHECK_EQ(r.mouseWorldX, 250);
    CHECK_EQ(r.mouseWorldY, 150);
    CHECK_EQ(r.mouseScreenX, 150);
    CHECK_EQ(r.mouseScreenY, 100);
    CHECK_EQ(z2.outputX(250.5), 300.0);
    CHECK_EQ(z2.worldY(200), 150.25);
    // zoom 0.5: the right edge of the output is the right edge of the engine's 1280 px view
    pc.cursor = {639, 10};
    r = c.build(pc, ScreenMapping{0, 0, 0, 0.5}, 0.016f, {1280, 960});
    CHECK_EQ(r.mouseScreenX, 1278);
    CHECK(r.mouseScreenX >= 1280 - 6);   // edge scroll band of the engine (0x415e8d)
    // a cursor that must not scroll stays out of the band, in the engine's view units
    pc.edgeScroll = false;
    r = c.build(pc, ScreenMapping{0, 0, 0, 0.5}, 0.016f, {1280, 960});
    CHECK_EQ(r.mouseScreenX, 1280 - 1 - InputRecordBuilder::EdgeBand);
    CHECK_EQ(r.mouseWorldX, 1278);
    // a move order (direct scheme) is a left click at its target, through the zoom
    pc = {};
    pc.cursor = {10, 10};
    pc.moveOrder = true;
    pc.moveTarget = {400, 100};
    pc.right = {true, true, false};
    r = c.build(pc, z2, 0.016f, {320, 240});
    CHECK_EQ(r.mouseWorldX, 300);
    CHECK_EQ(r.mouseWorldY, 100);
    CHECK_EQ(r.bits & (OriginalInput::LClick | OriginalInput::LDown | OriginalInput::RClick | OriginalInput::RDown),
             OriginalInput::LClick | OriginalInput::LDown);
}

// ---------------------------------------------------------------------------- composite
void testComposite() {
    constexpr int k = 3, LW = 8, LH = 6, WW = LW * k, WH = LH * k;
    std::vector<RGBA8> world(WW * WH), logical(LW * LH);
    std::vector<std::uint32_t> out(WW * WH);
    for (int y = 0; y < WH; ++y)
        for (int x = 0; x < WW; ++x)
            world[static_cast<std::size_t>(y * WW + x)] = hc::toPixel(hc::pack(x, y, 3));   // detail inside every block
    const auto xrgb = [](const RGBA8& p) { return (std::uint32_t{p.r} << 16) | (std::uint32_t{p.g} << 8) | p.b; };
    const auto at = [&](int x, int y) { return out[static_cast<std::size_t>(y * WW + x)]; };
    FramebufferRef W{world.data(), WH, WW}, L{logical.data(), LH, LW};
    view::HiResComposite c;
    c.beginLogical(W, L, k);
    CHECK(same(logical[0], world[static_cast<std::size_t>(1 * WW + 1)]));   // centre sample
    logical[static_cast<std::size_t>(2 * LW + 3)] = RGBA8{{255, 255, 255}, 255};  // a UI pixel
    c.beforeEffects(L);
    c.afterEffects(L, {});
    c.compose(W, L, out.data(), WW);
    for (int yy = 0; yy < k; ++yy)
        for (int xx = 0; xx < k; ++xx) CHECK_EQ(at(3 * k + xx, 2 * k + yy), 0xFFFFFFu);
    for (int y = 0; y < WH; ++y)                     // the world, with its detail, everywhere else
        for (int x = 0; x < WW; ++x)
            if (x / k != 3 || y / k != 2) CHECK_EQ(at(x, y), xrgb(world[static_cast<std::size_t>(y * WW + x)]));
    // Effect() fade on the whole picture: world pixels get the operation one by one
    c.beginLogical(W, L, k);
    c.beforeEffects(L);
    for (auto& p : logical) p = hc::toPixel(ScreenEffects::applyOp(5, 4, hc::fromPixel(p)));
    c.afterEffects(L, ScreenEffectView{5, 4});
    c.compose(W, L, out.data(), WW);
    CHECK_EQ(at(5, 4), xrgb(hc::toPixel(ScreenEffects::applyOp(5, 4, hc::fromPixel(world[static_cast<std::size_t>(4 * WW + 5)])))));
    // a UI pixel drawn after the effects (HUD) is shown as drawn
    c.beginLogical(W, L, k);
    c.beforeEffects(L);
    c.afterEffects(L, {});
    logical[static_cast<std::size_t>(5 * LW + 7)] = RGBA8{{1, 2, 3}, 255};
    c.compose(W, L, out.data(), WW);
    CHECK_EQ(at(7 * k + 1, 5 * k + 2), 0x010203u);
    // eff_openwindow: outside the window, the previous screen
    c.beginLogical(W, L, k);
    c.beforeEffects(L);
    logical[0] = RGBA8{{9, 9, 9}, 255};   // the old screen there
    ScreenEffectView win;
    win.window = true; win.left = 2; win.right = 5; win.top = 1; win.bottom = 4;
    c.afterEffects(L, win);
    c.compose(W, L, out.data(), WW);
    CHECK_EQ(at(1, 1), 0x090909u);
    CHECK_EQ(at(3 * k + 1, 2 * k + 1), xrgb(world[static_cast<std::size_t>((2 * k + 1) * WW + 3 * k + 1)]));   // inside: the world
}

} // namespace

int main() {
    testRasterIdentityAndScale();
    testRasterSeams();
    testEngineDrawView();
    testViewOrigin();
    testMotion();
    testMapping();
    testComposite();
    return gromada_test::finish();
}
