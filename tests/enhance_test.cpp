// Graphics enhancements (issue #21, src/game/enhance) on synthetic data (no game files):
// core options, emitter classification from the act_fight/act_death chains, per-frame
// emission and contact-shadow kernels from encoded frames (formats 2, 4, 8), and the two
// passes on a framebuffer: off = untouched, shadows only darken around the base, lights only
// brighten around the source, no source = no pixel visited, the same picture at 2x.
#include "game/enhance/enhance.hpp"
#include "libgromada/libgromada_all.hpp"
#include "test.hpp"

#include <cstring>
#include <functional>
#include <map>

using namespace gromada;
using namespace gromada::game;

namespace {

// ---- frame encoders (the formats DecodeFrame reads) ----
using PixelFn = std::function<int(int x, int y)>;   // -1 = transparent

void header(std::vector<std::byte>& d, int h) {
    for (const int v : {0, h}) {
        d.push_back(static_cast<std::byte>(v & 0xff));
        d.push_back(static_cast<std::byte>(v >> 8));
    }
}

// Format 2: per row control bytes (command << 6 | count): 0 skip, 2 literal, 3 repeat; 0 ends the row.
std::vector<std::byte> encode2(int w, int h, const PixelFn& px) {
    std::vector<std::byte> d;
    header(d, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w;) {
            const int c = px(x, y);
            int n = 1;
            while (x + n < w && n < 63 && px(x + n, y) == c) ++n;
            if (c < 0) d.push_back(static_cast<std::byte>(n));
            else {
                d.push_back(static_cast<std::byte>(3 << 6 | n));
                d.push_back(static_cast<std::byte>(c));
            }
            x += n;
        }
        d.push_back(std::byte{0});
    }
    return d;
}

// Format 8: (opacity << 5 | count), opacity 7 opaque, 1..6 blended, 0 skip; then the indices.
std::vector<std::byte> encode8(int w, int h, const PixelFn& px, int opacity) {
    std::vector<std::byte> d;
    header(d, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w;) {
            const bool on = px(x, y) >= 0;
            int n = 1;
            while (x + n < w && n < 31 && (px(x + n, y) >= 0) == on) ++n;
            d.push_back(static_cast<std::byte>((on ? opacity : 0) << 5 | n));
            if (on)
                for (int k = 0; k < n; ++k) d.push_back(static_cast<std::byte>(px(x + k, y)));
            x += n;
        }
        d.push_back(std::byte{0});
    }
    return d;
}

// Format 4: u16 (r << 13 | g << 10 | b << 7 | count), all factors 0 = skip; 0 ends the row.
std::vector<std::byte> encode4(int w, int h, const std::function<bool(int, int)>& on, int r, int g, int b) {
    std::vector<std::byte> d;
    header(d, h);
    const auto word = [&](int v) {
        d.push_back(static_cast<std::byte>(v & 0xff));
        d.push_back(static_cast<std::byte>(v >> 8));
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w;) {
            const bool o = on(x, y);
            int n = 1;
            while (x + n < w && n < 127 && on(x + n, y) == o) ++n;
            word(o ? (r << 13 | g << 10 | b << 7 | n) : n);
            x += n;
        }
        word(0);
    }
    return d;
}

// A one-frame graphics owning its data.
struct Gfx {
    VidGraphics g;
    std::vector<std::byte> data;
    Gfx(int fmt, int w, int h, std::vector<std::byte> bytes) : data(std::move(bytes)) {
        g.dataFormat = static_cast<std::uint8_t>(fmt);
        g.width = static_cast<std::uint16_t>(w);
        g.height = static_cast<std::uint16_t>(h);
        g.numOfFrames = 1;
        for (int i = 0; i < 256; ++i) g.palette[static_cast<std::size_t>(i)] = {static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(i)};
        g.palette[1] = {255, 150, 30};    // fire orange
        g.palette[2] = {150, 150, 150};   // grey smoke
        g.palette[3] = {90, 80, 60};      // dull building
    }
    Gfx(const Gfx&) = delete;
    [[nodiscard]] VidGraphics::Frame frame() const { return {std::span<const std::byte>{data}, &g}; }
};

const auto disc = [](int cx, int cy, int r, int c) {
    return [=](int x, int y) { return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r ? c : -1; };
};

struct Picture {
    int w, h;
    std::vector<RGBA8> px;
    Picture(int w_, int h_, std::uint8_t grey) : w(w_), h(h_), px(static_cast<std::size_t>(w_ * h_), RGBA8{{grey, grey, grey}, 255}) {}
    FramebufferRef fb() { return FramebufferRef{px.data(), h, w}; }
    const RGBA8& at(int x, int y) const { return px[static_cast<std::size_t>(y * w + x)]; }
};

