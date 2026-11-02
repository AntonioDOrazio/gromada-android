#pragma once
// gromada::game::ScreenEffects: Effect() of the scripts (issue #15), i.e. SVGA::Effect
// 0x406e40 and the effect half of SVGA::EndFrame 0x406ea0, applied to the finished picture
// (world, menus, texts, status bar) once per frame, like the original does on the locked
// 16-bit surface just before presenting it.
//
//  id  name            pixel operation (16-bit, hicolor.hpp)          level sequence
//  1   eff_nuke        brighten r,g,b by L (x8/(8-L), 0x405da0)         1,3,5 then -32,-30..-2
//  2   eff_rnuke       brighten red only (0x405e20, p = L << 6)        1,3,5 then -30..-2
//  3   eff_gnuke       brighten green only (p = L << 3)                1,3,5 then -30..-2
//  4   eff_bnuke       brighten blue only (p = L)                      1,3,5 then -30..-2
//  5   eff_fade        fade |L| (x15/16 .. x1/32, 9 = black, 0x405c40)  1..9 then -9..-1
//  6   eff_openwindow  the new picture opens from the centre over the previous one
//  255 eff_random      rand() % 6 + 1;  0 eff_none stops the current effect
// Negative levels are shown as (-L) >> 2 (eff_nuke, eff_bnuke) or ((-L) & ~3) >> 2 (red,
// green), i.e. 7 down to 0. The level steps once per frame when at least 71 ms of game clock
// passed since the last step (0x42df90, never reset: the first step of a new effect is
// usually immediate); the jump to the negative branch does not restart that timer, so the
// first negative level lasts one frame. The effect survives map loads (it lives in the SVGA
// object): eff_fade darkens the old map, Load() happens at 700 ms, the new map fades in.
//
// eff_openwindow (0x4070e4): Effect(6) presents the current surface three times (0x405700),
// so every buffer of the flip chain holds the previous picture; then the clipping rectangle
// (+0x1a54) of every drawing primitive shrinks to the centre (+-5 px) and grows each frame by
// min(4, w*dt/1600) px horizontally and min(3, h*dt/1600) vertically until it is the whole
// screen again, and only that rectangle is copied to the screen (0x402030). We keep the
// surface (`m_offscreen`) and the previous screen (`m_screen`) to compose the same picture.
// The original grows the window once per drawn frame (4/3 px per frame at any usual frame
// rate): here it grows once per ReferenceFrameMs of game clock (the mean frame time of the
// original's demos), unless frames are locked to recorded ones (demo playback).
//
// Header-only.

#include "core/std.hpp"
#include "game/hicolor.hpp"

namespace gromada::game {

class ScreenEffects {
public:
    // Mean frame time of gromada.exe in its demos (33.7, 37.5 and 40.0 ms): the original's
    // per-frame behaviours (turret turning, eff_openwindow) are paced on it.
    static constexpr int ReferenceFrameMs = 37;
    static constexpr int StepMs = 71;   // 0x47

    // Pixel operation of an effect: kind 1 = 0x405da0(level), 2 = 0x405e20(p), 5 = 0x405c40(level).
    [[nodiscard]] static hicolor::Color16 applyOp(int kind, int level, hicolor::Color16 c) noexcept {
        switch (kind) {
            case 1: {
                if (level <= 0) return c;
                const int l = level > 7 ? 7 : level;
                return hicolor::brighten(c, l, l, l);
            }
            case 2:
                if (level == 0) return c;
                return hicolor::brighten(c, (level >> 6) & 7, (level >> 3) & 7, level & 7);
            case 5: return hicolor::fade(c, level);
            default: return c;
        }
    }

    [[nodiscard]] int effect() const noexcept { return m_effect; }
    [[nodiscard]] int level() const noexcept { return m_level; }
    [[nodiscard]] bool windowOpening() const noexcept { return m_effect == 6 && m_level > 1; }

    // Effect(e) 0x406e40. `rand` is the shared MSVC rand() (eff_random only).
    template <class Rand>
    void start(int e, Rand&& rand) {
        if (m_effect == 6) return;   // ignored while the window opens
        if (e == 255) {
            start(rand() % 6 + 1, rand);
            if (m_effect == 6) captureScreen();
            return;
        }
        m_level = 1;
        m_effect = e;
        if (m_effect == 6) captureScreen();
    }

    // End of a frame (0x406ea0): applies the current effect to `fb` (the finished picture)
    // and steps it. Without a picture (frame not drawn) only the timing runs, like the
    // original with draw == 0. `frameLocked`: one original frame per call (demo playback).
    void endFrame(std::optional<FramebufferRef> fb, std::uint32_t clock, bool frameLocked) {
        if (fb) beginPicture(*fb);
        switch (m_effect) {
            case 1: case 2: case 3: case 4: case 5: stepColour(fb.has_value(), clock); break;
            case 6: stepWindow(clock, frameLocked); break;
            default: break;
        }
        if (fb) present(*fb);
    }

private:
    struct Rect {
        int left = 0, right = -1, top = 0, bottom = -1;   // inclusive, like +0x1a54..+0x1a60
        bool operator==(const Rect&) const = default;
    };

