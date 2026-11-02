#pragma once
// Cache of the world's picture (docs/rendering.md, "Cache dell'immagine"): the picture of the
// draw list is kept between frames and only the parts that change are drawn again; the final
// picture is exactly the one drawBand() makes from the whole list.
//
// - A pixel's value is the result of the commands that cover it, in order, on black. The
//   commands of this frame are matched in order with those of the cached picture (a common
//   subsequence of equal keys: same frame, palette contents, position, pass); every pixel
//   outside the bounds of the unmatched ones is covered by the same commands in the same order,
//   so it is unchanged. The bounds of the unmatched ones (where they were, where they are) are
//   drawn again: all the commands that touch them, in order, on black.
// - With an integer scale k and a command at an integer world position (left = 256 m), the
//   framebuffer column of world column m is k m - N, N = ceil(originX k / 256) (Raster::column
//   is a floor). So the pixels of such commands depend on the origin only through (Nx, Ny):
//   when the camera moves the cached picture is shifted by whole pixels, also at sub-pixel
//   origins, and the uncovered strips are drawn. A command at a sub-pixel position (something
//   moving, extrapolated) has the origin in its key: it is drawn again when the camera moves.
// - A rectangle is drawn on its own buffer through a raster with origin o + 256 q (its corner
//   aligned to x0 = k q): column'(w) = floor((w - o - 256 q) k / 256) = column(w) - x0 for every
//   w, so its pixels are those of the whole picture.
//
// Header-only; depends on view_raster.hpp.

#include "game/view_raster.hpp"

#include <cstring>
#include <functional>
#include <unordered_set>

namespace gromada::game::view {

class PictureCache {
public:
    using Parallel = std::function<void(int n, const std::function<void(int)>& fn)>;

    void invalidate() noexcept { m_valid = false; }

    // The commands at the start of `list` the cache can hold: world frames (the first band
    // pass or screen-space frame ends the prefix).
    [[nodiscard]] static std::size_t prefixLength(std::span<const DrawCommand> list) noexcept {
        std::size_t n = 0;
        while (n < list.size() && !list[n].bandPass && !list[n].screen && list[n].frame && list[n].palette) ++n;
        return n;
    }

    // Brings the cached picture to `prefix` drawn through `world` on black. False when the
    // scale is not an integer (the caller draws the commands itself). `inPlace`: the picture is
    // kept in world.fb itself (nothing else is drawn over it), no copy; `targetIntact`: world.fb
    // still holds what was left there by the last call (the caller vouches nothing wrote it).
    // Then inTarget() tells whether the picture is in world.fb, otherwise copyRows() puts it there.
    bool update(std::span<const DrawCommand> prefix, const Raster& world, const Parallel& parallel, int bands, bool inPlace = false,
                bool targetIntact = false) {
        const auto frac = world.scale & ((std::int64_t{1} << ScaleBits) - 1);
        const int k = frac == 0 ? static_cast<int>(world.scale >> ScaleBits) : 0;
        if (k < 1 || k > 64 || prefix.empty()) {
            m_valid = false;
            return false;
        }
        const int W = world.width(), H = world.height();
        const std::int64_t nx = -world.column(0), ny = -world.row(0);
        const auto mask = (std::int64_t{1} << SubBits) - 1;

        m_paletteHashes.clear();
        m_newKeys.clear();
        m_newKeys.reserve(prefix.size());
        for (const DrawCommand& c : prefix) {
            const int w = c.frame->width(), h = c.frame->height();
            const bool exact = ((c.left | c.top) & mask) == 0;
            const FrameBounds b = frameBounds(world, c.left, c.top, w, h);
            m_newKeys.push_back({c.frame, c.palette, paletteHash(c.palette), c.left, c.top, exact ? 0 : world.originX,
                                 exact ? 0 : world.originY, w, h, static_cast<std::uint8_t>(c.pass), c.skipShadowPixels, !exact,
                                 {b.x0, b.y0, b.x1, b.y1}});
        }

        if (W != m_w || H != m_h || k != m_k) m_valid = false;
        // where the last picture is, where this one goes
        RGBA8* target = world.fb.data_handle();
        const std::size_t pixels = static_cast<std::size_t>(W) * static_cast<std::size_t>(H);
        if (m_inTarget && !(targetIntact && target == m_target)) m_valid = false;
        if (inPlace) {
            if (m_valid && !m_inTarget) std::memcpy(target, m_image.data(), pixels * sizeof(RGBA8));
            m_img = target;
        } else {
            if (m_valid && m_inTarget) m_image.assign(target, target + pixels);
            m_image.resize(pixels);
            m_img = m_image.data();
        }
        m_inTarget = inPlace;
        m_target = target;
        bool full = !m_valid;
        const std::int64_t dx = nx - m_nx, dy = ny - m_ny;
        if (!full && (dx >= W || -dx >= W || dy >= H || -dy >= H)) full = true;
        m_rects.clear();
        m_k = k;
        if (!full) {
            shift(static_cast<int>(dx), static_cast<int>(dy));
            diff(static_cast<int>(dx), static_cast<int>(dy));
            merge();
            long long area = 0;
            for (const Rect& r : m_rects) area += r.area();
            if (area * 2 > static_cast<long long>(W) * H) full = true;
        }
        m_w = W;
        m_h = H;
        m_nx = nx;
        m_ny = ny;
        if (full) {
            std::fill_n(m_img, pixels, RGBA8{{0, 0, 0}, 255});
            drawRect({0, 0, W, H}, prefix, world, parallel, bands);
        } else {
            for (const Rect& r : m_rects) drawRect(r, prefix, world, parallel, bands);
        }
        std::swap(m_keys, m_newKeys);
        m_valid = true;
        return true;
    }

