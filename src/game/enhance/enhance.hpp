#pragma once
// gromada::game::enhance: optional graphics enhancements (issue #21), OFF by default.
// docs/enhancements.md is the design (what is feasible in software, costs, GPU proposal);
// docs/architecture.md "Miglioramenti grafici" documents the hook.
//
// Two presentation-only passes over the world picture, at whatever resolution the
// framebuffer has (the pipeline of issue #20 may draw the world at 1x..4x):
//
//  1. contact shadows / approximate ambient occlusion, applied to the GROUND after the
//     ground layers (0..4) and before the objects (layer 5 and above) are drawn, so that the
//     objects cover their own share of the darkening: a soft halo hugging the base of every
//     building, unit and tree, from the sprite's silhouette restricted to its collision
//     footprint (analysis.hpp, OcclusionKernel);
//  2. dynamic light after the whole world is drawn (before menus, texts, status bar and
//     cursor): every drawn shot/explosion/flash/light-spot frame that emits light
//     (analysis.hpp: classes + act_fight/act_death chains + per-frame pixel emission) lights
//     terrain and objects around it with its own colour, attenuated with distance.
//
// Both accumulate on a low-resolution grid aligned with the framebuffer (a few world pixels
// per cell, so the grid size does not depend on the internal resolution) and are applied with
// bilinear upsampling only on the rows/columns the grid touches: no sources, no cost.
//
// The hook (called by GameEngine::draw, see the Enhancer comment): the framebuffer, a View
// (world -> framebuffer transform) and the drawn sprites as DrawItem (world position,
// interpolated when the pipeline has it). Nothing here reads or writes simulation state; the
// caches are derived data and are not part of save states.
//
// Header-only; depends on core/, libgromada/ (formats, framebuffer) and analysis.hpp.

#include <bit>
#include <chrono>
#include <cstring>
#include <functional>
#include <unordered_map>
#include "core/std.hpp"
#include "game/enhance/analysis.hpp"
#include "libgromada/software_renderer.hpp"

