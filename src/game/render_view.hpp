#pragma once
// Presentation of the game view (issue #20, docs/rendering.md), session side:
//  - HiResComposite: the internal-resolution picture. The world is drawn at k x the logical
//    size (Engine::drawView); menus, texts, status bar and Effect() keep drawing on the
//    logical picture with their unchanged code. The pixels they changed go back as k x k
//    blocks (crisp nearest-neighbour UI), Effect() colour operations are applied to the
//    whole high-resolution picture and eff_openwindow shows the previous screen outside its
//    window.
//  - drawCursorScaled(): the cursor scaled by k (nearest).
//
// Header-only; depends on the engine contract, hicolor.hpp, screen_effects.hpp and
// input/cursor_file.hpp.

#include "core/std.hpp"
#include "game/engine_api.hpp"
#include "game/hicolor.hpp"
#include "game/screen_effects.hpp"
#include "game/view_raster.hpp"
#include "input/cursor_file.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace gromada::game::view {

// A few threads that run the bands of a picture: run(n, fn) calls fn(0..n-1) on the caller and
// the workers and returns as soon as every call is done. It does not wait for workers that
// have not woken up yet (on phones a sleeping thread on a slow core can take milliseconds): a
// late worker finds the bands taken and goes back to sleep. Bands are taken through one atomic
// ticket (run number << 32 | next band), so a late worker never takes a band of a later run
// with the function of an earlier one. Workers spin briefly before sleeping (a picture runs
// several passes in a row); the destructor joins them.
class WorkerPool {
public:
    explicit WorkerPool(int threads) {
        for (int i = 1; i < threads; ++i) m_threads.emplace_back([this] { loop(); });
    }
    ~WorkerPool() {
        {
            std::lock_guard lock{m_mutex};
            m_stop = true;
            m_stopFlag = true;
        }
        m_wake.notify_all();
        for (auto& t : m_threads) t.join();
    }
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;
    [[nodiscard]] int threads() const noexcept { return static_cast<int>(m_threads.size()) + 1; }

    void run(int n, const std::function<void(int)>& fn) {
        if (m_threads.empty() || n <= 1) {
            for (int i = 0; i < n; ++i) fn(i);
            return;
        }
        std::uint64_t run;
        {
            std::lock_guard lock{m_mutex};
            m_fn = &fn;
            m_n = n;
            run = ++m_generation;
            m_done.store(0);
            m_ticket.store(run << 32);
            m_published.store(run);
        }
        m_wake.notify_all();
        work(run, &fn, n);
        for (int spin = 0; spin < 2000 && m_done.load() != n; ++spin) pause();
        if (m_done.load() != n) {
            std::unique_lock lock{m_mutex};
            m_finished.wait(lock, [&] { return m_done.load() == n; });
        }
    }

    // Threads for the pictures: the cores minus one (the frontend's), at most 4; 1 on a
    // single or dual core machine with nothing to share... `requested` > 0 forces it.
    [[nodiscard]] static int autoThreads(int requested) {
        if (requested > 0) return std::min(requested, 8);
        const unsigned hc = std::thread::hardware_concurrency();
        return hc >= 3 ? static_cast<int>(std::min(4u, hc - 1)) : static_cast<int>(std::max(1u, hc));
    }

private:
    static void pause() noexcept {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        asm volatile("yield");
#endif
    }
    // Takes and runs bands of run `run` until none is left (or a later run has started).
    void work(std::uint64_t run, const std::function<void(int)>* fn, int n) {
        for (;;) {
            std::uint64_t t = m_ticket.load();
            for (;;) {
                if ((t >> 32) != run || static_cast<int>(t & 0xFFFFFFFFu) >= n) return;
                if (m_ticket.compare_exchange_weak(t, t + 1)) break;
            }
            (*fn)(static_cast<int>(t & 0xFFFFFFFFu));
            if (m_done.fetch_add(1) + 1 == n) {   // the last band: wake the caller if it sleeps
                std::lock_guard lock{m_mutex};
                m_finished.notify_all();
            }
        }
    }
    void loop() {
        std::uint64_t seen = 0;
        for (;;) {
            // a short spin: the next pass of the picture usually follows within microseconds
            for (int spin = 0; spin < 4000 && m_published.load() == seen && !m_stopFlag.load(); ++spin) pause();
            const std::function<void(int)>* fn;
            int n;
            {
                std::unique_lock lock{m_mutex};
                m_wake.wait(lock, [&] { return m_stop || m_generation != seen; });
                if (m_stop) return;
                seen = m_generation;
                fn = m_fn;
                n = m_n;
            }
            work(seen, fn, n);
        }
    }