    void beginPicture(FramebufferRef fb) {
        const int w = fb.extent(1), h = fb.extent(0);
        if (w != m_w || h != m_h) {
            m_w = w;
            m_h = h;
            m_full = {0, w - 1, 0, h - 1};
            m_clip = m_full;
            m_offscreen.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), RGBA8{{0, 0, 0}, 255});
            m_screen = m_offscreen;
            if (m_effect == 6) m_effect = 0;
        }
        // The frame was drawn into the surface through the clipping rectangle.
        copyRect(fb, m_clip, true);
    }

    void present(FramebufferRef fb) {
        if (m_clip == m_full) {
            if (std::exchange(m_dirty, false)) copyRect(fb, m_full, false);
            return;
        }
        m_dirty = false;
        for (int y = 0; y < m_h; ++y)
            for (int x = 0; x < m_w; ++x) {
                const bool in = x >= m_clip.left && x <= m_clip.right && y >= m_clip.top && y <= m_clip.bottom;
                fb[y, x] = (in ? m_offscreen : m_screen)[idx(x, y)];
            }
    }

    void copyRect(FramebufferRef fb, const Rect& r, bool toSurface) {
        for (int y = std::max(r.top, 0); y <= std::min(r.bottom, m_h - 1); ++y)
            for (int x = std::max(r.left, 0); x <= std::min(r.right, m_w - 1); ++x) {
                if (toSurface) m_offscreen[idx(x, y)] = fb[y, x];
                else fb[y, x] = m_offscreen[idx(x, y)];
            }
    }

    // 0x405700: the surface is presented three times, the screen shows it everywhere.
    void captureScreen() { m_screen = m_offscreen; }

    [[nodiscard]] std::size_t idx(int x, int y) const noexcept {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(m_w) + static_cast<std::size_t>(x);
    }

    void applyToSurface(int kind, int level) {
        if (level == 0) return;
        m_dirty = true;
        for (auto& p : m_offscreen) p = hicolor::toPixel(applyOp(kind, level, hicolor::fromPixel(p)));
    }

    // Cases 1..5 of 0x406ea0.
    void stepColour(bool draw, std::uint32_t clock) {
        const int l = m_level;
        if (draw) {
            switch (m_effect) {
                case 1: applyToSurface(1, l > 0 ? l : (-l) >> 2); break;
                case 2: applyToSurface(2, l > 0 ? l << 6 : ((-l) & ~3) << 4); break;
                case 3: applyToSurface(2, l > 0 ? l << 3 : ((-l) & ~3) << 1); break;
                case 4: applyToSurface(2, l > 0 ? l : (-l) >> 2); break;
                case 5: applyToSurface(5, l < 0 ? -l : l); break;
                default: break;
            }
        }
        if (clock - m_last >= static_cast<std::uint32_t>(StepMs)) {
            if (m_effect == 5) {
                if (++m_level > 9) m_level = -9;
                else m_last = clock;
            } else {
                m_level += 2;
                if (m_level > 6) m_level = m_effect == 1 ? -32 : -30;
                else m_last = clock;
            }
        }
        if (m_level == 0) m_effect = 0;
    }

    // Case 6 of 0x406ea0.
    void stepWindow(std::uint32_t clock, bool frameLocked) {
        if (m_w <= 0) { m_effect = 0; return; }
        if (m_level == 1) {
            m_level = 2;
            m_last = clock;
            const int cx = m_w / 2, cy = m_h / 2;
            m_clip = {cx - 5, cx + 5, cy - 5, cy + 5};
            return;
        }
        if (m_level == 0 || m_last == clock) return;
        if (frameLocked) {
            growWindow(clock - m_last, clock);
            return;
        }
        // Paced on the original's frame time: one step per ReferenceFrameMs of game clock.
        for (int n = 0; n < 2 && m_effect == 6 && clock - m_last >= static_cast<std::uint32_t>(ReferenceFrameMs); ++n)
            growWindow(ReferenceFrameMs, m_last + ReferenceFrameMs);
        if (m_effect == 6 && clock - m_last >= static_cast<std::uint32_t>(ReferenceFrameMs)) m_last = clock;
    }

    void growWindow(std::uint32_t dt, std::uint32_t now) {
        const auto step = [](int size, std::uint32_t d) {
            const std::uint32_t x = (static_cast<std::uint32_t>(size) * d) >> 1;
            return static_cast<int>((static_cast<std::uint64_t>(x) * 0x51eb851fu) >> 40);   // x / 800
        };
        const int dx = std::min(step(m_w, dt), 4), dy = std::min(step(m_h, dt), 3);
        if (dx == 0 || dy == 0) return;
        m_last = now;
        m_clip.left = std::max(m_clip.left - dx, m_full.left);
        m_clip.right = std::min(m_clip.right + dx, m_full.right);
        m_clip.top = std::max(m_clip.top - dy, m_full.top);
        m_clip.bottom = std::min(m_clip.bottom + dy, m_full.bottom);
        if (m_clip == m_full) m_effect = 0;
    }

    int m_effect = 0, m_level = 0;   // +0x1fbb, +0x1fbf
    std::uint32_t m_last = 0;        // 0x42df90
    int m_w = 0, m_h = 0;
    Rect m_full, m_clip;
    std::vector<RGBA8> m_offscreen, m_screen;
    bool m_dirty = false;   // the surface differs from the picture (an effect touched it)
};

} // namespace gromada::game