    [[nodiscard]] bool inTarget() const noexcept { return m_inTarget; }

    // Rows [y0, y1) of the cached picture into fb (same size as the raster of update()).
    void copyRows(FramebufferRef fb, int y0, int y1) const noexcept {
        if (y1 <= y0 || m_inTarget) return;
        std::memcpy(fb.data_handle() + static_cast<std::size_t>(y0) * static_cast<std::size_t>(m_w),
                    m_image.data() + static_cast<std::size_t>(y0) * static_cast<std::size_t>(m_w),
                    static_cast<std::size_t>(y1 - y0) * static_cast<std::size_t>(m_w) * sizeof(RGBA8));
    }

private:
    struct Rect {
        int x0, y0, x1, y1;
        [[nodiscard]] long long area() const noexcept { return static_cast<long long>(x1 - x0) * (y1 - y0); }
    };
    struct Key {
        const VidGraphics::Frame* frame;
        const std::array<ColorRgb8, 256>* palette;
        std::uint64_t paletteHash;
        std::int64_t left, top;
        std::int64_t originX, originY;   // sub-pixel positions only (0 at integer positions)
        int w, h;
        std::uint8_t pass;
        bool skipShadowPixels;
        bool subPixel;
        Rect bounds;   // framebuffer pixels of that frame (not compared)
        [[nodiscard]] bool same(const Key& o) const noexcept {
            return frame == o.frame && palette == o.palette && paletteHash == o.paletteHash && left == o.left && top == o.top &&
                   originX == o.originX && originY == o.originY && w == o.w && h == o.h && pass == o.pass &&
                   skipShadowPixels == o.skipShadowPixels && subPixel == o.subPixel;
        }
    };