    std::vector<std::thread> m_threads;
    std::mutex m_mutex;
    std::condition_variable m_wake, m_finished;
    const std::function<void(int)>* m_fn = nullptr;   // of run m_generation (valid while it runs)
    int m_n = 0;
    std::uint64_t m_generation = 0;
    std::atomic<std::uint64_t> m_ticket{0}, m_published{0};
    std::atomic<int> m_done{0};
    std::atomic<bool> m_stopFlag{false};
    bool m_stop = false;
};

// RGBA8 (bytes r, g, b, a) -> XRGB8888: a byte swap of each little-endian word, vectorized.
inline void toXrgb(const RGBA8* src, std::uint32_t* out, int n) noexcept {
    static_assert(sizeof(RGBA8) == 4);
    for (int i = 0; i < n; ++i) {
        std::uint32_t v;
        std::memcpy(&v, src + i, 4);
        out[i] = ((v & 0xFFu) << 16) | (v & 0xFF00u) | ((v >> 16) & 0xFFu);
    }
}

// ---------------------------------------------------------------------------- composite
// The internal-resolution picture (k > 1). Per frame:
//   1. the world is drawn into `world` (k * viewW x k * viewH);
//   2. beginLogical(): the logical picture (what menus, texts, the status bar and Effect()
//      draw on, unchanged code) is the world sampled at the centre of each k x k block;
//   3. beforeEffects() / afterEffects(): snapshots around Engine::drawScreenEffects();
//   4. compose(): the output picture: the logical pixels changed by the UI as k x k blocks,
//      the effect's colour operation applied to the world pixels, eff_openwindow's outside the
//      previous screen.
class HiResComposite {
public:
    void beginLogical(FramebufferRef world, FramebufferRef logical, int k, int bands = 1,
                      const std::function<void(int, const std::function<void(int)>&)>& parallel = {}) {
        m_k = k;
        const int lw = logical.extent(1), lh = logical.extent(0);
        const int ww = world.extent(1);
        const int off = k / 2;
        m_world.resize(static_cast<std::size_t>(lw) * static_cast<std::size_t>(lh));
        const auto job = [&](int b) {   // rows [lh b / bands, lh (b + 1) / bands): the logical rows and their snapshot
            for (int y = lh * b / bands, e = lh * (b + 1) / bands; y < e; ++y) {
                const RGBA8* src = world.data_handle() + static_cast<std::size_t>(y * k + off) * static_cast<std::size_t>(ww) + off;
                RGBA8* dst = logical.data_handle() + static_cast<std::size_t>(y) * static_cast<std::size_t>(lw);
                RGBA8* snap = m_world.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(lw);
                for (int x = 0; x < lw; ++x) snap[x] = dst[x] = src[static_cast<std::size_t>(x) * static_cast<std::size_t>(k)];
            }
        };
        bands = std::clamp(bands, 1, std::max(1, lh));
        if (bands > 1 && parallel) parallel(bands, job);
        else for (int b = 0; b < bands; ++b) job(b);
        m_haveUi = m_haveFx = false;
    }
    void beforeEffects(FramebufferRef logical) { snapshot(logical, m_ui); m_haveUi = true; }
    void afterEffects(FramebufferRef logical, const ScreenEffectView& fx) {
        snapshot(logical, m_fx);
        m_haveFx = true;
        m_effect = fx;
    }