namespace gromada::game::enhance {

// ---------------------------------------------------------------------------- settings
// Levels: 0 = off (default), 1 = soft, 2 = normal, 3 = strong.
struct Settings {
    int lights = 0;           // dynamic light around shots, explosions, flashes
    int contactShadows = 0;   // contact shadows / approximate ambient occlusion
    [[nodiscard]] bool any() const noexcept { return lights > 0 || contactShadows > 0; }
    bool operator==(const Settings&) const = default;
};

// Core options (key, "Description; default|values..."), libretro retro_variable layout.
struct CoreOption {
    const char* key;
    const char* value;
};
inline constexpr CoreOption coreOptions[] = {
    {"gromada_fx_lights", "Luci dinamiche (spari, esplosioni); disattivate|tenui|normali|intense"},
    {"gromada_fx_contact_shadows", "Ombre di contatto (occlusione ambientale); disattivate|tenui|normali|marcate"},
};

// Reads the settings with the core's getVar(key) -> const char* (nullptr: default).
template <class GetVar>
[[nodiscard]] Settings readSettings(GetVar&& getVar) {
    const auto level = [&](const char* key) {
        const char* v = getVar(key);
        if (!v) return 0;
        static constexpr const char* names[][4] = {{"disattivate", "tenui", "normali", "intense"},
                                                   {"disattivate", "tenui", "normali", "marcate"}};
        for (const auto& row : names)
            for (int i = 0; i < 4; ++i)
                if (std::strcmp(v, row[i]) == 0) return i;
        return 0;
    };
    Settings s;
    s.lights = level("gromada_fx_lights");
    s.contactShadows = level("gromada_fx_contact_shadows");
    return s;
}

// ---------------------------------------------------------------------------- hook data
// World -> framebuffer: fb = (world - origin) * scale. Today origin = camera, scale = 1; with
// the pipeline of #20, origin = interpolated camera and scale = internal resolution x zoom.
struct View {
    float originX = 0, originY = 0;
    float scale = 1;
};

// One sprite drawn by the world pass (sub-objects of a chain are items too). Positions are in
// world pixels: (x, y) is the ground point, the body is drawn z pixels higher unless zeroZ.
// The frame is drawn with its centre at (x, y - z), like putcadr.
struct DrawItem {
    const VidGraphics::Frame* frame = nullptr;
    float x = 0, y = 0, z = 0;
    std::uint16_t nvid = 0;
    std::uint8_t cls = 0, category = 0;
    std::int16_t sizeX = 0, sizeY = 0, sizeZ = 0;
    bool zeroZ = false;
};

// Per-frame cost and counts (tools, tests).
struct Stats {
    double groundMs = 0, lightMs = 0;             // whole passes
    double groundApplyMs = 0, lightApplyMs = 0;   // of which: upsampling onto the framebuffer
    int occluders = 0, lights = 0;
    long groundPixels = 0, lightPixels = 0;       // framebuffer pixels visited by the upsampling
};

// ---------------------------------------------------------------------------- tuning
// Light response, per channel, as an exposure curve: the light brings the extra energy
// e = l * (mul*d + 255*add) (l = accumulated light of the channel, d = the pixel: lit surfaces
// reflect in proportion to their colour, plus a small coloured glow on dark ground), and the
// pixel moves towards white by out = d + (255 - d) * (1 - exp(-e/255)). Dark and mid tones
// gain the most, bright pixels (the emitter itself, explosions) roll off instead of clipping.
struct LightLevel {
    float mul, add, gain;
};
inline constexpr LightLevel LightLevels[4] = {{0, 0, 0}, {1.2f, 0.10f, 0.7f}, {1.6f, 0.14f, 1.0f}, {1.8f, 0.16f, 1.2f}};
inline constexpr int ExposureSteps = 1024;   // LUT over e = 0 .. 4*255
[[nodiscard]] inline const std::array<std::uint16_t, ExposureSteps>& exposureLut() {
    static const auto lut = [] {
        std::array<std::uint16_t, ExposureSteps> t{};
        for (int i = 0; i < ExposureSteps; ++i)
            t[static_cast<std::size_t>(i)] = static_cast<std::uint16_t>(std::lround((1.0 - std::exp(-i * 4.0 / ExposureSteps)) * 256.0));
        return t;
    }();
    return lut;
}
inline constexpr float ShadowLevels[4] = {0, 0.32f, 0.46f, 0.62f};
inline constexpr float MinLightPower = 6.0f;      // emitted px below which a frame gives no light
inline constexpr float LightCell = 4.0f;          // world px per light-grid cell
inline constexpr float ShadowBoost = 1.7f;        // kernel coverage -> occlusion (edge ~ full strength)
inline constexpr float ShadowFadeZ = 24.0f;       // occluders this high above ground cast no contact shadow
inline constexpr float LightKnee = 0.35f;
// Contact-shadow kernels are built on first sight (~50 us each on desktop): at most this many
// per frame, so the first frame of a level does not stall (the rest appear over the next
// frames); the cache is dropped beyond MaxKernelBytes (all the occluders of level 19: ~7 MB).
inline constexpr int MaxKernelBuildsPerFrame = 24;
inline constexpr std::size_t MaxKernelBytes = 32u << 20;         // accumulated light l -> l / (1 + knee*l): many lights do not burn out

// Light radius (world px) and intensity (0..1) from the emitted power of a frame and the
// spread of its emitting pixels (a large light sprite of the original keeps its extent).
[[nodiscard]] inline float lightRadius(float power, float spread) noexcept {
    return std::clamp(std::max(14.0f + 2.6f * std::sqrt(power), 1.5f * spread), 20.0f, 150.0f);
}
[[nodiscard]] inline float lightIntensity(float power) noexcept { return std::clamp(0.2f + std::sqrt(power) / 45.0f, 0.0f, 1.0f); }

// ---------------------------------------------------------------------------- grid
// A low-resolution accumulation grid aligned with the framebuffer: cell (i, j) covers
// framebuffer pixels [i*cell, (i+1)*cell) x [j*cell, (j+1)*cell); values are sampled at cell
// centres and upsampled bilinearly. Sources accumulate in float (at, touch); finalize() turns
// the touched cells into 8.8 fixed-point values (with a tone curve) that forEachRun upsamples.
// Touched cells are flagged, so the upsampling visits only the pixels next to them and the
// next reset clears only those.
template <int Channels>
class Grid {
public:
    // `neutral`: the fixed-point value of an untouched cell (what finalize(0) gives).
    void reset(int fbW, int fbH, float cell, std::int32_t neutral) {
        const int w = static_cast<int>(std::ceil(static_cast<float>(fbW) / cell)) + 1;
        const int h = static_cast<int>(std::ceil(static_cast<float>(fbH) / cell)) + 1;
        if (cell != m_cell) {   // analog zoom changes it every frame: only the x table depends on it
            m_cell = cell;
            m_tableW = -1;
        }
        if (w != m_w || h != m_h || neutral != m_neutral) {
            m_w = w;
            m_h = h;
            m_neutral = neutral;
            m_v.assign(static_cast<std::size_t>(w * h * Channels), 0.0f);
            m_iv.assign(static_cast<std::size_t>(w * h * Channels), neutral);
            m_act.assign(static_cast<std::size_t>(w * h), 0);
            m_min.assign(static_cast<std::size_t>(h), w);
            m_max.assign(static_cast<std::size_t>(h), -1);
            m_tableW = -1;
        } else if (m_any) {
            for (int j = 0; j < h; ++j) {   // clear only what the last frame touched
                const auto r = static_cast<std::size_t>(j);
                if (m_max[r] >= m_min[r]) {
                    const auto a = idx(m_min[r], j), b = idx(m_max[r], j) + Channels;
                    std::fill(m_v.begin() + a, m_v.begin() + b, 0.0f);
                    std::fill(m_iv.begin() + a, m_iv.begin() + b, neutral);
                    std::fill(&m_act[r * static_cast<std::size_t>(w) + static_cast<std::size_t>(m_min[r])],
                              &m_act[r * static_cast<std::size_t>(w) + static_cast<std::size_t>(m_max[r])] + 1, std::uint8_t{0});
                }
                m_min[r] = w;
                m_max[r] = -1;
            }
        }
        m_any = false;
    }
    [[nodiscard]] int width() const noexcept { return m_w; }
    [[nodiscard]] int height() const noexcept { return m_h; }
    [[nodiscard]] float cell() const noexcept { return m_cell; }
    [[nodiscard]] bool any() const noexcept { return m_any; }
    [[nodiscard]] float* at(int i, int j) noexcept { return &m_v[idx(i, j)]; }
    void touch(int i0, int i1, int j) noexcept {
        const auto r = static_cast<std::size_t>(j);
        m_min[r] = std::min(m_min[r], i0);
        m_max[r] = std::max(m_max[r], i1);
        std::fill(&m_act[r * static_cast<std::size_t>(m_w) + static_cast<std::size_t>(i0)],
                  &m_act[r * static_cast<std::size_t>(m_w) + static_cast<std::size_t>(i1)] + 1, std::uint8_t{1});
        m_any = true;
    }