int lum(const RGBA8& p) { return p.r + p.g + p.b; }
bool same(const Picture& a, const Picture& b) {
    return a.px.size() == b.px.size() && std::memcmp(a.px.data(), b.px.data(), a.px.size() * sizeof(RGBA8)) == 0;
}

void testOptions() {
    std::map<std::string, std::string> vars;
    const auto get = [&](const char* k) -> const char* {
        auto f = vars.find(k);
        return f == vars.end() ? nullptr : f->second.c_str();
    };
    CHECK(enhance::readSettings(get) == enhance::Settings{});
    CHECK(!enhance::readSettings(get).any());
    vars["gromada_fx_lights"] = "normali";
    vars["gromada_fx_contact_shadows"] = "marcate";
    CHECK((enhance::readSettings(get) == enhance::Settings{2, 3}));
    vars["gromada_fx_lights"] = "intense";
    vars["gromada_fx_contact_shadows"] = "disattivate";
    CHECK((enhance::readSettings(get) == enhance::Settings{3, 0}));
    vars["gromada_fx_lights"] = "???";
    CHECK(enhance::readSettings(get).lights == 0);
    // the option strings: key, description, then the default first ("disattivate")
    for (const auto& o : enhance::coreOptions) {
        const std::string v = o.value;
        CHECK(v.find("; disattivate|") != std::string::npos);
    }
}

void testEmitters() {
    std::vector<Vid> vids(8);
    vids[0].type = static_cast<ObjectClass>(2);   // a vehicle: fires vid 1, dies into vid 3
    vids[0].childNvid[enhance::ActFight] = 1;
    vids[0].childNvid[enhance::ActDeath] = -3;   // negative = the same vid (abs, like World)
    vids[1].type = static_cast<ObjectClass>(9);   // muzzle effect of another class, spawns 2 on birth
    vids[1].childNvid[14] = 2;
    vids[2].type = static_cast<ObjectClass>(1);
    vids[3].type = static_cast<ObjectClass>(1);
    vids[4].type = static_cast<ObjectClass>(1);   // unrelated static: not an emitter
    vids[5].type = static_cast<ObjectClass>(enhance::vclass::Effect1);   // effect class: candidate
    vids[6].type = static_cast<ObjectClass>(2);   // the vehicle itself is not reachable
    std::vector<const Vid*> ptr;
    for (const auto& v : vids) ptr.push_back(&v);
    ptr.push_back(nullptr);   // a missing vid is tolerated
    const auto e = enhance::classifyEmitters(ptr);
    CHECK_EQ(e.size(), std::size_t{9});
    CHECK(!e[0]);
    CHECK(e[1] && e[2] && e[3]);
    CHECK(!e[4]);
    CHECK(e[5]);
    CHECK(!e[6] && !e[7] && !e[8]);
}

void testEmission() {
    // fire (format 8, opaque orange): emits, warm colour, centroid at the disc centre
    Gfx fire(8, 32, 32, encode8(32, 32, disc(16, 12, 6, 1), 7));
    const auto e = enhance::frameEmission(fire.frame(), enhance::vclass::Effect1);
    CHECK(e.power > enhance::MinLightPower);
    CHECK(e.r == 1.0f && e.g > e.b && e.g < 1.0f);
    CHECK(std::abs(e.cx - 16.5f) < 1.0f && std::abs(e.cy - 12.5f) < 1.0f);
    // smoke (grey, half transparent): no light
    Gfx smoke(8, 32, 32, encode8(32, 32, disc(16, 16, 10, 2), 3));
    CHECK(enhance::frameEmission(smoke.frame(), enhance::vclass::Effect1).power < enhance::MinLightPower);
    // light pixels of the original (format 4, blue level 4): blue light
    Gfx light(4, 40, 40, encode4(40, 40, [](int x, int y) { return (x - 20) * (x - 20) + (y - 20) * (y - 20) < 225; }, 0, 0, 4));
    const auto l = enhance::frameEmission(light.frame(), enhance::vclass::Projectile);
    CHECK(l.power > enhance::MinLightPower);
    CHECK(l.b == 1.0f && l.r == 0.0f && l.g == 0.0f);
    CHECK(l.spread > 5.0f);
    // opaque format 2: a small projectile glows, a large sprite (debris, wreck) never does
    Gfx bolt(2, 8, 8, encode2(8, 8, disc(4, 4, 3, 1)));
    CHECK(enhance::frameEmission(bolt.frame(), enhance::vclass::Projectile).power > 0);
    Gfx wreck(2, 64, 64, encode2(64, 64, disc(32, 32, 20, 1)));
    CHECK_EQ(enhance::frameEmission(wreck.frame(), enhance::vclass::Projectile).power, 0.0f);
    CHECK_EQ(enhance::frameEmission(bolt.frame(), 1).power, 0.0f);   // not a projectile class
}