    // The final picture into `dst` (XRGB8888, `dstPitch` pixels per row): the world converted,
    // with the logical pixels changed by the UI as k x k blocks, the effect's colour operation
    // and eff_openwindow's old screen. One pass over the internal-resolution picture.
    // `parallel` (optional): runs fn(0..n-1) on several threads (WorkerPool::run).
    void compose(FramebufferRef world, FramebufferRef logical, std::uint32_t* dst, std::size_t dstPitch, int bands = 1,
                 const std::function<void(int, const std::function<void(int)>&)>& parallel = {}) {
        const int lh = logical.extent(0);
        bands = std::clamp(bands, 1, std::max(1, lh));
        m_bandMode.resize(static_cast<std::size_t>(bands));
        m_bandFill.resize(static_cast<std::size_t>(bands));
        const auto job = [&](int b) {
            composeRows(world, logical, dst, dstPitch, lh * b / bands, lh * (b + 1) / bands, m_bandMode[static_cast<std::size_t>(b)],
                        m_bandFill[static_cast<std::size_t>(b)]);
        };
        if (bands > 1 && parallel) parallel(bands, job);
        else for (int b = 0; b < bands; ++b) job(b);
    }

private:
    void composeRows(FramebufferRef world, FramebufferRef logical, std::uint32_t* dst, std::size_t dstPitch, int rowBegin, int rowEnd,
                     std::vector<std::uint8_t>& modes, std::vector<std::uint32_t>& fills) const {
        const int k = m_k, lw = logical.extent(1), lh = logical.extent(0), ww = world.extent(1);
        const std::size_t n = static_cast<std::size_t>(lw) * static_cast<std::size_t>(lh);
        if (m_world.size() != n || world.extent(0) != lh * k || ww != lw * k) return;
        const RGBA8* fin = logical.data_handle();
        const RGBA8* ui = m_haveUi ? m_ui.data() : fin;
        const RGBA8* fx = m_haveFx ? m_fx.data() : (m_haveUi ? m_ui.data() : fin);
        const bool colour = m_haveFx && m_effect.colorKind != 0 && m_effect.colorLevel != 0;
        const ScreenEffects::ColourLut lut = colour ? ScreenEffects::colourLut(m_effect.colorKind, m_effect.colorLevel) : ScreenEffects::ColourLut{};
        const bool window = m_haveFx && m_effect.window;
        const auto xrgb = [](const RGBA8& p) { return (std::uint32_t{p.r} << 16) | (std::uint32_t{p.g} << 8) | p.b; };
        modes.resize(static_cast<std::size_t>(lw));
        fills.resize(static_cast<std::size_t>(lw));
        for (int y = rowBegin; y < rowEnd; ++y) {
            // per logical pixel: 0 = the world, 1 = the world through the effect, 2 = a block
            const std::size_t r = static_cast<std::size_t>(y) * static_cast<std::size_t>(lw);
            const bool rowOutside = window && (y < m_effect.top || y > m_effect.bottom);
            // Fast path (most rows while playing): nothing drawn over the world on this row
            // and no window, i.e. every pixel would get mode 0 (1 with a colour effect: the
            // whole row through its tables). Whole-row comparisons (alpha included: if only
            // alpha differs the per-pixel path below decides, same result).
            if (!window) {
                const std::size_t bytes = static_cast<std::size_t>(lw) * sizeof(RGBA8);
                if (std::memcmp(fin + r, fx + r, bytes) == 0 && std::memcmp(ui + r, m_world.data() + r, bytes) == 0) {
                    for (int yy = 0; yy < k; ++yy) {
                        const RGBA8* src = world.data_handle() + static_cast<std::size_t>(y * k + yy) * static_cast<std::size_t>(ww);
                        std::uint32_t* out = dst + static_cast<std::size_t>(y * k + yy) * dstPitch;
                        if (!colour) {
                            toXrgb(src, out, ww);
                        } else {
                            for (int X = 0; X < ww; ++X) {
                                const RGBA8& p = src[X];
                                out[X] = std::uint32_t{lut.r[p.r]} << 16 | std::uint32_t{lut.g[p.g]} << 8 | lut.b[p.b];
                            }
                        }
                    }
                    continue;
                }
            }
            for (int x = 0; x < lw; ++x) {
                const std::size_t i = r + static_cast<std::size_t>(x);
                const std::uint32_t f = word(fin[i]), e = word(fx[i]);
                const bool outside = rowOutside || (window && (x < m_effect.left || x > m_effect.right));
                std::uint8_t mode = colour ? 1 : 0;
                if (f != e) { mode = 2; fills[static_cast<std::size_t>(x)] = xrgb(fin[i]); }   // after the effects (HUD)
                else if (outside || word(ui[i]) != word(m_world[i])) { mode = 2; fills[static_cast<std::size_t>(x)] = xrgb(fx[i]); } // old screen; UI
                modes[static_cast<std::size_t>(x)] = mode;
            }
            for (int yy = 0; yy < k; ++yy) {
                const RGBA8* src = world.data_handle() + static_cast<std::size_t>(y * k + yy) * static_cast<std::size_t>(ww);
                std::uint32_t* out = dst + static_cast<std::size_t>(y * k + yy) * dstPitch;
                for (int x = 0; x < lw;) {
                    const std::uint8_t mode = modes[static_cast<std::size_t>(x)];
                    if (mode == 2) {
                        std::fill(out + x * k, out + x * k + k, fills[static_cast<std::size_t>(x)]);
                        ++x;
                        continue;
                    }
                    int e = x + 1;   // a run of world pixels: one tight loop
                    while (e < lw && modes[static_cast<std::size_t>(e)] == mode) ++e;
                    if (mode == 0) {
                        toXrgb(src + x * k, out + x * k, (e - x) * k);
                    } else {
                        for (int X = x * k; X < e * k; ++X) {
                            const RGBA8& p = src[X];
                            out[X] = std::uint32_t{lut.r[p.r]} << 16 | std::uint32_t{lut.g[p.g]} << 8 | lut.b[p.b];
                        }
                    }
                    x = e;
                }
            }
        }
    }

