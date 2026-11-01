// .CUR parser and cursor drawing, on cursor files generated here (no game data), plus a
// check of the original cursores/*.CUR when GROMADA_DATA is set.
#include "input/cursor_file.hpp"
#include "test.hpp"

#include <cstdio>
#include <functional>
#include <unistd.h>

using namespace gromada::input;

namespace {

void put16(std::vector<std::uint8_t>& b, std::uint32_t v) { b.push_back(v & 255); b.push_back((v >> 8) & 255); }
void put32(std::vector<std::uint8_t>& b, std::uint32_t v) { put16(b, v & 0xFFFF); put16(b, v >> 16); }

enum class Px { Transparent, Black, White, Invert };

// Builds a single-image .CUR: 1 bpp black/white palette (like Gromada's) or 32 bpp.
std::vector<std::uint8_t> makeCursor(int w, int h, int hotX, int hotY, int bpp,
                                     const std::function<Px(int, int)>& px,
                                     const std::function<std::uint32_t(int, int)>& argb = {}) {
    const int xorStride = ((w * bpp + 31) / 32) * 4, andStride = ((w + 31) / 32) * 4;
    const int palette = bpp == 1 ? 2 : 0;
    const std::uint32_t imageSize = 40 + palette * 4 + (xorStride + andStride) * h;
    std::vector<std::uint8_t> b;
    put16(b, 0); put16(b, 2); put16(b, 1);                       // ICONDIR
    b.push_back(w); b.push_back(h); b.push_back(0); b.push_back(0); // ICONDIRENTRY
    put16(b, hotX); put16(b, hotY); put32(b, imageSize); put32(b, 22);
    put32(b, 40); put32(b, w); put32(b, h * 2); put16(b, 1); put16(b, bpp); // BITMAPINFOHEADER
    put32(b, 0); put32(b, 0); put32(b, 0); put32(b, 0); put32(b, 0); put32(b, 0);
    if (bpp == 1) { put32(b, 0x000000); put32(b, 0xFFFFFF); }
    std::vector<std::uint8_t> xorBits(xorStride * h), andBits(andStride * h);
    for (int y = 0; y < h; ++y) {
        const int row = h - 1 - y; // bottom-up
        for (int x = 0; x < w; ++x) {
            const Px p = px(x, y);
            const bool andBit = p == Px::Transparent || p == Px::Invert;
            const bool xorBit = p == Px::White || p == Px::Invert;
            if (andBit) andBits[row * andStride + x / 8] |= 0x80 >> (x % 8);
            if (bpp == 1 && xorBit) xorBits[row * xorStride + x / 8] |= 0x80 >> (x % 8);
            if (bpp == 32) {
                const std::uint32_t c = argb(x, y);
                for (int k = 0; k < 4; ++k) xorBits[row * xorStride + x * 4 + k] = (c >> (8 * k)) & 255;
            }
        }
    }
    b.insert(b.end(), xorBits.begin(), xorBits.end());
    b.insert(b.end(), andBits.begin(), andBits.end());
    return b;
}

Px pattern(int x, int y) {
    if (x == y) return Px::White;
    if (x == y + 1) return Px::Black;
    if (x == 31 - y) return Px::Invert;
    return Px::Transparent;
}

void testMonochrome() {
    const auto bytes = makeCursor(32, 32, 5, 9, 1, pattern);
    CHECK_EQ(bytes.size(), std::size_t{326}); // same layout as the game's cursors
    const auto img = parseCursor(bytes);
    CHECK(img.has_value());
    if (!img) return;
    CHECK(img->width == 32 && img->height == 32 && img->hotX == 5 && img->hotY == 9);
    int mismatches = 0;
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            const CursorTexel& t = img->at(x, y);
            switch (pattern(x, y)) {
            case Px::White: mismatches += !(t.mode == CursorTexel::Blend && t.alpha == 255 && t.rgb == 0xFFFFFF); break;
            case Px::Black: mismatches += !(t.mode == CursorTexel::Blend && t.alpha == 255 && t.rgb == 0); break;
            case Px::Invert: mismatches += !(t.mode == CursorTexel::Xor && t.rgb == 0xFFFFFF); break;
            case Px::Transparent: mismatches += !(t.mode == CursorTexel::Blend && t.alpha == 0); break;
            }
        }
    CHECK_EQ(mismatches, 0);
}