// A 60x60 "building": a dull disc whose bottom (frame row 40) is the front edge of its
// footprint (sizeY 20: frame rows 20..40, the frame centre is the sprite position).
struct Building {
    Gfx gfx{2, 60, 60, encode2(60, 60, disc(30, 24, 16, 3))};
    enhance::DrawItem item(float x, float y) const {
        enhance::DrawItem it;
        it.frame = &frame;
        it.x = x;
        it.y = y;
        it.nvid = 0;
        it.cls = 3;
        it.category = enhance::vcat::Vehicle;
        it.sizeX = 30;
        it.sizeY = 20;
        it.sizeZ = 30;
        return it;
    }
    VidGraphics::Frame frame = gfx.frame();
};

void testKernel() {
    Building b;
    const auto k = enhance::occlusionKernel(b.frame, 30, 20, 30);
    CHECK(!k.empty());
    CHECK(k.w > 30 / enhance::OcclusionKernel::Cell && k.h > 20 / enhance::OcclusionKernel::Cell);
    // zero border (the splat reads neighbours without checks)
    for (int x = 0; x < k.w; ++x) CHECK(k.v[static_cast<std::size_t>(x)] == 0 && k.v[static_cast<std::size_t>((k.h - 1) * k.w + x)] == 0);
    for (int y = 0; y < k.h; ++y) CHECK(k.v[static_cast<std::size_t>(y * k.w)] == 0 && k.v[static_cast<std::size_t>(y * k.w + k.w - 1)] == 0);
    // the kernel covers the footprint rows below the centre, not the top of the disc
    std::fprintf(stderr, "kernel %dx%d at (%.0f, %.0f)\n", k.w, k.h, k.ox, k.oy);
    CHECK(k.oy >= -24.0f && k.oy + static_cast<float>(k.h * enhance::OcclusionKernel::Cell) > 10.0f);
    // light-only frames have no silhouette
    Gfx light(4, 40, 40, encode4(40, 40, [](int, int) { return true; }, 3, 3, 3));
    CHECK(enhance::occlusionKernel(light.frame(), 30, 20, 30).empty());
}