    [[nodiscard]] static std::uint64_t hashKey(const Key& k) noexcept {
        std::uint64_t h = k.paletteHash;
        const auto mix = [&](std::uint64_t v) { h = (h ^ v) * 0x100000001b3ULL; h ^= h >> 29; };
        mix(reinterpret_cast<std::uintptr_t>(k.frame));
        mix(static_cast<std::uint64_t>(k.left));
        mix(static_cast<std::uint64_t>(k.top));
        mix(static_cast<std::uint64_t>(k.originX));
        mix(static_cast<std::uint64_t>(k.originY));
        mix(static_cast<std::uint64_t>(k.pass) << 1 | static_cast<std::uint64_t>(k.skipShadowPixels));
        return h;
    }
    // The palette's contents (palettes are rebuilt in place when the gamma changes).
    std::uint64_t paletteHash(const std::array<ColorRgb8, 256>* p) {
        for (const auto& [ptr, h] : m_paletteHashes)
            if (ptr == p) return h;
        std::uint64_t h = 0xcbf29ce484222325ULL;
        const auto* bytes = reinterpret_cast<const unsigned char*>(p->data());
        for (std::size_t i = 0; i < sizeof(*p); ++i) h = (h ^ bytes[i]) * 0x100000001b3ULL;
        m_paletteHashes.emplace_back(p, h);
        return h;
    }

    // Clipped to the picture, corner aligned to whole source pixels (multiples of k).
    void addRect(Rect r) {
        r = {std::max(r.x0, 0), std::max(r.y0, 0), std::min(r.x1, m_w), std::min(r.y1, m_h)};
        if (r.x0 >= r.x1 || r.y0 >= r.y1) return;
        r.x0 -= r.x0 % m_k;
        r.y0 -= r.y0 % m_k;
        m_rects.push_back(r);
    }

    // New picture (x, y) = old picture (x + dx, y + dy); the uncovered strips are redrawn.
    void shift(int dx, int dy) {
        if (dx == 0 && dy == 0) return;
        const int W = m_w, H = m_h;
        const int cx0 = std::max(0, -dx), cx1 = std::min(W, W - dx);   // columns with a source
        const int cy0 = std::max(0, -dy), cy1 = std::min(H, H - dy);
        const auto rowAt = [&](int y) { return m_img + static_cast<std::size_t>(y) * static_cast<std::size_t>(W); };
        const std::size_t bytes = static_cast<std::size_t>(cx1 - cx0) * sizeof(RGBA8);
        if (dy >= 0)
            for (int y = cy0; y < cy1; ++y) std::memmove(rowAt(y) + cx0, rowAt(y + dy) + cx0 + dx, bytes);
        else
            for (int y = cy1 - 1; y >= cy0; --y) std::memmove(rowAt(y) + cx0, rowAt(y + dy) + cx0 + dx, bytes);
        if (cy0 > 0) addRect({0, 0, W, cy0});
        if (cy1 < H) addRect({0, cy1, W, H});
        if (cx0 > 0) addRect({0, cy0, cx0, cy1});
        if (cx1 < W) addRect({cx1, cy0, W, cy1});
    }

    // Matches the cached commands with the new ones in order (a common subsequence); the
    // bounds of the others are redrawn: where they were (moved with the picture), where they are.
    void diff(int dx, int dy) {
        m_oldSet.clear();
        m_newSet.clear();
        for (const Key& k : m_keys) m_oldSet.insert(hashKey(k));
        for (const Key& k : m_newKeys) m_newSet.insert(hashKey(k));
        const auto gone = [&](const Key& k) { addRect({k.bounds.x0 - dx, k.bounds.y0 - dy, k.bounds.x1 - dx, k.bounds.y1 - dy}); };
        const auto added = [&](const Key& k) { addRect(k.bounds); };
        std::size_t i = 0, j = 0;
        while (i < m_keys.size() && j < m_newKeys.size()) {
            if (m_keys[i].same(m_newKeys[j])) { ++i; ++j; continue; }
            if (!m_newSet.contains(hashKey(m_keys[i]))) gone(m_keys[i++]);
            else if (!m_oldSet.contains(hashKey(m_newKeys[j]))) added(m_newKeys[j++]);
            else gone(m_keys[i++]);   // moved in the order: its new place is unmatched later
        }
        for (; i < m_keys.size(); ++i) gone(m_keys[i]);
        for (; j < m_newKeys.size(); ++j) added(m_newKeys[j]);
    }

