#pragma once
// Rasterizer of the game view (issue #20, docs/rendering.md): putcadr (hicolor::FrameVisitor)
// through a view transform, for any scale and any sub-pixel position.
//
// Every source pixel covers the framebuffer pixels between the projections of its two edges
// (nearest-neighbour scaling). Edges are computed in fixed point from world coordinates, so
// sprites that touch in the world touch on the screen at every scale (no seams between
// terrain tiles), and a frame at an integer position with scale 1 covers exactly the pixels
// hicolor::drawFrame writes. The pixel operations (palette copy, shadow, light, format-8
// blend, cast shadow) are those of hicolor::FrameVisitor, applied to every covered pixel.
//
// Header-only; depends on hicolor.hpp.

#include "core/std.hpp"
#include "game/hicolor.hpp"

#include <cstring>

namespace gromada::game::view {

// World coordinates in fixed point: 1/256 world pixel. Scale: framebuffer pixels per world
// pixel in 16.16.
inline constexpr int SubBits = 8;
inline constexpr int ScaleBits = 16;

[[nodiscard]] inline std::int64_t toSub(double world) noexcept {
    return static_cast<std::int64_t>(std::floor(world * (1 << SubBits) + 0.5));
}
[[nodiscard]] inline std::int64_t toScale(double scale) noexcept {
    return static_cast<std::int64_t>(std::llround(scale * (1 << ScaleBits)));
}

// A framebuffer seen through a view transform: fb pixel = (world - origin) * scale.
struct Raster {
    FramebufferRef fb;
    std::int64_t originX = 0, originY = 0;   // world point at the framebuffer's (0, 0), 1/256 px
    std::int64_t scale = std::int64_t{1} << ScaleBits;
    // rows drawn: [clipTop, clipBottom) (bands drawn by different threads); -1 = the whole height
    int clipTop = 0, clipBottom = -1;
    // First framebuffer column/row at or after world coordinate `w` (1/256 px): floor().
    [[nodiscard]] int column(std::int64_t w) const noexcept {
        return static_cast<int>(((w - originX) * scale) >> (SubBits + ScaleBits));
    }
    [[nodiscard]] int row(std::int64_t w) const noexcept {
        return static_cast<int>(((w - originY) * scale) >> (SubBits + ScaleBits));
    }
    [[nodiscard]] int width() const noexcept { return fb.extent(1); }
    [[nodiscard]] int height() const noexcept { return fb.extent(0); }
    [[nodiscard]] int top() const noexcept { return std::max(clipTop, 0); }
    [[nodiscard]] int bottom() const noexcept { return clipBottom < 0 ? fb.extent(0) : std::min(clipBottom, fb.extent(0)); }
};

// Scratch buffers of drawFrameScaled (edge tables), reused between calls of one thread.
struct ScaleScratch {
    std::vector<int> cols, rows;
};

// putcadr through a scale: hicolor::FrameVisitor's operations on every framebuffer pixel a
// source pixel covers. `cols[c]` / `rows[r]` are the framebuffer column/row of the left/top
// edge of source column c / row r (c = w, r = h: the right/bottom edge). `k` > 0: the scale
// is the integer k, so every source column covers exactly k framebuffer columns (also at
// sub-pixel positions) and the edges need not be looked up.
struct ScaledFrameVisitor {
    using Pass = hicolor::FrameVisitor::Pass;
    const int* cols;
    const int* rows;
    int w, h;
    const std::array<ColorRgb8, 256>* palette;
    FramebufferRef fb;
    int format = 2;
    Pass pass = Pass::Normal;
    bool skipShadowPixels = false;
    int k = 0;
    int top = 0, bottom = 0;   // framebuffer rows drawn
    int sx = 0, sy = 0;

    [[nodiscard]] ClippingInfo begin_image(BoundingBox source_rect) noexcept {
        const int W = fb.extent(1);
        int c0 = 0, c1 = w, r0 = 0, r1 = h;
        while (c0 < w && cols[c0 + 1] <= 0) ++c0;
        while (c1 > c0 && cols[c1 - 1] >= W) --c1;
        while (r0 < h && rows[r0 + 1] <= top) ++r0;
        while (r1 > r0 && rows[r1 - 1] >= bottom) --r1;
        return {source_rect, BoundingBox{c0, c1, r0, r1}};
    }
    void set_cursor(int cx, int cy) noexcept { sx = cx; sy = cy; }
    void advance_cursor(int n) noexcept { sx += n; }