    // Fixed-point values of the touched rows: iv = f(v) (8.8), f(0) must be the neutral value.
    template <class F>
    void finalize(F&& f) {
        for (int j = 0; j < m_h; ++j) {
            const auto r = static_cast<std::size_t>(j);
            if (m_max[r] < m_min[r]) continue;
            for (std::size_t k = idx(m_min[r], j), e = idx(m_max[r], j) + Channels; k < e; ++k) m_iv[k] = f(m_v[k]);
        }
    }

    // Upsamples the finalized grid bilinearly onto the framebuffer: calls f(y, xa, xb, v) for
    // every run of pixels [xa, xb) of row y whose sample involves a touched cell, with
    // v[x * Channels + c] the 8.8 value at pixel x (indexed by the absolute x).
    template <class F>
    void forEachRun(int fbW, int fbH, F&& f) {
        if (!m_any) return;
        if (m_tableW != fbW) buildTable(fbW);
        m_row.resize(static_cast<std::size_t>(m_w * Channels));
        m_px.resize(static_cast<std::size_t>(fbW * Channels));
        for (int y = 0; y < fbH; ++y) {
            const float gy = (static_cast<float>(y) + 0.5f) / m_cell - 0.5f;
            int j0 = static_cast<int>(std::floor(gy));
            std::int32_t t = static_cast<std::int32_t>((gy - static_cast<float>(j0)) * 256.0f);
            if (j0 < 0) { j0 = 0; t = 0; }
            const int j1 = std::min(j0 + 1, m_h - 1);
            const auto r0 = static_cast<std::size_t>(j0), r1 = static_cast<std::size_t>(j1);
            const int lo = std::min(m_min[r0], m_min[r1]), hi = std::max(m_max[r0], m_max[r1]);
            if (hi < lo) continue;
            // pixel spans of cells [c0, c1] sample cells c and c+1 (clamped to c1)
            const int c0 = std::max(lo - 1, 0), c1 = std::min(hi, m_w - 1);
            const std::uint8_t* act0 = &m_act[r0 * static_cast<std::size_t>(m_w)];
            const std::uint8_t* act1 = &m_act[r1 * static_cast<std::size_t>(m_w)];
            const std::int32_t* top = &m_iv[idx(0, j0)];
            const std::int32_t* bot = &m_iv[idx(0, j1)];
            for (int i = c0; i <= c1;) {
                const auto active = [&](int k) {
                    const int kn = std::min(k + 1, m_w - 1);
                    return (act0[k] | act0[kn] | act1[k] | act1[kn]) != 0;
                };
                if (!active(i)) { ++i; continue; }
                const int first = i;
                while (i <= c1 && active(i)) ++i;   // run of pixel spans of cells [first, i)
                const int last = std::min(i, m_w - 1);
                for (int k = first * Channels, e = (last + 1) * Channels; k < e; ++k)
                    m_row[static_cast<std::size_t>(k)] = top[k] + (((bot[k] - top[k]) * t) >> 8);
                const int xa = m_start[static_cast<std::size_t>(first)], xb = m_start[static_cast<std::size_t>(i)];
                for (int k = first; k < i; ++k) {
                    const std::int32_t* ra = &m_row[static_cast<std::size_t>(k * Channels)];
                    const std::int32_t* rb = &m_row[static_cast<std::size_t>(std::min(k + 1, m_w - 1) * Channels)];
                    for (int x = m_start[static_cast<std::size_t>(k)], xe = m_start[static_cast<std::size_t>(k) + 1]; x < xe; ++x) {
                        const std::int32_t s = m_xs[static_cast<std::size_t>(x)];
                        for (int c = 0; c < Channels; ++c)
                            m_px[static_cast<std::size_t>(x * Channels + c)] = ra[c] + (((rb[c] - ra[c]) * s) >> 8);
                    }
                }
                if (xa < xb) f(y, xa, xb, m_px.data());
            }
        }
    }

private:
    [[nodiscard]] std::size_t idx(int i, int j) const noexcept { return static_cast<std::size_t>((j * m_w + i) * Channels); }
    // Per pixel x: the 8-bit weight of the right cell of its bilinear pair; m_start[i] is the
    // first pixel whose left cell is i.
    void buildTable(int fbW) {
        m_xs.assign(static_cast<std::size_t>(fbW), 0);
        m_start.assign(static_cast<std::size_t>(m_w) + 1, fbW);
        int prev = -1;
        for (int x = 0; x < fbW; ++x) {
            const float gx = (static_cast<float>(x) + 0.5f) / m_cell - 0.5f;
            const int i0 = std::clamp(static_cast<int>(std::floor(gx)), 0, m_w - 1);
            m_xs[static_cast<std::size_t>(x)] = gx < 0 ? 0 : static_cast<std::int32_t>((gx - static_cast<float>(i0)) * 256.0f);
            for (int i = prev + 1; i <= i0; ++i) m_start[static_cast<std::size_t>(i)] = x;
            prev = std::max(prev, i0);
        }
        m_tableW = fbW;
    }
    int m_w = 0, m_h = 0, m_tableW = -1;
    float m_cell = 0;
    std::int32_t m_neutral = 0;
    bool m_any = false;
    std::vector<float> m_v;
    std::vector<std::int32_t> m_iv;
    std::vector<std::uint8_t> m_act;
    std::vector<int> m_min, m_max, m_start;
    std::vector<std::int32_t> m_xs, m_row, m_px;
};

// ---------------------------------------------------------------------------- enhancer
// Owned by the engine (built on first use, only when an effect is on). GameEngine::draw:
//   enh->begin()                         before the first layer (clears the item lists)
//   enh->drawn.push_back(item)           for every sprite it draws (chains included)
//   enh->ground(fb, view, occluders)     after layer 4, with the layer-5 sprites about to be
//                                        drawn (the sorted unit layer: buildings, units, trees)
//   enh->finish(fb, view)                after the last layer, before menus/texts/cursor
class Enhancer {
public:
    explicit Enhancer(std::span<const Vid* const> vids) : m_emitter(classifyEmitters(vids)) {}

