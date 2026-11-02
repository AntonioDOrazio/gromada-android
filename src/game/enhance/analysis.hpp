#pragma once
// gromada::game::enhance, data analysis (issue #21): which sprites emit light and how much,
// and the soft contact-shadow kernel of an occluder, both derived from the original data
// (vid classes, the child chains of act_fight/act_death, graphics formats and pixels), never
// from vid names. Presentation only: nothing here is read by the simulation.
//
// Header-only; depends on libgromada (Vid, VidGraphics, DecodeFrame) and core/.

#include "core/std.hpp"
#include "libgromada/graphics_decoder.hpp"
#include "libgromada/resources.hpp"

namespace gromada::game::enhance {

// ObjectClass values (libgromada ObjectClass / game::cls) and categories used here.
namespace vclass {
inline constexpr int Projectile = 5, CannonMissile = 6, Effect1 = 11, Effect2 = 12, Shell = 14, AviaMissile = 15;
}
namespace vcat {
inline constexpr int Object = 0x2, Vehicle = 0x4;
}
inline constexpr int ActFight = 8, ActDeath = 15;   // child slots of act_fight / act_death

// ---------------------------------------------------------------------------- emitters
// A vid can emit light when it belongs to the shot/explosion family: the projectile and
// effect classes (Projectile, CannonMissile, Effect1, Effect2, Shell, AviaMissile) or any vid
// reachable from the act_fight / act_death children of some vid (muzzle flashes, rays,
// blasts, light spots, smoke: the whole chain that a shot or a death spawns). Whether it
// actually emits, and how much, is decided per frame from its pixels (frameEmission), so
// smoke, dust, debris and wrecks reached by the same chains stay dark.
[[nodiscard]] inline std::vector<std::uint8_t> classifyEmitters(std::span<const Vid* const> vids) {
    const std::size_t n = vids.size();
    std::vector<std::uint8_t> out(n, 0);
    std::vector<int> queue;
    const auto child = [&](const Vid& v, int slot) { return std::abs(static_cast<int>(v.childNvid[static_cast<std::size_t>(slot)])); };
    const auto push = [&](int c) {
        if (c > 0 && static_cast<std::size_t>(c) < n && !out[static_cast<std::size_t>(c)]) {
            out[static_cast<std::size_t>(c)] = 1;
            queue.push_back(c);
        }
    };
    for (const Vid* v : vids)
        if (v) for (const int slot : {ActFight, ActDeath}) push(child(*v, slot));
    while (!queue.empty()) {
        const int c = queue.back();
        queue.pop_back();
        if (const Vid* v = vids[static_cast<std::size_t>(c)])
            for (int slot = 0; slot < 16; ++slot) push(child(*v, slot));
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (!vids[i]) continue;
        switch (static_cast<int>(vids[i]->type)) {
        case vclass::Projectile: case vclass::CannonMissile: case vclass::Effect1: case vclass::Effect2:
        case vclass::Shell: case vclass::AviaMissile: out[i] = 1; break;
        default: break;
        }
    }
    return out;
}

// The light a frame gives off, in frame pixels. `power` is the sum of the per-pixel emission
// (an emitting pixel counts up to 1), `r/g/b` the colour normalised to max 1, (cx, cy) the
// emission centroid in the frame and `spread` the RMS distance of the emission from it.
struct Emission {
    float power = 0, r = 0, g = 0, b = 0, cx = 0, cy = 0, spread = 0;
};

// Emission of one frame of a candidate vid:
//  - light pixels (formats 3 and 4: the original's own light sprites, x8/(8-L) per channel)
//    emit their level, with the colour of the per-channel levels, at LightWeight;
//  - colour pixels (format 8 blended at opacity op/7, or opaque format 2 for small projectile
//    sprites) emit by "heat": bright (max channel above 55%) and saturated colours, so fire,
//    laser and plasma glow while grey smoke and dust do not.
// Opaque format-2 frames of large sprites (debris, wrecks, mines) never emit.
inline constexpr float LightWeight = 0.35f;
inline constexpr int MaxOpaqueEmitterSize = 16;

[[nodiscard]] inline Emission frameEmission(const VidGraphics::Frame& frame, int vidClass) {
    Emission e;
    if (!frame.parent || frame.data.empty()) return e;
    const VidGraphics& g = *frame.parent;
    const int fmt = g.dataFormat;
    const bool projectile = vidClass == vclass::Projectile || vidClass == vclass::CannonMissile ||
                            vidClass == vclass::Shell || vidClass == vclass::AviaMissile;
    const bool opaqueEmits = fmt == 2 && projectile && g.width <= MaxOpaqueEmitterSize && g.height <= MaxOpaqueEmitterSize;
    if (fmt != 3 && fmt != 4 && fmt != 8 && !opaqueEmits) return e;

    struct Acc {
        double w = 0, r = 0, g = 0, b = 0, x = 0, y = 0, xx = 0, yy = 0;
    } acc;
    struct Visitor {
        const std::array<ColorRgb8, 256>* pal;
        Acc* a;
        bool opaque;   // opaque colour pixels count (format 8 at opacity 7, small format-2 projectiles)
        int x = 0, y = 0;
        ClippingInfo begin_image(BoundingBox r) noexcept { return {r, r}; }
        void set_cursor(int cx, int cy) noexcept { x = cx; y = cy; }
        void advance_cursor(int n) noexcept { x += n; }
        void add(double w, double r, double g, double b) noexcept {
            a->w += w; a->r += r * w; a->g += g * w; a->b += b * w;
            a->x += (x + 0.5) * w; a->y += (y + 0.5) * w;
            a->xx += (x + 0.5) * (x + 0.5) * w; a->yy += (y + 0.5) * (y + 0.5) * w;
        }
        void hot(ColorRgb8 c, double alpha) noexcept {
            const double r = c.r / 255.0, g = c.g / 255.0, b = c.b / 255.0;
            const double mx = std::max({r, g, b}), mn = std::min({r, g, b});
            const double sat = mx > 0 ? (mx - mn) / mx : 0;
            const double h = std::clamp((mx - 0.55) / 0.45, 0.0, 1.0) * (0.3 + 0.7 * sat) * alpha;
            if (h > 0) add(h, r, g, b);
        }
        void draw_pixels_shadow(int n) noexcept { x += n; }
        void draw_pixels_indexed(std::span<const IndexedColor> c) noexcept {
            for (const auto i : c) { if (opaque) hot((*pal)[std::to_integer<std::uint8_t>(i)], 1.0); ++x; }
        }
        void draw_pixels_repeat(int n, IndexedColor c) noexcept {
            for (int i = 0; i < n; ++i) { if (opaque) hot((*pal)[std::to_integer<std::uint8_t>(c)], 1.0); ++x; }
        }
        void draw_pixels(std::span<const CompressedColor> c) noexcept { x += static_cast<int>(c.size()); }
        void draw_pixels_repeat(int n, CompressedColor) noexcept { x += n; }
        void draw_pixels_light(int n, std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept {
            const double lr = (r & 7) / 7.0, lg = (g & 7) / 7.0, lb = (b & 7) / 7.0;
            const double w = (lr + lg + lb) / 3.0 * LightWeight;
            for (int i = 0; i < n; ++i, ++x)
                if (w > 0) add(w, lr, lg, lb);
        }
        void draw_pixels_alpha_blend(std::uint8_t t, std::span<const std::byte> c) noexcept {
            const double alpha = ((255 - t) / 42) / 7.0;
            for (const auto i : c) { hot((*pal)[std::to_integer<std::uint8_t>(i)], alpha); ++x; }
        }
    };
    DecodeFrame(frame, Visitor{&g.palette, &acc, fmt == 8 || opaqueEmits});
    if (acc.w <= 0) return e;
    const double m = std::max({acc.r, acc.g, acc.b});
    if (m <= 0) return e;
    e.power = static_cast<float>(acc.w);
    e.r = static_cast<float>(acc.r / m);
    e.g = static_cast<float>(acc.g / m);
    e.b = static_cast<float>(acc.b / m);
    e.cx = static_cast<float>(acc.x / acc.w);
    e.cy = static_cast<float>(acc.y / acc.w);
    const double var = acc.xx / acc.w - (acc.x / acc.w) * (acc.x / acc.w) + acc.yy / acc.w - (acc.y / acc.w) * (acc.y / acc.w);
    e.spread = static_cast<float>(std::sqrt(std::max(0.0, var)));
    return e;
}

// ---------------------------------------------------------------------------- occluders
// Contact-shadow kernel of an occluder frame: the sprite's own silhouette (opaque and blended
// pixels; light and shadow pixels excluded) restricted to the rows of its ground footprint
// (the collision box sizeX x sizeY centred on the sprite position, which is the centre of the
// frame), downsampled to Cell x Cell world pixels and blurred (two box passes). Splatted on
// the ground under the sprite, the part outside the silhouette is a soft dark halo hugging the
// base: an ambient-occlusion approximation (sky hidden by the object near its foot).
struct OcclusionKernel {
    static constexpr int Cell = 2;   // world pixels per kernel cell
    int w = 0, h = 0;                // cells
    float ox = 0, oy = 0;            // top-left of cell (0,0) relative to the sprite position, world px
    std::vector<float> v;            // w*h coverage 0..1 (blurred)
    [[nodiscard]] bool empty() const noexcept { return v.empty(); }
};

// Blur radius of the halo (world px) from the occluder's height: taller objects occlude more.
[[nodiscard]] constexpr float occlusionRadius(int sizeZ) noexcept {
    return std::clamp(static_cast<float>(sizeZ) * 0.22f, 4.0f, 11.0f);
}

[[nodiscard]] inline OcclusionKernel occlusionKernel(const VidGraphics::Frame& frame, int sizeX, int sizeY, int sizeZ) {
    OcclusionKernel k;
    if (!frame.parent || frame.data.empty() || frame.parent->dataFormat == 7) return k;
    const int fw = frame.width(), fh = frame.height();
    if (fw <= 0 || fh <= 0) return k;
    (void)sizeX;
    // footprint rows of the frame (frame centre = sprite position)
    const int band0 = std::clamp(fh / 2 - sizeY / 2, 0, fh), band1 = std::clamp(fh / 2 + (sizeY + 1) / 2 + 1, 0, fh);
    if (band1 <= band0) return k;
    std::vector<float> mask(static_cast<std::size_t>(fw) * static_cast<std::size_t>(band1 - band0), 0.0f);
    struct Visitor {
        float* m;
        int fw, b0, b1;
        int x = 0, y = 0;
        ClippingInfo begin_image(BoundingBox r) noexcept { return {r, r}; }
        void set_cursor(int cx, int cy) noexcept { x = cx; y = cy; }
        void advance_cursor(int n) noexcept { x += n; }
        void run(int n, float a) noexcept {
            if (y >= b0 && y < b1)
                for (int i = 0; i < n; ++i)
                    if (x + i >= 0 && x + i < fw) m[static_cast<std::size_t>(y - b0) * static_cast<std::size_t>(fw) + static_cast<std::size_t>(x + i)] = a;
            x += n;
        }
        void draw_pixels_shadow(int n) noexcept { x += n; }
        void draw_pixels_indexed(std::span<const IndexedColor> c) noexcept { run(static_cast<int>(c.size()), 1.0f); }
        void draw_pixels_repeat(int n, IndexedColor) noexcept { run(n, 1.0f); }
        void draw_pixels(std::span<const CompressedColor> c) noexcept { run(static_cast<int>(c.size()), 1.0f); }
        void draw_pixels_repeat(int n, CompressedColor) noexcept { run(n, 1.0f); }
        void draw_pixels_light(int n, std::uint8_t, std::uint8_t, std::uint8_t) noexcept { x += n; }
        void draw_pixels_alpha_blend(std::uint8_t t, std::span<const std::byte> c) noexcept {
            run(static_cast<int>(c.size()), static_cast<float>((255 - t) / 42) / 7.0f);
        }
    };
    DecodeFrame(frame, Visitor{mask.data(), fw, band0, band1});

    constexpr int C = OcclusionKernel::Cell;
    const float radius = occlusionRadius(sizeZ);
    const int box = std::max(1, static_cast<int>(std::lround(radius / (2.0f * C))));   // two passes ~ gaussian
    const int margin = 2 * box + 1;
    const int cw = (fw + C - 1) / C, ch = (band1 - band0 + C - 1) / C;
    k.w = cw + 2 * margin;
    k.h = ch + 2 * margin;
    k.ox = static_cast<float>(-fw / 2 - margin * C);
    k.oy = static_cast<float>(band0 - fh / 2 - margin * C);
    std::vector<float> a(static_cast<std::size_t>(k.w) * static_cast<std::size_t>(k.h), 0.0f);
    bool any = false;
    for (int y = 0; y < band1 - band0; ++y)
        for (int x = 0; x < fw; ++x) {
            const float m = mask[static_cast<std::size_t>(y) * static_cast<std::size_t>(fw) + static_cast<std::size_t>(x)];
            if (m <= 0) continue;
            any = true;
            a[static_cast<std::size_t>(y / C + margin) * static_cast<std::size_t>(k.w) + static_cast<std::size_t>(x / C + margin)] += m / (C * C);
        }
    if (!any) return k;
    // separable box blur, two passes per axis
    std::vector<float> t(a.size());
    const auto blurX = [&](const std::vector<float>& src, std::vector<float>& dst) {
        for (int y = 0; y < k.h; ++y)
            for (int x = 0; x < k.w; ++x) {
                float s = 0;
                for (int d = -box; d <= box; ++d) {
                    const int xx = x + d;
                    if (xx >= 0 && xx < k.w) s += src[static_cast<std::size_t>(y * k.w + xx)];
                }
                dst[static_cast<std::size_t>(y * k.w + x)] = s / static_cast<float>(2 * box + 1);
            }
    };
    const auto blurY = [&](const std::vector<float>& src, std::vector<float>& dst) {
        for (int y = 0; y < k.h; ++y)
            for (int x = 0; x < k.w; ++x) {
                float s = 0;
                for (int d = -box; d <= box; ++d) {
                    const int yy = y + d;
                    if (yy >= 0 && yy < k.h) s += src[static_cast<std::size_t>(yy * k.w + x)];
                }
                dst[static_cast<std::size_t>(y * k.w + x)] = s / static_cast<float>(2 * box + 1);
            }
    };
    blurX(a, t); blurX(t, a);
    blurY(a, t); blurY(t, a);
    // crop to the cells above 1/256 and keep a zero border of one cell (the splat reads the
    // neighbour of every cell without bounds checks)
    int x0 = k.w, x1 = -1, y0 = k.h, y1 = -1;
    for (int y = 0; y < k.h; ++y)
        for (int x = 0; x < k.w; ++x)
            if (a[static_cast<std::size_t>(y * k.w + x)] >= 1.0f / 256.0f) {
                x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
            }
    if (x1 < 0) return k;
    const int nw = x1 - x0 + 3, nh = y1 - y0 + 3;
    k.v.assign(static_cast<std::size_t>(nw * nh), 0.0f);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const float v = a[static_cast<std::size_t>(y * k.w + x)];
            k.v[static_cast<std::size_t>((y - y0 + 1) * nw + (x - x0 + 1))] = v >= 1.0f / 256.0f ? v : 0.0f;
        }
    k.ox += static_cast<float>((x0 - 1) * C);
    k.oy += static_cast<float>((y0 - 1) * C);
    k.w = nw;
    k.h = nh;
    return k;
}

} // namespace gromada::game::enhance