void testTrueColor() {
    auto argb = [](int x, int y) -> std::uint32_t {
        if (x == 0 && y == 0) return 0xFF102030; // opaque
        if (x == 1 && y == 0) return 0x80FF0000; // half transparent red
        return 0;
    };
    const auto img = parseCursor(makeCursor(16, 16, 15, 15, 32, [](int, int) { return Px::Transparent; }, argb));
    CHECK(img.has_value());
    if (!img) return;
    CHECK(img->width == 16 && img->hotX == 15 && img->hotY == 15);
    CHECK(img->at(0, 0).rgb == 0x102030 && img->at(0, 0).alpha == 255);
    CHECK(img->at(1, 0).rgb == 0xFF0000 && img->at(1, 0).alpha == 0x80);
    CHECK(img->at(5, 5).alpha == 0);

    std::uint32_t fb[2] = {0x0000FF, 0x0000FF};
    drawCursor(*img, 15, 15, fb, 2, 1, sizeof fb);
    CHECK_EQ(fb[0], std::uint32_t{0x102030});
    CHECK_EQ(fb[1], std::uint32_t{0x80007F}); // 50% red over blue
}

void testMalformed() {
    const auto good = makeCursor(32, 32, 1, 1, 1, pattern);
    // Every truncation is rejected without reading out of bounds.
    int accepted = 0;
    for (std::size_t n = 0; n < good.size(); ++n)
        accepted += parseCursor(std::span(good.data(), n)).has_value();
    CHECK_EQ(accepted, 0);
    auto bad = good;
    bad[2] = 3; // not an icon/cursor
    CHECK(!parseCursor(bad));
    bad = good;
    bad[18] = 0xFF; // offset beyond the file
    CHECK(!parseCursor(bad));
    bad = good;
    bad[22 + 14] = 2; // 2 bpp: unsupported
    CHECK(!parseCursor(bad));
    bad = good;
    const std::uint8_t png[] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    std::copy(std::begin(png), std::end(png), bad.begin() + 22);
    CHECK(!parseCursor(bad));
    // Hotspot outside the image is clamped.
    const auto far = parseCursor(makeCursor(32, 32, 90, 90, 1, pattern));
    CHECK(far && far->hotX == 31 && far->hotY == 31);
}