    void setSettings(const Settings& s) noexcept { m_settings = s; }
    [[nodiscard]] const Settings& settings() const noexcept { return m_settings; }
    [[nodiscard]] const Stats& stats() const noexcept { return m_stats; }
    [[nodiscard]] bool isEmitter(int nvid) const noexcept {
        return nvid >= 0 && static_cast<std::size_t>(nvid) < m_emitter.size() && m_emitter[static_cast<std::size_t>(nvid)];
    }

    std::vector<DrawItem> drawn;       // filled by the world pass
    std::vector<DrawItem> occluders;   // scratch for the caller of ground()

    void begin() {
        drawn.clear();
        occluders.clear();
        m_stats = {};
    }

    // Tools only (engine_run --fx-scale): called with the ground picture before the shadows.
    std::function<void(FramebufferRef)> groundObserver;

    // Contact shadows on the ground layers (hook 1).
    void ground(FramebufferRef fb, const View& view, std::span<const DrawItem> occluders) {
        if (groundObserver) groundObserver(fb);
        const float strength = ShadowLevels[std::clamp(m_settings.contactShadows, 0, 3)];
        if (strength <= 0) return;
        const auto t0 = std::chrono::steady_clock::now();
        m_stats.groundPixels = 0;
        const int fbW = fb.extent(1), fbH = fb.extent(0);
        const float cell = OcclusionKernel::Cell * view.scale;
        m_shadow.reset(fbW, fbH, cell, 256);
        int count = 0, builds = 0;
        for (const DrawItem& it : occluders) {
            if (!it.frame || !(it.category & (vcat::Object | vcat::Vehicle)) || it.sizeZ < 4) continue;
            const float wz = it.zeroZ ? 1.0f : std::clamp(1.0f - it.z / ShadowFadeZ, 0.0f, 1.0f);
            if (wz <= 0) continue;
            const OcclusionKernel* k = kernel(it, builds);
            if (!k || k->empty()) continue;
            splat(*k, it, view, wz);
            ++count;
        }
        const auto ta = std::chrono::steady_clock::now();
        if (m_shadow.any()) {
            // finalized value = the multiplier kept by the pixel, 256 = untouched
            m_shadow.finalize([&](float occ) {
                return static_cast<std::int32_t>(256.0f - std::min(1.0f, occ * ShadowBoost) * strength * 256.0f + 0.5f);
            });
            m_shadow.forEachRun(fbW, fbH, [&](int y, int xa, int xb, const std::int32_t* keep) {
                m_stats.groundPixels += xb - xa;
                RGBA8* row = &fb[y, 0];
                for (int x = xa; x < xb; ++x) {
                    // the three channels in two multiplies (r and b share one, alpha kept)
                    const auto p = std::bit_cast<std::uint32_t>(row[x]);
                    const auto k = static_cast<std::uint32_t>(keep[x]);
                    const std::uint32_t rb = (((p & 0x00ff00ffu) * k) >> 8) & 0x00ff00ffu;
                    const std::uint32_t g = (((p & 0x0000ff00u) * k) >> 8) & 0x0000ff00u;
                    row[x] = std::bit_cast<RGBA8>(rb | g | (p & 0xff000000u));
                }
            });
        }
        m_stats.occluders = count;
        const auto t1 = std::chrono::steady_clock::now();
        m_stats.groundMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        m_stats.groundApplyMs = std::chrono::duration<double, std::milli>(t1 - ta).count();
    }