void testPasses() {
    Building b;
    Gfx fire(8, 32, 32, encode8(32, 32, disc(16, 16, 6, 1), 7));
    enhance::DrawItem flame;
    flame.frame = nullptr;
    const VidGraphics::Frame fireFrame = fire.frame();
    flame.frame = &fireFrame;
    flame.x = 200;
    flame.y = 100;
    flame.nvid = 1;
    flame.cls = enhance::vclass::Effect1;
    flame.category = 16;
    std::vector<Vid> vids(2);
    vids[1].type = static_cast<ObjectClass>(enhance::vclass::Effect1);
    std::vector<const Vid*> ptr{&vids[0], &vids[1]};
    enhance::Enhancer fx{ptr};
    CHECK(fx.isEmitter(1) && !fx.isEmitter(0) && !fx.isEmitter(7) && !fx.isEmitter(-1));
    const enhance::View view{0, 0, 1};
    const std::vector<enhance::DrawItem> occ{b.item(80, 100)};
    const std::vector<enhance::DrawItem> lit{flame};

    // all off: nothing changes
    Picture ref(320, 200, 120);
    {
        Picture p(320, 200, 120);
        fx.setSettings({});
        fx.begin();
        fx.ground(p.fb(), view, occ);
        fx.lights(p.fb(), view, lit);
        CHECK(same(p, ref));
    }
    // contact shadows: darker just below the base, untouched far away, never brighter
    Picture s1(320, 200, 120);
    fx.setSettings({0, 2});
    fx.begin();
    fx.ground(s1.fb(), view, occ);
    CHECK(fx.stats().occluders == 1 && fx.stats().groundPixels > 0);
    CHECK(lum(s1.at(80, 113)) < lum(ref.at(80, 113)) - 30);   // just below the front edge of the base (y 110)
    CHECK(lum(s1.at(80, 40)) == lum(ref.at(80, 40)));         // above the building: no footprint there
    CHECK(s1.at(300, 190).r == 120);
    bool brighter = false;
    for (std::size_t i = 0; i < s1.px.size(); ++i) brighter |= lum(s1.px[i]) > lum(ref.px[i]);
    CHECK(!brighter);
    // an occluder high above the ground casts no contact shadow
    {
        Picture p(320, 200, 120);
        auto high = occ;
        high[0].z = 40;
        fx.begin();
        fx.ground(p.fb(), view, high);
        CHECK(same(p, ref));
    }
    // lights: brighter and warm around the source, untouched far away, never darker
    Picture l1(320, 200, 120);
    fx.setSettings({2, 0});
    fx.begin();
    fx.lights(l1.fb(), view, lit);
    CHECK(fx.stats().lights == 1);
    const RGBA8 c = l1.at(200, 100), n = l1.at(200 + 18, 100);
    CHECK(c.r > 150 && c.r > c.b + 20);
    CHECK(n.r > 120 && n.r < c.r);
    CHECK(l1.at(10, 10).r == 120 && l1.at(310, 190).r == 120);
    bool darker = false;
    for (std::size_t i = 0; i < l1.px.size(); ++i) darker |= lum(l1.px[i]) < lum(ref.px[i]);
    CHECK(!darker);
    // no source: no pixel visited
    {
        Picture p(320, 200, 120);
        fx.begin();
        fx.lights(p.fb(), view, occ);   // the building is not an emitter
        CHECK(fx.stats().lights == 0 && fx.stats().lightPixels == 0);
        CHECK(same(p, ref));
    }
    // the camera moves the effect with the world
    {
        Picture p(320, 200, 120);
        fx.begin();
        fx.lights(p.fb(), enhance::View{100, 0, 1}, lit);
        CHECK(p.at(100, 100).r == c.r && p.at(200, 100).r == 120);
    }
    // 2x framebuffer: the same picture at twice the resolution (within upsampling rounding)
    {
        Picture s2(640, 400, 120), l2(640, 400, 120);
        fx.setSettings({2, 2});
        fx.begin();
        fx.ground(s2.fb(), enhance::View{0, 0, 2}, occ);
        fx.lights(l2.fb(), enhance::View{0, 0, 2}, lit);
        int worstS = 0, worstL = 0;
        for (int y = 0; y < 200; y += 3)
            for (int x = 0; x < 320; x += 3) {
                worstS = std::max(worstS, std::abs(s2.at(2 * x, 2 * y).r - s1.at(x, y).r));
                worstL = std::max(worstL, std::abs(l2.at(2 * x, 2 * y).r - l1.at(x, y).r));
            }
        CHECK(worstS <= 12);
        CHECK(worstL <= 12);
        std::fprintf(stderr, "2x vs 1x: shadows max |dr| %d, lights max |dr| %d\n", worstS, worstL);
    }
    // analog zoom: another scale in between leaves no residue (same picture as before)
    {
        fx.setSettings({2, 2});
        for (const float sc : {1.37f, 0.61f, 1.0f}) {
            Picture s(320, 200, 120), l(320, 200, 120);
            fx.begin();
            fx.ground(s.fb(), enhance::View{0, 0, sc}, occ);
            fx.lights(l.fb(), enhance::View{0, 0, sc}, lit);
            CHECK(fx.stats().lights == 1 && fx.stats().occluders == 1);
            if (sc == 1.0f) {
                CHECK(same(s, s1));
                CHECK(same(l, l1));
            }
        }
    }
    // a 16:9-sized and an odd-sized framebuffer, sources partly outside: no out-of-bounds
    for (const auto [w, h] : {std::pair{854, 480}, std::pair{333, 77}}) {
        Picture p(w, h, 60);
        auto edge = occ;
        edge.push_back(b.item(-10, -5));
        edge.push_back(b.item(static_cast<float>(w) + 5, static_cast<float>(h) + 10));
        auto lights = lit;
        lights.push_back(flame);
        lights.back().x = static_cast<float>(w) - 2;
        lights.back().y = static_cast<float>(h) - 1;
        fx.setSettings({3, 3});
        fx.begin();
        fx.ground(p.fb(), view, edge);
        fx.lights(p.fb(), view, lights);
        CHECK(fx.stats().lights == 2);
    }
}

} // namespace

int main() {
    testOptions();
    testEmitters();
    testEmission();
    testKernel();
    testPasses();
    return gromada_test::finish();
}
