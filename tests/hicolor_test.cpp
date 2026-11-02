// Issue #15: the 16-bit colour model (src/game/hicolor.hpp) and the screen effects
// (src/game/screen_effects.hpp) against vectors produced by running the original gromada.exe
// in an emulator (tools/oracle/gen_hicolor.py): SVGA tables, RGB conversion, Effect() pixel
// operations, SetGamma on a palette and putcadr on synthetic frames. No game data needed.
#include "game/hicolor.hpp"
#include "game/screen_effects.hpp"
#include "test.hpp"

#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace hc = gromada::game::hicolor;

namespace {

// ---- minimal JSON reader (objects, arrays, integers, strings) -----------------------------
struct Json {
    enum Kind { Null, Num, Str, Arr, Obj } kind = Null;
    std::int64_t num = 0;
    std::string str;
    std::vector<Json> arr;
    std::map<std::string, Json> obj;
    const Json& at(const std::string& k) const {
        static const Json none;
        auto it = obj.find(k);
        return it == obj.end() ? none : it->second;
    }
};

struct Parser {
    std::string_view s;
    std::size_t p = 0;
    bool ok = true;
    void ws() { while (p < s.size() && (s[p] == ' ' || s[p] == '\n' || s[p] == '\r' || s[p] == '\t')) ++p; }
    bool eat(char c) { ws(); if (p < s.size() && s[p] == c) { ++p; return true; } return false; }
    std::string string() {
        std::string out;
        if (!eat('"')) { ok = false; return out; }
        while (p < s.size() && s[p] != '"') { if (s[p] == '\\' && p + 1 < s.size()) ++p; out += s[p++]; }
        ++p;
        return out;
    }
    Json value() {
        Json j;
        ws();
        if (p >= s.size()) { ok = false; return j; }
        const char c = s[p];
        if (c == '{') {
            ++p; j.kind = Json::Obj;
            if (eat('}')) return j;
            do { std::string k = string(); if (!eat(':')) { ok = false; return j; } j.obj[k] = value(); } while (ok && eat(','));
            if (!eat('}')) ok = false;
        } else if (c == '[') {
            ++p; j.kind = Json::Arr;
            if (eat(']')) return j;
            do { j.arr.push_back(value()); } while (ok && eat(','));
            if (!eat(']')) ok = false;
        } else if (c == '"') {
            j.kind = Json::Str; j.str = string();
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            j.kind = Json::Num;
            const bool neg = c == '-';
            if (neg) ++p;
            std::int64_t v = 0;
            while (p < s.size() && s[p] >= '0' && s[p] <= '9') v = v * 10 + (s[p++] - '0');
            j.num = neg ? -v : v;
        } else {
            ok = false;
        }
        return j;
    }
};

Json load(const std::string& name) {
    std::ifstream in(std::string(GROMADA_ORACLE_DIR) + "/" + name + ".json", std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    Parser p{text};
    Json j = p.value();
    if (!p.ok || j.kind != Json::Obj) { std::fprintf(stderr, "bad or missing %s.json\n", name.c_str()); ++gromada_test::failures; }
    return j;
}

std::vector<std::int64_t> ints(const Json& a) {
    std::vector<std::int64_t> out;
    for (const auto& v : a.arr) out.push_back(v.num);
    return out;
}

std::uint32_t fnv(const std::vector<hc::Color16>& w) {
    std::uint32_t h = 0x811C9DC5u;
    for (const auto v : w) h = (h ^ v) * 0x01000193u;
    return h;
}

struct Tally {
    const char* name;
    int cases = 0, bad = 0;
    void check(bool good, std::int64_t index, std::int64_t expected, std::int64_t got) {
        ++cases;
        if (good) return;
        if (bad < 8) std::fprintf(stderr, "  %s case %lld: expected %lld got %lld\n", name, (long long)index, (long long)expected, (long long)got);
        ++bad;
    }
    ~Tally() {
        std::fprintf(stderr, "%-18s %6d checks, %d mismatches\n", name, cases, bad);
        gromada_test::failures += bad;
        if (!cases) ++gromada_test::failures;
    }
};

void testTables() {
    const Json j = load("hicolor_tables");
    const Json& a = j.at("arrays");
    CHECK(ints(a.at("shifts")) == (std::vector<std::int64_t>{hc::RShift, hc::GShift, hc::BShift}));
    CHECK(ints(a.at("bits")) == (std::vector<std::int64_t>{hc::RBits, hc::GBits, hc::BBits}));
    const auto half = ints(a.at("halfMask")), grey = ints(a.at("grey"));
    CHECK(half.size() == 1 && half[0] == hc::HalfMask);
    CHECK(grey.size() == 1 && grey[0] == hc::Grey);
    {
        Tally t{"fade masks"};
        const auto m = ints(a.at("fadeMasks"));   // dwords at +0x22f..+0x23f: 5 removed bits .. 1
        for (int i = 0; i < 5; ++i) {
            const std::uint32_t w = hc::fadeMask(5 - i);
            t.check(m[static_cast<std::size_t>(i)] == static_cast<std::int64_t>(w | (w << 16)), i, m[static_cast<std::size_t>(i)], w | (w << 16));
        }
    }
    {
        Tally t{"blue brighten"};
        const auto b = ints(a.at("blueBrighten"));
        for (int L = 0; L < 8; ++L)
            for (int i = 0; i < 32; ++i)
                t.check(b[static_cast<std::size_t>(L * 32 + i)] == hc::brightenChannel(i, L, 5), L * 32 + i, b[static_cast<std::size_t>(L * 32 + i)], hc::brightenChannel(i, L, 5));
    }
    {
        Tally t{"darken"};
        const auto d = ints(a.at("darken"));
        for (int k = 1; k <= 6; ++k)
            for (int hi = 0; hi < 32; ++hi)
                for (int mid = 0; mid < 64; ++mid) {
                    const auto idx = static_cast<std::size_t>((6 - k) * 2048 + (hi << 6) + mid);
                    const int want = (hc::darkenChannel(hi, k) << 6) | hc::darkenChannel(mid, k);
                    t.check(d[idx] == want, static_cast<std::int64_t>(idx), d[idx], want);
                }
    }
    {
        Tally t{"blend"};
        const auto b = ints(a.at("blend"));
        for (int op = 1; op <= 6; ++op)
            for (int src = 0; src < 32; ++src)
                for (int dst = 0; dst < 32; ++dst) {
                    const auto idx = static_cast<std::size_t>(((op - 1) << 10) + (src << 5) + dst);
                    t.check(b[idx] == hc::blendChannel(src, dst, op), static_cast<std::int64_t>(idx), b[idx], hc::blendChannel(src, dst, op));
                }
    }
    {
        Tally t{"rg brighten"};
        const auto h = ints(a.at("rgBrightenHash"));
        for (int tab = 0; tab < 64; ++tab) {
            std::vector<hc::Color16> w(2048);
            for (int i = 0; i < 2048; ++i)
                w[static_cast<std::size_t>(i)] = static_cast<hc::Color16>(
                    ((hc::brightenChannel(i >> 6, tab >> 3, 5) << 6) | hc::brightenChannel(i & 63, tab & 7, 6)) << 5);
            t.check(h[static_cast<std::size_t>(tab)] == fnv(w), tab, h[static_cast<std::size_t>(tab)], fnv(w));
        }
    }
    {
        Tally t{"rgb -> 565"};
        std::int64_t i = 0;
        for (const auto& c : j.at("cases").arr) {
            const auto v = ints(c);
            const auto got = hc::fromRgb8(static_cast<std::uint8_t>(v[0]), static_cast<std::uint8_t>(v[1]), static_cast<std::uint8_t>(v[2]));
            t.check(got == v[3], i++, v[3], got);
        }
    }
}

std::vector<hc::Color16> allColours() {
    std::vector<hc::Color16> v(65536);
    for (int i = 0; i < 65536; ++i) v[static_cast<std::size_t>(i)] = static_cast<hc::Color16>(i);
    return v;
}

void testEffects() {
    const Json j = load("hicolor_effects");
    Tally t{"effect ops"};
    for (const auto& c : j.at("cases").arr) {
        const auto v = ints(c);
        const int kind = static_cast<int>(v[0]), level = static_cast<int>(v[1]);
        auto w = allColours();
        for (auto& p : w) p = gromada::game::ScreenEffects::applyOp(kind, level, p);
        t.check(fnv(w) == static_cast<std::uint32_t>(v[2]), kind * 1000 + level, v[2], fnv(w));
        t.check(w[0x1234] == v[3] && w[0xffff] == v[4] && w[0x8410] == v[5] && w[0x7bef] == v[6], kind * 1000 + level, v[3], w[0x1234]);
    }
}

void testGamma() {
    const Json j = load("hicolor_gamma");
    const Json& a = j.at("arrays");
    std::vector<hc::Color16> pal;
    for (const auto x : ints(a.at("palette"))) pal.push_back(static_cast<hc::Color16>(x));
    Tally t{"gamma"};
    for (const auto& c : j.at("cases").arr) {
        const auto v = ints(c);
        auto p = pal;
        hc::applyGamma(p, static_cast<int>(v[0]));
        t.check(fnv(p) == static_cast<std::uint32_t>(v[1]), v[0], v[1], fnv(p));
        const Json& full = a.at("g" + std::to_string(v[0]));
        if (full.kind == Json::Arr) {
            const auto f = ints(full);
            for (std::size_t i = 0; i < f.size(); ++i) t.check(f[i] == p[i], v[0] * 1000 + static_cast<std::int64_t>(i), f[i], p[i]);
        }
    }
}

// Format 4 runs with blue factor 0 and a red/green factor take the path 0x4021c0 of the
// original, which overwrites the low word of the table pointer with the pixel (`mov bx, ax`)
// and reads a heap-address-dependent entry: not reproducible, we implement the intended
// brightening (docs/engine.md D5). Marks the surface pixels such runs cover.
void markFormat4Bug(const std::vector<std::byte>& d, int x0, int y0, int fbw, int fbh, std::vector<bool>& mask) {
    const auto u16 = [&](std::size_t p) { return std::to_integer<int>(d[p]) | (std::to_integer<int>(d[p + 1]) << 8); };
    const int startY = u16(0), lines = u16(2);
    std::size_t p = 4;
    for (int ly = startY; ly < startY + lines; ++ly) {
        int x = 0;
        for (int w; (w = u16(p)), p += 2, (w & 0x7f) != 0;) {
            const int n = w & 0x7f;
            if (((w >> 7) & 7) == 0 && (w >> 10) != 0)
                for (int i = 0; i < n; ++i) {
                    const int px = x0 + x + i, py = y0 + ly;
                    if (px >= 0 && px < fbw && py >= 0 && py < fbh) mask[static_cast<std::size_t>(py * fbw + px)] = true;
                }
            x += n;
        }
    }
}

void testPutcadr() {
    const Json j = load("hicolor_putcadr");
    const Json& a = j.at("arrays");
    constexpr int W = 40, H = 12, FBW = 56, FBH = 18;
    std::array<hc::Color16, 256> pal16{};
    {
        const auto p = ints(a.at("palette"));
        for (std::size_t i = 0; i < 256; ++i) pal16[i] = static_cast<hc::Color16>(p[i]);
    }
    const auto palette = hc::expand(pal16);
    const auto background = ints(a.at("background"));
    std::map<int, VidGraphics> gfx;
    std::map<int, std::vector<std::byte>> data;
    for (const int fmt : {2, 3, 4, 8}) {
        auto& d = data[fmt];
        for (const auto b : ints(a.at("frame" + std::to_string(fmt)))) d.push_back(static_cast<std::byte>(b));
        auto& g = gfx[fmt];
        g.dataFormat = static_cast<std::uint8_t>(fmt);
        g.width = W;
        g.height = H;
        g.numOfFrames = 1;
    }
    Tally t{"putcadr"};
    std::vector<RGBA8> pixels(FBW * FBH);
    int index = 0, skipped = 0;
    for (const auto& c : j.at("cases").arr) {
        const auto v = ints(c);
        const int fmt = static_cast<int>(v[0]);
        const bool castShadow = v[1] != 0 && v[4] > 0;
        const int x = static_cast<int>(v[2]), y = static_cast<int>(v[3]), z = static_cast<int>(v[4]);
        for (std::size_t i = 0; i < pixels.size(); ++i) pixels[i] = hc::toPixel(static_cast<hc::Color16>(background[i]));
        FramebufferRef fb{pixels.data(), FBH, FBW};
        const VidGraphics::Frame frame{std::span<const std::byte>{data[fmt]}, &gfx[fmt]};
        if (castShadow)
            hc::drawFrame(frame, x - (z >> 1) - (z >> 2), y + (z >> 2) - (z >> 1), palette, fb, hc::FrameVisitor::Pass::CastShadow);
        hc::drawFrame(frame, x, y - z, palette, fb, hc::FrameVisitor::Pass::Normal, castShadow);
        std::vector<bool> skip(pixels.size());
        if (fmt == 4) {
            if (castShadow) markFormat4Bug(data[fmt], x - (z >> 1) - (z >> 2), y + (z >> 2) - (z >> 1), FBW, FBH, skip);
            markFormat4Bug(data[fmt], x, y - z, FBW, FBH, skip);
        }
        int bad = 0;
        std::int64_t firstExpected = 0, firstGot = 0;
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            if (skip[i]) { ++skipped; continue; }
            const auto got = hc::fromPixel(pixels[i]);
            if (got != v[5 + i]) {
                if (!bad) { firstExpected = v[5 + i] | (static_cast<std::int64_t>(i) << 16); firstGot = got; }
                ++bad;
            }
        }
        t.check(bad == 0, index++, firstExpected, firstGot);
    }
    std::fprintf(stderr, "putcadr: %d format-4 pixels on the original's bugged path not compared\n", skipped);
}

void testPixelModel() {
    for (int c = 0; c < 65536; ++c) {
        const auto v = static_cast<hc::Color16>(c);
        if (hc::fromRgb8(hc::toRgb8(v)) != v) { CHECK(false); break; }
    }
    CHECK_EQ(hc::toRgb8(0xFFFF).r, 255);
    CHECK_EQ(hc::toRgb8(0xFFFF).g, 255);
    CHECK_EQ(hc::shadow(0xFFFF), hc::pack(15, 31, 15));
    CHECK_EQ(hc::fade(0xFFFF, 4), hc::shadow(0xFFFF));
    CHECK_EQ(hc::fade(0xFFFF, 9), 0);
}

// The EndFrame state machine (0x406ea0): level sequences and their timing.
void testEffectSequences() {
    using gromada::game::ScreenEffects;
    const auto run = [](int effect, std::uint32_t last0, int frameMs, int frames) {
        ScreenEffects fx;
        std::vector<int> levels;
        int r = 0;
        // Prime the 71 ms timer: a previous effect stepped at last0.
        if (last0) { fx.start(5, [&] { return r; }); for (std::uint32_t c = last0 - 9 * 71 - 71; fx.effect(); c += 71) fx.endFrame(std::nullopt, c, false); }
        fx.start(effect, [&] { return r; });
        std::uint32_t clock = last0 + 100;   // the 71 ms timer last stepped long ago
        for (int i = 0; i < frames && fx.effect(); ++i, clock += static_cast<std::uint32_t>(frameMs)) {
            levels.push_back(fx.level());
            fx.endFrame(std::nullopt, clock, false);
        }
        levels.push_back(fx.effect());
        return levels;
    };
    // eff_nuke at 71 ms frames: 1, 3, 5, -32 (one frame), -30 .. -2, then off.
    {
        const auto l = run(1, 0, 71, 100);
        std::vector<int> want{1, 3, 5, -32};
        for (int v = -30; v <= -2; v += 2) want.push_back(v);
        want.push_back(0);
        CHECK(l == want);
    }
    // eff_bnuke: the negative branch starts at -30.
    {
        const auto l = run(4, 0, 71, 100);
        CHECK(l.size() > 4 && l[3] == -30 && l[4] == -28);
    }
    // eff_fade: 1..9, -9 (one frame), -8 .. -1, off.
    {
        const auto l = run(5, 0, 71, 100);
        std::vector<int> want;
        for (int v = 1; v <= 9; ++v) want.push_back(v);
        for (int v = -9; v <= -1; ++v) want.push_back(v);
        want.push_back(0);
        CHECK(l == want);
    }
    // At 20 ms frames a level lasts until 71 ms of clock passed since the last step.
    {
        const auto l = run(5, 0, 20, 400);
        CHECK(l.size() > 6 && l[0] == 1 && l[1] == 2 && l[2] == 2 && l[3] == 2 && l[4] == 2 && l[5] == 3);
    }
    // Effect(0) stops a running effect; eff_random draws rand() % 6 + 1.
    {
        ScreenEffects fx;
        fx.start(1, [] { return 0; });
        fx.start(0, [] { return 0; });
        CHECK_EQ(fx.effect(), 0);
        fx.start(255, [] { return 10; });   // 10 % 6 + 1 = 5
        CHECK_EQ(fx.effect(), 5);
    }
    // eff_openwindow: the previous picture stays outside a window that opens from the centre.
    {
        ScreenEffects fx;
        constexpr int W = 64, H = 48;
        std::vector<RGBA8> pic(W * H);
        FramebufferRef fb{pic.data(), H, W};
        const RGBA8 oldc{{255, 0, 0}, 255}, newc{{0, 0, 255}, 255};
        std::ranges::fill(pic, oldc);
        fx.endFrame(fb, 1000, false);              // the previous frame
        fx.start(6, [] { return 0; });
        fx.start(1, [] { return 0; });             // ignored while the window opens
        CHECK_EQ(fx.effect(), 6);
        std::uint32_t clock = 1017;
        std::ranges::fill(pic, newc);
        fx.endFrame(fb, clock, false);             // window = centre +-5
        CHECK(pic[static_cast<std::size_t>(24 * W + 32)].b == 255 && pic[0].r == 255);
        CHECK(pic[static_cast<std::size_t>(24 * W + 26)].r == 255 && pic[static_cast<std::size_t>(24 * W + 27)].b == 255);
        int frames = 0;
        while (fx.effect() && frames < 1000) {
            clock += 17;
            std::ranges::fill(pic, newc);
            fx.endFrame(fb, clock, false);
            ++frames;
        }
        CHECK_EQ(fx.effect(), 0);
        CHECK(pic[0].b == 255);
        // 64 px wide: min(4, 64*37/1600 = 1) px per 37 ms -> (32-5)/1 steps of 37 ms.
        CHECK(frames > 50 && frames < 70);
    }
}

} // namespace

int main() {
    testEffectSequences();
    testPixelModel();
    testTables();
    testEffects();
    testGamma();
    testPutcadr();
    return gromada_test::finish();
}