void testDraw() {
    const auto img = *parseCursor(makeCursor(32, 32, 5, 9, 1, pattern));
    constexpr int W = 40, H = 30;
    std::vector<std::uint32_t> fb(W * H, 0x404040);
    drawCursor(img, 10, 12, fb.data(), W, H, W * 4); // top-left at (5, 3)
    auto at = [&](int x, int y) { return fb[y * W + x]; };
    CHECK_EQ(at(5, 3), std::uint32_t{0xFFFFFF});     // (0,0) white
    CHECK_EQ(at(6, 3), std::uint32_t{0x000000});     // (1,0) black
    CHECK_EQ(at(5 + 31, 3), std::uint32_t{0xBFBFBF}); // (31,0) inverted
    CHECK_EQ(at(7, 3), std::uint32_t{0x404040});     // transparent
    CHECK_EQ(at(4, 3), std::uint32_t{0x404040});     // outside the cursor
    CHECK_EQ(at(5 + 26, 3 + 26), std::uint32_t{0xFFFFFF}); // (26,26) still on screen

    // Clipping on every side: nothing outside the buffer is touched.
    std::vector<std::uint32_t> guarded((W + 2) * (H + 2), 0x123456);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) guarded[(y + 1) * (W + 2) + x + 1] = 0;
    std::uint32_t* inner = guarded.data() + (W + 2) + 1;
    for (const auto& [x, y] : {std::pair{-20, -20}, {-3, 5}, {W - 2, H - 2}, {W + 50, 5}, {20, H + 40}, {0, 0}})
        drawCursor(img, x, y, inner, W, H, (W + 2) * 4);
    int touchedGuard = 0;
    for (int y = 0; y < H + 2; ++y)
        for (int x = 0; x < W + 2; ++x)
            if (x == 0 || y == 0 || x == W + 1 || y == H + 1) touchedGuard += guarded[y * (W + 2) + x] != 0x123456;
    CHECK_EQ(touchedGuard, 0);
    // The hotspot pixel lands on the target: pattern(5,9) is transparent, so test a cursor
    // whose hotspot is opaque.
    const auto dot = *parseCursor(makeCursor(8, 8, 3, 4, 1, [](int x, int y) { return x == 3 && y == 4 ? Px::White : Px::Transparent; }));
    std::vector<std::uint32_t> small(16 * 16, 0);
    drawCursor(dot, 7, 9, small.data(), 16, 16, 16 * 4);
    CHECK_EQ(small[9 * 16 + 7], std::uint32_t{0xFFFFFF});
    CHECK_EQ(std::count(small.begin(), small.end(), 0xFFFFFFu), 1);
}

void testCursorSet() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("gromada_cursor_test_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir / "CURSORES");
    {
        const auto bytes = makeCursor(32, 32, 7, 8, 1, pattern);
        std::ofstream(dir / "CURSORES" / "FIGHT.CUR", std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        std::ofstream(dir / "CURSORES" / "move.cur", std::ios::binary) << "garbage";
    }
    CursorSet set;
    CHECK_EQ(set.load(dir), 1); // case-insensitive lookup; the broken file falls back
    CHECK(set.get(CursorShape::Fight).hotX == 7 && set.get(CursorShape::Fight).width == 32);
    const auto& fallback = set.get(CursorShape::Move);
    CHECK(!fallback.empty() && fallback.hotX == 0 && fallback.hotY == 0 && fallback.at(0, 0).alpha == 255);
    CHECK_EQ(set.load(dir / "missing"), 0);
    CHECK(set.get(CursorShape::Fight).width == fallback.width);
    fs::remove_all(dir);
}

void testOriginalCursors() {
    const auto data = gromada_test::dataDir();
    if (!data) {
        std::fprintf(stderr, "original cursors: skipped (GROMADA_DATA not set)\n");
        return;
    }
    CursorSet set;
    CHECK_EQ(set.load(*data), 5);
    struct { CursorShape shape; int hx, hy; } expected[] = {
        {CursorShape::Arrow, 11, 7}, {CursorShape::Move, 16, 16}, {CursorShape::NoMove, 15, 16},
        {CursorShape::Fight, 16, 16}, {CursorShape::Cycle, 15, 17}};
    for (const auto& e : expected) {
        const auto& img = set.get(e.shape);
        CHECK(img.width == 32 && img.height == 32 && img.hotX == e.hx && img.hotY == e.hy);
        int opaque = 0;
        for (const auto& t : img.texels) opaque += t.alpha == 255;
        CHECK(opaque > 20 && opaque < 32 * 32 / 2);
    }
    // The arrow's hotspot is at its tip: opaque pixels only below/right of it.
    const auto& arrow = set.get(CursorShape::Arrow);
    CHECK(arrow.at(arrow.hotX, arrow.hotY).alpha == 255);
}

} // namespace

int main() {
    testMonochrome();
    testTrueColor();
    testMalformed();
    testDraw();
    testCursorSet();
    testOriginalCursors();
    return gromada_test::finish();
}
