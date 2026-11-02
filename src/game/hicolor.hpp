#pragma once
// gromada::game::hicolor: the 16-bit colour model of gromada.exe (issue #15).
//
// The original draws into a 16-bit DirectDraw surface. SVGA::SetMode (0x404a80, tables at
// 0x404ed0..0x40541e) derives from the surface's RGB masks every table the renderer uses:
// brighten tables (+0x2b red/green, +0x2f blue), darken tables (+0x27), the alpha blend table
// (+0x243) and the fade masks (+0x22f..+0x23f). This header reproduces them for RGB565 (the
// common format of the time; a 555 card gives slightly different greens) as pure functions,
// verified against the original code run in emulation (tools/oracle/gen_hicolor.py,
// tests/hicolor_test.cpp).
//
// Our framebuffer stays RGBA8: a 565 colour is stored expanded (bit replication), which is
// exactly invertible (`fromRgb8(toRgb8(c)) == c`), so every operation that reads the
// destination (shadow, light, blend, Effect()) converts the pixel back to 565, works on it
// with the original arithmetic and writes it back expanded. Pixels drawn with an 8-bit
// palette are truncated on the way in, which is what the original's palette conversion
// (0x4055d0) does.
//
// Header-only; depends on libgromada's decoder (DecodeFrame visitors).

#include "core/std.hpp"
#include "libgromada/graphics_decoder.hpp"
#include "libgromada/software_renderer.hpp"