    // Dynamic light from the drawn items (hook 2).
    void finish(FramebufferRef fb, const View& view) { lights(fb, view, drawn); }

    void lights(FramebufferRef fb, const View& view, std::span<const DrawItem> items) {
        const LightLevel lv = LightLevels[std::clamp(m_settings.lights, 0, 3)];
        if (lv.gain <= 0) return;
        const auto t0 = std::chrono::steady_clock::now();
        m_stats.lightPixels = 0;
        const int fbW = fb.extent(1), fbH = fb.extent(0);
        bool started = false;
        int count = 0;
        for (const DrawItem& it : items) {
            if (!it.frame || !isEmitter(it.nvid)) continue;
            const Emission& e = emission(it);
            if (e.power < MinLightPower) continue;
            if (!started) {
                m_light.reset(fbW, fbH, LightCell * view.scale, 0);
                started = true;
            }
            const float bodyZ = it.zeroZ ? 0.0f : it.z;
            const float wx = it.x - static_cast<float>(it.frame->width() / 2) + e.cx;
            const float wy = it.y - static_cast<float>(it.frame->height() / 2) - bodyZ + e.cy + bodyZ * 0.5f;
            count += addLight((wx - view.originX) * view.scale, (wy - view.originY) * view.scale,
                              lightRadius(e.power, e.spread) * view.scale, lightIntensity(e.power) * lv.gain, e);
        }
        const auto ta = std::chrono::steady_clock::now();
        if (started && m_light.any()) {
            m_light.finalize([](float l) { return static_cast<std::int32_t>(std::min(4.0f, l / (1.0f + LightKnee * l)) * 256.0f + 0.5f); });
            const auto mul = static_cast<std::int32_t>(lv.mul * 256.0f), add = static_cast<std::int32_t>(lv.add * 256.0f);
            const auto& lut = exposureLut();
            m_light.forEachRun(fbW, fbH, [&](int y, int xa, int xb, const std::int32_t* v) {
                m_stats.lightPixels += xb - xa;
                RGBA8* row = &fb[y, 0];
                const auto ch = [&](std::int32_t d, std::int32_t l) {
                    // e = (d*mul + 255*add) * l in 8-bit units; one LUT step = 255*4/1024 ~ 1 unit
                    const std::int32_t e = std::min<std::int32_t>(ExposureSteps - 1, ((d * mul + 255 * add) * l) >> 16);
                    return static_cast<std::uint8_t>(d + (((255 - d) * lut[static_cast<std::size_t>(e)]) >> 8));
                };
                for (int x = xa; x < xb; ++x) {
                    RGBA8& p = row[x];
                    const std::int32_t* l = &v[x * 3];
                    p.r = ch(p.r, l[0]);
                    p.g = ch(p.g, l[1]);
                    p.b = ch(p.b, l[2]);
                }
            });
        }
        m_stats.lights = count;
        const auto t1 = std::chrono::steady_clock::now();
        m_stats.lightMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        m_stats.lightApplyMs = std::chrono::duration<double, std::milli>(t1 - ta).count();
    }

private:
    const Emission& emission(const DrawItem& it) {
        const auto key = reinterpret_cast<std::uintptr_t>(it.frame->data.data());
        auto f = m_emission.find(key);
        if (f == m_emission.end()) f = m_emission.emplace(key, frameEmission(*it.frame, it.cls)).first;
        return f->second;
    }
    // The kernel of an occluder frame, or nullptr when it is not built yet and this frame's
    // build budget is spent.
    const OcclusionKernel* kernel(const DrawItem& it, int& builds) {
        const auto key = reinterpret_cast<std::uintptr_t>(it.frame->data.data()) * 31u ^
                         (static_cast<std::uintptr_t>(static_cast<std::uint16_t>(it.sizeY)) << 1) ^
                         (static_cast<std::uintptr_t>(static_cast<std::uint16_t>(it.sizeZ)) << 17);
        auto f = m_kernels.find(key);
        if (f == m_kernels.end()) {
            if (builds >= MaxKernelBuildsPerFrame) return nullptr;
            ++builds;
            if (m_kernelBytes > MaxKernelBytes) {
                m_kernels.clear();
                m_kernelBytes = 0;
            }
            f = m_kernels.emplace(key, occlusionKernel(*it.frame, it.sizeX, it.sizeY, it.sizeZ)).first;
            m_kernelBytes += f->second.v.size() * sizeof(float) + sizeof(OcclusionKernel) + 32;
        }
        return &f->second;
    }