    void draw_pixels_shadow(int n) noexcept {
        if (pass == Pass::CastShadow || skipShadowPixels) { sx += n; return; }
        each(n, [](RGBA8& p) { p = hicolor::toPixel(hicolor::shadow(hicolor::fromPixel(p))); });
    }
    void draw_pixels_indexed(std::span<const IndexedColor> c) noexcept {
        const int n = static_cast<int>(c.size());
        if (pass == Pass::CastShadow) { opaqueShadow(n); return; }
        const auto& pal = *palette;
        opaque(n, [&](int i) { return RGBA8{pal[std::to_integer<std::uint8_t>(c[static_cast<std::size_t>(i)])], 255}; });
    }
    void draw_pixels_repeat(int n, IndexedColor c) noexcept {
        if (pass == Pass::CastShadow) { opaqueShadow(n); return; }
        const RGBA8 v{(*palette)[std::to_integer<std::uint8_t>(c)], 255};
        opaque(n, [&](int) { return v; });
    }
    void draw_pixels(std::span<const CompressedColor> c) noexcept {
        const int n = static_cast<int>(c.size());
        if (pass == Pass::CastShadow) { opaqueShadow(n); return; }
        opaque(n, [&](int i) { return RGBA8{hicolor::quantize(c[static_cast<std::size_t>(i)].to_rgb8()), 255}; });
    }
    void draw_pixels_repeat(int n, CompressedColor c) noexcept {
        if (pass == Pass::CastShadow) { opaqueShadow(n); return; }
        const RGBA8 v{hicolor::quantize(c.to_rgb8()), 255};
        opaque(n, [&](int) { return v; });
    }
    void draw_pixels_light(int n, std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept {
        if (pass == Pass::CastShadow && format == 3) {
            const int level = (r >> 1) + 1;
            each(n, [&](RGBA8& p) { p = hicolor::toPixel(hicolor::fade(hicolor::fromPixel(p), level)); });
            return;
        }
        const int rl = r & 7, gl = g & 7, bl = b & 7;
        each(n, [&](RGBA8& p) { p = hicolor::toPixel(hicolor::brighten(hicolor::fromPixel(p), rl, gl, bl)); });
    }
    void draw_pixels_alpha_blend(std::uint8_t t, std::span<const std::byte> c) noexcept {
        const int op = (255 - t) / 42;
        const int n = static_cast<int>(c.size());
        if (pass == Pass::CastShadow) {
            const int level = (op >> 1) + 1;
            each(n, [&](RGBA8& p) { p = hicolor::toPixel(hicolor::fade(hicolor::fromPixel(p), level)); });
            return;
        }
        const auto& pal = *palette;
        blendSource(
            n, [&](int i) { return hicolor::fromRgb8(pal[std::to_integer<std::uint8_t>(c[static_cast<std::size_t>(i)])]); },
            [op](RGBA8& p, hicolor::Color16 src) { p = hicolor::toPixel(hicolor::blend(src, hicolor::fromPixel(p), op)); });
    }

private:
    void opaqueShadow(int n) noexcept {
        each(n, [](RGBA8& p) { p = hicolor::toPixel(hicolor::shadow(hicolor::fromPixel(p))); });
    }

    // The run's source pixels [i0, i1) that exist, and the framebuffer rows of source row sy.
    struct Span { int i0, i1, y0, y1; };
    [[nodiscard]] bool span(int n, Span& sp) const noexcept {
        if (sy < 0 || sy >= h || sx >= w) return false;
        sp.y0 = std::max(rows[sy], top);
        sp.y1 = std::min(rows[sy + 1], bottom);
        sp.i0 = std::max(0, -sx);
        sp.i1 = std::min(n, w - sx);
        return sp.y0 < sp.y1 && sp.i0 < sp.i1;
    }
    // Pixels whose value does not depend on the destination: computed once per source pixel on
    // the first covered row, the other rows are copies.
    template <class V>
    void opaque(int n, V&& value) noexcept {
        Span sp;
        if (!span(n, sp)) { sx += n; return; }
        const int W = fb.extent(1);
        RGBA8* base = fb.data_handle();
        RGBA8* row = base + static_cast<std::size_t>(sp.y0) * static_cast<std::size_t>(W);
        const int x = cols[sx];
        if (k == 1) {
            for (int i = std::max(sp.i0, -x), e = std::min(sp.i1, W - x); i < e; ++i) row[x + i] = value(i);
        } else if (k > 1) {
            const int iLo = std::max(sp.i0, x >= 0 ? 0 : (-x) / k), iHi = std::min(sp.i1, (W - x + k - 1) / k);
            // [iA, iB): source pixels whose k framebuffer pixels are all inside the row, written
            // without clamping; the (at most one per side) partial ones as before. Every source
            // pixel writes its own pixels, so the order does not matter: same picture.
            const int iA = std::min(iHi, std::max(iLo, x >= 0 ? 0 : (-x + k - 1) / k));
            const int iB = std::max(iA, std::min(iHi, (W - x) / k));
            const auto clamped = [&](int i) {
                const RGBA8 v = value(i);
                for (int X = std::max(x + i * k, 0), E = std::min(x + i * k + k, W); X < E; ++X) row[X] = v;
            };
            for (int i = iLo; i < iA; ++i) clamped(i);
            RGBA8* out = row + x + iA * k;
            if (k == 2) {
                for (int i = iA; i < iB; ++i, out += 2) { const RGBA8 v = value(i); out[0] = v; out[1] = v; }
            } else {
                for (int i = iA; i < iB; ++i, out += k) { const RGBA8 v = value(i); for (int j = 0; j < k; ++j) out[j] = v; }
            }
            for (int i = iB; i < iHi; ++i) clamped(i);
        } else {
            for (int i = sp.i0; i < sp.i1; ++i) {
                const int x0 = std::max(cols[sx + i], 0), x1 = std::min(cols[sx + i + 1], W);
                if (x0 < x1) std::fill(row + x0, row + x1, value(i));
            }
        }
        const int x0 = std::max(cols[sx + sp.i0], 0), x1 = std::min(cols[sx + sp.i1], W);
        if (x1 > x0)
            for (int y = sp.y0 + 1; y < sp.y1; ++y)
                std::memcpy(base + static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + x0, row + x0,
                            static_cast<std::size_t>(x1 - x0) * sizeof(RGBA8));
        sx += n;
    }
    // Operations that only read the destination: every covered pixel of each row, one
    // contiguous range (the source pixels' columns are adjacent), each pixel once.
    template <class F>
    void each(int n, F&& op) noexcept {
        Span sp;
        if (!span(n, sp)) { sx += n; return; }
        const int W = fb.extent(1);
        const int x0 = std::max(cols[sx + sp.i0], 0), x1 = std::min(cols[sx + sp.i1], W);
        if (x0 < x1)
            for (int y = sp.y0; y < sp.y1; ++y) {
                RGBA8* row = fb.data_handle() + static_cast<std::size_t>(y) * static_cast<std::size_t>(W);
                for (int X = x0; X < x1; ++X) op(row[X]);
            }
        sx += n;
    }
    // Operations that read the destination and a value of the source pixel: source(i) once per
    // row and source pixel, op(pixel, value) on each pixel it covers (k > 1: the k pixels of the
    // interior ones without clamping, as in opaque()).
    template <class S, class B>
    void blendSource(int n, S&& source, B&& op) noexcept {
        Span sp;
        if (!span(n, sp)) { sx += n; return; }
        const int W = fb.extent(1);
        const int x = cols[sx];
        for (int y = sp.y0; y < sp.y1; ++y) {
            RGBA8* row = fb.data_handle() + static_cast<std::size_t>(y) * static_cast<std::size_t>(W);
            if (k == 1) {
                for (int i = std::max(sp.i0, -x), e = std::min(sp.i1, W - x); i < e; ++i) op(row[x + i], source(i));
            } else if (k > 1) {
                const int iLo = std::max(sp.i0, x >= 0 ? 0 : (-x) / k), iHi = std::min(sp.i1, (W - x + k - 1) / k);
                const int iA = std::min(iHi, std::max(iLo, x >= 0 ? 0 : (-x + k - 1) / k));
                const int iB = std::max(iA, std::min(iHi, (W - x) / k));
                const auto clamped = [&](int i) {
                    const auto v = source(i);
                    for (int X = std::max(x + i * k, 0), E = std::min(x + i * k + k, W); X < E; ++X) op(row[X], v);
                };
                for (int i = iLo; i < iA; ++i) clamped(i);
                RGBA8* out = row + x + iA * k;
                if (k == 2) {
                    for (int i = iA; i < iB; ++i, out += 2) { const auto v = source(i); op(out[0], v); op(out[1], v); }
                } else {
                    for (int i = iA; i < iB; ++i, out += k) { const auto v = source(i); for (int j = 0; j < k; ++j) op(out[j], v); }
                }
                for (int i = iB; i < iHi; ++i) clamped(i);
            } else {
                for (int i = sp.i0; i < sp.i1; ++i) {
                    const int x0 = std::max(cols[sx + i], 0), x1 = std::min(cols[sx + i + 1], W);
                    if (x0 >= x1) continue;
                    const auto v = source(i);
                    for (int X = x0; X < x1; ++X) op(row[X], v);
                }
            }
        }
        sx += n;
    }
};

// Bounds of a frame drawn with its top-left corner at world (left, top) (1/256 px).
struct FrameBounds {
    int x0, y0, x1, y1;   // framebuffer pixels [x0, x1) x [y0, y1)
};
[[nodiscard]] inline FrameBounds frameBounds(const Raster& r, std::int64_t left, std::int64_t top, int w, int h) noexcept {
    return {r.column(left), r.row(top), r.column(left + (std::int64_t{w} << SubBits)), r.row(top + (std::int64_t{h} << SubBits))};
}

// A pass over the rows of a band instead of a frame (issue #21, docs/enhancements.md: the
// contact shadows between layers 0..4 and 5, the lights after the last layer): called as
// fn(ctx, framebuffer, band, first row, end row) at its place in the list. It must only touch
// those rows and give each pixel the same value whatever the band split, so that the picture
// stays the same for any number of bands.
using BandPass = void (*)(void* ctx, FramebufferRef fb, int band, int y0, int y1);

// One frame to draw (a draw list is built once, then rasterized by bands, see drawBand()), or a
// band pass (bandPass set, frame unused).
struct DrawCommand {
    const VidGraphics::Frame* frame;
    const std::array<ColorRgb8, 256>* palette;
    std::int64_t left, top;          // 1/256 px in the raster's world (or screen) space
    bool screen;                     // through the screen raster
    hicolor::FrameVisitor::Pass pass;
    bool skipShadowPixels;
    BandPass bandPass = nullptr;
    void* bandContext = nullptr;
};

// hicolor::drawFrame through a view transform: frame top-left at world (left, top), 1/256 px.
inline void drawFrameScaled(const VidGraphics::Frame& frame, std::int64_t left, std::int64_t top,
                            const std::array<ColorRgb8, 256>& palette, const Raster& r, ScaleScratch& scratch,
                            hicolor::FrameVisitor::Pass pass = hicolor::FrameVisitor::Pass::Normal,
                            bool skipShadowPixels = false) {
    const int w = frame.width(), h = frame.height();
    if (frame.data.empty() || w <= 0 || h <= 0) return;
    const int format = frame.parent ? frame.parent->dataFormat : 2;
    if (format == 7) return;
    const FrameBounds b = frameBounds(r, left, top, w, h);
    if (b.x1 <= 0 || b.y1 <= r.top() || b.x0 >= r.width() || b.y0 >= r.bottom() || b.x1 <= b.x0 || b.y1 <= b.y0) return;
    scratch.cols.resize(static_cast<std::size_t>(w) + 1);
    scratch.rows.resize(static_cast<std::size_t>(h) + 1);
    for (int c = 0; c <= w; ++c) scratch.cols[static_cast<std::size_t>(c)] = r.column(left + (std::int64_t{c} << SubBits));
    for (int y = 0; y <= h; ++y) scratch.rows[static_cast<std::size_t>(y)] = r.row(top + (std::int64_t{y} << SubBits));
    const int k = (r.scale & ((std::int64_t{1} << ScaleBits) - 1)) == 0 ? static_cast<int>(r.scale >> ScaleBits) : 0;
    DecodeFrame(frame, ScaledFrameVisitor{scratch.cols.data(), scratch.rows.data(), w, h, &palette, r.fb, format, pass,
                                          skipShadowPixels, k, r.top(), r.bottom()});
}

// Rasterizes `list` in painter order, in `bands` horizontal bands of the framebuffer; band b
// covers rows [b*H/bands, (b+1)*H/bands). Every band is independent (a pixel belongs to one
// band and sees the commands in order), so the bands can run on different threads and the
// picture is the same for any number of bands.
inline void drawBand(std::span<const DrawCommand> list, Raster world, Raster screen, int band, int bands, ScaleScratch& scratch) {
    const int H = world.fb.extent(0);
    const int y0 = static_cast<int>(static_cast<std::int64_t>(H) * band / bands);
    const int y1 = static_cast<int>(static_cast<std::int64_t>(H) * (band + 1) / bands);
    world.clipTop = screen.clipTop = y0;
    world.clipBottom = screen.clipBottom = y1;
    for (const DrawCommand& c : list) {
        if (c.bandPass) {
            c.bandPass(c.bandContext, world.fb, band, y0, y1);
            continue;
        }
        drawFrameScaled(*c.frame, c.left, c.top, *c.palette, c.screen ? screen : world, scratch, c.pass, c.skipShadowPixels);
    }
}

} // namespace gromada::game::view