    static bool same(const RGBA8& a, const RGBA8& b) noexcept { return a.r == b.r && a.g == b.g && a.b == b.b; }
    static std::uint32_t word(const RGBA8& p) noexcept {   // r, g, b (alpha ignored)
        std::uint32_t v;
        std::memcpy(&v, &p, 4);
        return v & 0x00FFFFFFu;
    }
    static void snapshot(FramebufferRef fb, std::vector<RGBA8>& out) {
        const std::size_t n = static_cast<std::size_t>(fb.extent(0)) * static_cast<std::size_t>(fb.extent(1));
        out.assign(fb.data_handle(), fb.data_handle() + n);
    }
    int m_k = 1;
    std::vector<RGBA8> m_world, m_ui, m_fx;
    std::vector<std::vector<std::uint8_t>> m_bandMode;    // per band: mode of each logical pixel of a row
    std::vector<std::vector<std::uint32_t>> m_bandFill;
    bool m_haveUi = false, m_haveFx = false;
    ScreenEffectView m_effect;
};

// The cursor at logical (x, y), every texel a k x k block.
inline void drawCursorScaled(const input::CursorImage& img, int x, int y, int k, std::uint32_t* fb, int width, int height,
                             std::size_t pitchBytes) {
    if (k <= 1) { input::drawCursor(img, x, y, fb, width, height, pitchBytes); return; }
    if (img.empty() || !fb) return;
    const int x0 = (x - img.hotX) * k, y0 = (y - img.hotY) * k;
    for (int py = std::max(0, -y0); py < img.height * k && y0 + py < height; ++py) {
        auto* row = reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(fb) + static_cast<std::size_t>(y0 + py) * pitchBytes);
        for (int px = std::max(0, -x0); px < img.width * k && x0 + px < width; ++px) {
            const input::CursorTexel& t = img.at(px / k, py / k);
            std::uint32_t& dst = row[x0 + px];
            if (t.mode == input::CursorTexel::Xor) {
                dst = (dst ^ t.rgb) & 0xFFFFFFu;
            } else if (t.alpha == 255) {
                dst = t.rgb;
            } else if (t.alpha) {
                const std::uint32_t a = t.alpha, ia = 255 - a;
                auto ch = [&](int s) { return (((t.rgb >> s) & 255) * a + ((dst >> s) & 255) * ia + 127) / 255; };
                dst = (ch(16) << 16) | (ch(8) << 8) | ch(0);
            }
        }
    }
}

} // namespace gromada::game::view