    // Gathers the kernel into the shadow grid with a bilinear shift (sub-cell positions). The
    // kernel has a zero border, so grid cell i = i0 + u (u = 1 .. w-1) reads K[u] and K[u-1].
    void splat(const OcclusionKernel& k, const DrawItem& it, const View& view, float weight) {
        const float cell = m_shadow.cell();
        const float gx = ((it.x + k.ox - view.originX) * view.scale) / cell;   // grid coordinate of kernel cell 0
        const float gy = ((it.y + k.oy - view.originY) * view.scale) / cell;
        const int i0 = static_cast<int>(std::floor(gx)), j0 = static_cast<int>(std::floor(gy));
        const float fx = gx - static_cast<float>(i0), fy = gy - static_cast<float>(j0);
        const float w00 = (1 - fx) * (1 - fy) * weight, w10 = fx * (1 - fy) * weight, w01 = (1 - fx) * fy * weight,
                    w11 = fx * fy * weight;
        const int ua = std::max(1, -i0), ub = std::min(k.w - 1, m_shadow.width() - 1 - i0);
        const int va = std::max(1, -j0), vb = std::min(k.h - 1, m_shadow.height() - 1 - j0);
        if (ua > ub || va > vb) return;
        for (int v = va; v <= vb; ++v) {
            const float* kr = &k.v[static_cast<std::size_t>(v * k.w)];
            const float* kp = &k.v[static_cast<std::size_t>((v - 1) * k.w)];
            float* row = m_shadow.at(0, j0 + v);   // grid cell i0 + u, >= 0 for u >= ua
            for (int u = ua; u <= ub; ++u) row[i0 + u] += w00 * kr[u] + w10 * kr[u - 1] + w01 * kp[u] + w11 * kp[u - 1];
            m_shadow.touch(i0 + ua, i0 + ub, j0 + v);
        }
    }

