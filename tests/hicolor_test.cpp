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
    int index = 0;
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
        int bad = 0;
        std::int64_t firstExpected = 0, firstGot = 0;
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            const auto got = hc::fromPixel(pixels[i]);
            if (got != v[5 + i]) {
                if (!bad) { firstExpected = v[5 + i] | (static_cast<std::int64_t>(i) << 16); firstGot = got; }
                ++bad;
            }
        }
        t.check(bad == 0, index++, firstExpected, firstGot);
    }
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

} // namespace

int main() {
    testPixelModel();
    testTables();
    testEffects();
    testGamma();
    testPutcadr();
    return gromada_test::finish();
}