    // Fewer, larger rectangles (every rectangle decodes the frames that touch it): two are
    // joined when their bounding box is not much larger than both.
    void merge() {
        constexpr long long Slack = 64 * 64;
        bool joined = true;
        while (joined && m_rects.size() > 1) {
            joined = false;
            for (std::size_t a = 0; a < m_rects.size() && !joined; ++a)
                for (std::size_t b = a + 1; b < m_rects.size(); ++b) {
                    const Rect& p = m_rects[a];
                    const Rect& q = m_rects[b];
                    const Rect u{std::min(p.x0, q.x0), std::min(p.y0, q.y0), std::max(p.x1, q.x1), std::max(p.y1, q.y1)};
                    if (u.area() <= p.area() + q.area() + Slack) {
                        m_rects[a] = u;
                        m_rects[b] = m_rects.back();
                        m_rects.pop_back();
                        joined = true;
                        break;
                    }
                }
        }
    }

    // The prefix drawn on black in rectangle r of the picture, through a raster on the
    // rectangle's own buffer that gives the same columns and rows (r.x0, r.y0: multiples of k).
    void drawRect(Rect r, std::span<const DrawCommand> prefix, const Raster& world, const Parallel& parallel, int bands) {
        const int rw = r.x1 - r.x0, rh = r.y1 - r.y0;
        if (rw <= 0 || rh <= 0) return;
        const bool whole = rw == m_w && rh == m_h;
        if (!whole) m_tmp.assign(static_cast<std::size_t>(rw) * static_cast<std::size_t>(rh), RGBA8{{0, 0, 0}, 255});
        const FramebufferRef buf{whole ? m_img : m_tmp.data(), rh, rw};
        const Raster t{buf, world.originX + (std::int64_t{r.x0 / m_k} << SubBits), world.originY + (std::int64_t{r.y0 / m_k} << SubBits),
                       world.scale};
        const int n = (bands > 1 && parallel && r.area() >= 65536) ? bands : 1;
        if (m_scratch.size() < static_cast<std::size_t>(n)) m_scratch.resize(static_cast<std::size_t>(n));
        const auto band = [&](int b) {
            Raster tb = t;
            tb.clipTop = static_cast<int>(static_cast<std::int64_t>(rh) * b / n);
            tb.clipBottom = static_cast<int>(static_cast<std::int64_t>(rh) * (b + 1) / n);
            const int by0 = r.y0 + tb.clipTop, by1 = r.y0 + tb.clipBottom;
            ScaleScratch& s = m_scratch[static_cast<std::size_t>(b)];
            for (std::size_t c = 0; c < prefix.size(); ++c) {
                const Rect& fb = m_newKeys[c].bounds;
                if (fb.x1 <= r.x0 || fb.x0 >= r.x1 || fb.y1 <= by0 || fb.y0 >= by1) continue;
                const DrawCommand& cmd = prefix[c];
                drawFrameScaled(*cmd.frame, cmd.left, cmd.top, *cmd.palette, tb, s, cmd.pass, cmd.skipShadowPixels);
            }
        };
        if (n > 1) parallel(n, band);
        else band(0);
        if (!whole)
            for (int y = 0; y < rh; ++y)
                std::memcpy(m_img + static_cast<std::size_t>(r.y0 + y) * static_cast<std::size_t>(m_w) + r.x0,
                            m_tmp.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(rw),
                            static_cast<std::size_t>(rw) * sizeof(RGBA8));
    }

    bool m_valid = false;
    int m_w = 0, m_h = 0, m_k = 1;
    std::int64_t m_nx = 0, m_ny = 0;
    std::vector<RGBA8> m_image, m_tmp;   // the picture (when not kept in the target), a rectangle
    RGBA8* m_img = nullptr;              // the picture: m_image's data or the target
    const RGBA8* m_target = nullptr;     // the target of the last update
    bool m_inTarget = false;
    std::vector<Key> m_keys, m_newKeys;
    std::vector<Rect> m_rects;
    std::unordered_set<std::uint64_t> m_oldSet, m_newSet;
    std::vector<std::pair<const std::array<ColorRgb8, 256>*, std::uint64_t>> m_paletteHashes;
    std::vector<ScaleScratch> m_scratch;
};

} // namespace gromada::game::view