namespace gromada::game::hicolor {

using Color16 = std::uint16_t;

// RGB565 masks, shifts and channel sizes (SVGA +0, +2, +4; +8, +0x10, +0xc; +0x14, +0x1c, +0x18).
inline constexpr Color16 RMask = 0xF800, GMask = 0x07E0, BMask = 0x001F;
inline constexpr int RShift = 11, GShift = 5, BShift = 0;
inline constexpr int RBits = 5, GBits = 6, BBits = 5;
// SVGA +6: every channel without its lowest bit; (c & HalfMask) >> 1 halves each channel.
inline constexpr Color16 HalfMask = 0xF7DE;
// SVGA +0x1a82: (all masks >> 1) & (HalfMask >> 1), the grey of the radar.
inline constexpr Color16 Grey = 0x7BEF;

[[nodiscard]] constexpr int red(Color16 c) noexcept { return c >> RShift; }
[[nodiscard]] constexpr int green(Color16 c) noexcept { return (c >> GShift) & 0x3f; }
[[nodiscard]] constexpr int blue(Color16 c) noexcept { return c & 0x1f; }
[[nodiscard]] constexpr Color16 pack(int r, int g, int b) noexcept {
    return static_cast<Color16>((r << RShift) | (g << GShift) | b);
}

// 0x405560 / 0x4055d0: 8-bit channel >> (8 - bits), truncation.
[[nodiscard]] constexpr Color16 fromRgb8(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept {
    return pack(r >> 3, g >> 2, b >> 3);
}
[[nodiscard]] constexpr Color16 fromRgb8(ColorRgb8 c) noexcept { return fromRgb8(c.r, c.g, c.b); }
// Display of a 565 colour on our 8-bit framebuffer (bit replication: 31 -> 255, 63 -> 255).
[[nodiscard]] constexpr ColorRgb8 toRgb8(Color16 c) noexcept {
    const int r = red(c), g = green(c), b = blue(c);
    return {static_cast<std::uint8_t>((r << 3) | (r >> 2)), static_cast<std::uint8_t>((g << 2) | (g >> 4)),
            static_cast<std::uint8_t>((b << 3) | (b >> 2))};
}
[[nodiscard]] constexpr RGBA8 toPixel(Color16 c) noexcept { return RGBA8{toRgb8(c), 255}; }
[[nodiscard]] constexpr Color16 fromPixel(const RGBA8& p) noexcept { return fromRgb8(p.r, p.g, p.b); }
// An 8-bit colour as the original shows it (16-bit palette entry, displayed).
[[nodiscard]] constexpr ColorRgb8 quantize(ColorRgb8 c) noexcept { return toRgb8(fromRgb8(c)); }

// ---------------------------------------------------------------------------- tables
// Brighten tables (+0x2b for the two top channels, +0x2f for blue): v * 8 / (8 - level),
// saturated at the channel maximum (0x404f... loop at 0x405218: hi*8/(8-rL), mid*8/(8-gL);
// 0x405354: blue i*8/(8-L), >= 32 -> 31). level 0..7.
[[nodiscard]] constexpr int brightenChannel(int v, int level, int bits) noexcept {
    const int max = (1 << bits) - 1;
    const int r = v * 8 / (8 - level);
    return r > max ? max : r;
}
// 0x405e20 with p = rLevel << 6 | gLevel << 3 | bLevel (also the light pixels of formats 3/4,
// 0x402110, and 0x405da0 with the same level on the three channels).
[[nodiscard]] constexpr Color16 brighten(Color16 c, int rLevel, int gLevel, int bLevel) noexcept {
    return pack(brightenChannel(red(c), rLevel, RBits), brightenChannel(green(c), gLevel, GBits),
                brightenChannel(blue(c), bLevel, BBits));
}
// Darken table (+0x27, 6 tables for k = 6..1): v * k / 7 (0x40512b loop, /7 by 0x92492493).
[[nodiscard]] constexpr int darkenChannel(int v, int k) noexcept { return v * k / 7; }
// Blend table (+0x243): table t = op - 1, [src][dst] = (src*op + dst*(7-op)) / 7 on 5-bit
// channels (0x40538e loop). Format 8 (0x4032e2) reads each channel's top 5 bits (shift +0x20
// .. +0x22 = shift + 1 for the 6-bit green) and writes the result back at that shift, so the
// lowest green bit is lost.
[[nodiscard]] constexpr int blendChannel(int src5, int dst5, int op) noexcept { return (src5 * op + dst5 * (7 - op)) / 7; }
[[nodiscard]] constexpr Color16 blend(Color16 src, Color16 dst, int op) noexcept {
    return pack(blendChannel(red(src), red(dst), op), blendChannel(green(src) >> 1, green(dst) >> 1, op) << 1,
                blendChannel(blue(src), blue(dst), op));
}

// Cast shadow and shadow pixels: (c & HalfMask) >> 1 (0x4027b8, 0x402af8).
[[nodiscard]] constexpr Color16 shadow(Color16 c) noexcept { return static_cast<Color16>((c & HalfMask) >> 1); }

// Fade masks (+0x23f = HalfMask, then each one without one more low bit per channel:
// (m*2) & m, 0x4053f5). fadeMask(n) = every channel without its n lowest bits, n = 1..5.
[[nodiscard]] constexpr Color16 fadeMask(int n) noexcept {
    Color16 m = HalfMask;
    for (int i = 1; i < n; ++i) m = static_cast<Color16>((m * 2) & m);
    return m;
}
// 0x405c40 fade(level): 1..3 c -= (c & mask) >> (5-level) (x15/16, x7/8, x3/4); 4..8
// c = (c & mask) >> (level-3) (x1/2 .. x1/32); 9 black; 0 nothing.
[[nodiscard]] constexpr Color16 fade(Color16 c, int level) noexcept {
    if (level <= 0) return c;
    if (level >= 9) return 0;
    if (level < 4) {
        const int s = 5 - level;
        return static_cast<Color16>(c - ((c & fadeMask(s)) >> s));
    }
    const int s = level - 3;
    return static_cast<Color16>((c & fadeMask(s)) >> s);
}

// 0x4072c0: gamma 0xRGB on a 16-bit palette. Nibble n: 0 clears the channel, 1..6 multiply by
// n/7 (darken table), 7 keeps it, 8..14 brighten with level n-7 (x8/(15-n)), 15 saturates.
// The red "nibble" is (gamma & 0xffff) >> 8, not masked: values above 15 leave red alone.
inline void applyGamma(std::span<Color16> palette, int gamma) {
    gamma &= 0xffff;
    const int r = gamma >> 8, g = (gamma >> 4) & 0xf, b = gamma & 0xf;
    const auto brightenLevel = [](int n) { return n > 7 && n < 15 ? n - 7 : 0; };
    const int rl = brightenLevel(r), gl = brightenLevel(g), bl = brightenLevel(b);
    for (auto& c : palette) {
        if (rl | gl | bl) c = brighten(c, rl, gl, bl);
        if (b == 15) c = static_cast<Color16>(c | BMask);
        else if (b < 1) c = static_cast<Color16>(c & ~BMask);
        else if (b < 7) c = static_cast<Color16>((c & ~BMask) | darkenChannel(blue(c), b));
        if (r == 15) c = static_cast<Color16>(c | RMask);
        else if (r < 1) c = static_cast<Color16>(c & ~RMask);
        else if (r < 7) c = static_cast<Color16>((c & ~RMask) | (darkenChannel(red(c), r) << RShift));
        if (g == 15) c = static_cast<Color16>(c | GMask);
        else if (g < 1) c = static_cast<Color16>(c & ~GMask);
        else if (g < 7) c = static_cast<Color16>((c & ~GMask) | (darkenChannel(green(c), g) << GShift));
    }
}

// The 16-bit palette of a graphics (0x406830): 8-bit entries truncated, then the global gamma
// unless the owner vid ignores it (vid flags & IgnoreGlobalGamma, 0x406903), for any format.
inline std::array<Color16, 256> palette16(const std::array<ColorRgb8, 256>& rgb, int gamma, bool ignoreGamma) {
    std::array<Color16, 256> out{};
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = fromRgb8(rgb[i]);
    if (gamma != 0x777 && !ignoreGamma) applyGamma(out, gamma);
    return out;
}
// The same, displayed (what the renderer copies for literal pixels).
inline std::array<ColorRgb8, 256> expand(const std::array<Color16, 256>& p) {
    std::array<ColorRgb8, 256> out{};
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = toRgb8(p[i]);
    return out;
}

// ---------------------------------------------------------------------------- frames
// putcadr (0x4021f0) per format, on our RGBA8 framebuffer:
//  format 2: literal/repeat pixels from the palette; shadow pixels (cmd 0x40) halve the
//            destination, unless the sprite has just drawn its cast shadow ([ebp-0x20]);
//  format 3: light, the three channels brightened by the factor (0x405da0);
//  format 4: light, per channel factors (0x402110);
//  format 8: opacity 7 opaque, 1..6 blend (src*op + dst*(7-op))/7, 0 transparent (0x4031fb);
//  cast shadow pass (vid flag 0x80 and z > 0, 0x42df8c = 1): format 2 opaque pixels halve
//  the destination and its shadow pixels are skipped; formats 3 and 8 fade the destination
//  by (factor >> 1) + 1 (0x405c40, 1..4 = x15/16 .. x1/2); format 4 does not check the flag
//  and draws its light again.
struct FrameVisitor {
    enum class Pass : std::uint8_t { Normal, CastShadow };
    int x0, y0;
    const std::array<ColorRgb8, 256>* palette;   // displayed 565 colours (or any 8-bit palette)
    FramebufferRef fb;
    int format = 2;
    Pass pass = Pass::Normal;
    bool skipShadowPixels = false;               // the cast shadow has been drawn
    int x = 0, y = 0;

    [[nodiscard]] ClippingInfo begin_image(BoundingBox source_rect) noexcept {
        const BoundingBox dst{-x0, fb.extent(1) - x0, -y0, fb.extent(0) - y0};
        return {source_rect, dst};
    }
    void set_cursor(int cx, int cy) noexcept { x = x0 + cx; y = y0 + cy; }
    void advance_cursor(int n) noexcept { x += n; }

    void draw_pixels_shadow(int n) noexcept {
        if (pass == Pass::CastShadow || skipShadowPixels) { x += n; return; }
        each(n, [](RGBA8& p, int) { p = toPixel(shadow(fromPixel(p))); });
    }
    void draw_pixels_indexed(std::span<const IndexedColor> c) noexcept {
        const int n = static_cast<int>(c.size());
        if (pass == Pass::CastShadow) { opaqueShadow(n); return; }
        each(n, [&](RGBA8& p, int i) { p = RGBA8{(*palette)[std::to_integer<std::uint8_t>(c[static_cast<std::size_t>(i)])], 255}; });
    }
    void draw_pixels_repeat(int n, IndexedColor c) noexcept {
        if (pass == Pass::CastShadow) { opaqueShadow(n); return; }
        const RGBA8 v{(*palette)[std::to_integer<std::uint8_t>(c)], 255};
        each(n, [&](RGBA8& p, int) { p = v; });
    }
    // Format 6 (direct 15-bit colours: only screen images use it).
    void draw_pixels(std::span<const CompressedColor> c) noexcept {
        const int n = static_cast<int>(c.size());
        if (pass == Pass::CastShadow) { opaqueShadow(n); return; }
        each(n, [&](RGBA8& p, int i) { p = RGBA8{quantize(c[static_cast<std::size_t>(i)].to_rgb8()), 255}; });
    }
    void draw_pixels_repeat(int n, CompressedColor c) noexcept {
        if (pass == Pass::CastShadow) { opaqueShadow(n); return; }
        const RGBA8 v{quantize(c.to_rgb8()), 255};
        each(n, [&](RGBA8& p, int) { p = v; });
    }
    void draw_pixels_light(int n, std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept {
        if (pass == Pass::CastShadow && format == 3) {
            const int level = (r >> 1) + 1;
            each(n, [&](RGBA8& p, int) { p = toPixel(fade(fromPixel(p), level)); });
            return;
        }
        each(n, [&](RGBA8& p, int) { p = toPixel(brighten(fromPixel(p), r & 7, g & 7, b & 7)); });
    }
    // Format 8: the decoder passes t = 255 - opacity * 42 (opacity 1..6).
    void draw_pixels_alpha_blend(std::uint8_t t, std::span<const std::byte> c) noexcept {
        const int op = (255 - t) / 42;
        const int n = static_cast<int>(c.size());
        if (pass == Pass::CastShadow) {
            const int level = (op >> 1) + 1;
            each(n, [&](RGBA8& p, int) { p = toPixel(fade(fromPixel(p), level)); });
            return;
        }
        each(n, [&](RGBA8& p, int i) {
            const Color16 src = fromRgb8((*palette)[std::to_integer<std::uint8_t>(c[static_cast<std::size_t>(i)])]);
            p = toPixel(blend(src, fromPixel(p), op));
        });
    }

private:
    void opaqueShadow(int n) noexcept {
        // format 2: halve; format 8 opaque run (opacity 7): fade level 4 = the same halving.
        each(n, [](RGBA8& p, int) { p = toPixel(shadow(fromPixel(p))); });
    }
    template <class F>
    void each(int n, F&& f) noexcept {
        if (y >= 0 && y < fb.extent(0)) {
            const int e = std::min(x + n, fb.extent(1));
            for (int i = std::max(x, 0); i < e; ++i) f(fb[y, i], i - x);
        }
        x += n;
    }
};

// Draws one frame like putcadr at top-left (x, y). `castShadow`: the sprite draws its cast
// shadow first at (x - z/2 - z/4, y + z/4 - z/2) (vid flag 0x80, z > 0); the body goes at
// y - z unless the vid has ZeroZ (the caller passes the final body position).
inline void drawFrame(const VidGraphics::Frame& frame, int x, int y, const std::array<ColorRgb8, 256>& palette,
                      FramebufferRef fb, FrameVisitor::Pass pass = FrameVisitor::Pass::Normal, bool skipShadowPixels = false) {
    const int w = frame.width(), h = frame.height();
    if (frame.data.empty() || x + w <= 0 || y + h <= 0 || x >= fb.extent(1) || y >= fb.extent(0)) return;
    const int format = frame.parent ? frame.parent->dataFormat : 2;
    if (format == 7) return;   // lens: handled by the UI
    DecodeFrame(frame, FrameVisitor{x, y, &palette, fb, format, pass, skipShadowPixels});
}

// A frame with its graphics' own palette displayed in 16 bits (screen images of the UI).
inline void drawFrame(const VidGraphics::Frame& frame, int x, int y, FramebufferRef fb) {
    if (!frame.parent) return;
    const auto pal = expand(palette16(frame.parent->palette, 0x777, true));
    drawFrame(frame, x, y, pal, fb);
}

} // namespace gromada::game::hicolor