    // Smooth compact falloff (1 - d^2/R^2)^2, evaluated at the light-grid cell centres.
    // Returns whether the light reaches the framebuffer.
    bool addLight(float lx, float ly, float radius, float intensity, const Emission& e) {
        const float cell = m_light.cell();
        const float cx = lx / cell - 0.5f, cy = ly / cell - 0.5f, rc = radius / cell;
        const int ia = std::max(0, static_cast<int>(std::floor(cx - rc))), ib = std::min(m_light.width() - 1, static_cast<int>(std::ceil(cx + rc)));
        const int ja = std::max(0, static_cast<int>(std::floor(cy - rc))), jb = std::min(m_light.height() - 1, static_cast<int>(std::ceil(cy + rc)));
        if (ia > ib || ja > jb) return false;
        const float inv = 1.0f / (rc * rc);
        for (int j = ja; j <= jb; ++j) {
            const float dy = static_cast<float>(j) - cy;
            int lo = ib + 1, hi = ia - 1;
            for (int i = ia; i <= ib; ++i) {
                const float dx = static_cast<float>(i) - cx;
                const float q = (dx * dx + dy * dy) * inv;
                if (q >= 1) continue;
                const float f = (1 - q) * (1 - q) * intensity;
                float* c = m_light.at(i, j);
                c[0] += f * e.r;
                c[1] += f * e.g;
                c[2] += f * e.b;
                lo = std::min(lo, i);
                hi = std::max(hi, i);
            }
            if (hi >= lo) m_light.touch(lo, hi, j);
        }
        return true;
    }

    Settings m_settings;
    Stats m_stats;
    std::vector<std::uint8_t> m_emitter;
    std::unordered_map<std::uintptr_t, Emission> m_emission;
    std::unordered_map<std::uintptr_t, OcclusionKernel> m_kernels;
    std::size_t m_kernelBytes = 0;
    Grid<1> m_shadow;
    Grid<3> m_light;
};

} // namespace gromada::game::enhance
