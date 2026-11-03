#pragma once
// Output resolution (docs/rendering.md, "Risoluzione di uscita"): the picture drawn at the
// internal resolution (k x the logical size) is handed to the frontend at another multiple m
// of the logical size, e.g. 640 x 480 for the frontend's shaders.
//
// - m < k, `average`: every output pixel is the mean of the internal pixels it covers
//   (area weights, exact integer arithmetic: an output pixel spans k/m internal pixels per axis,
//   partial ones weighted by their overlap; rounding to nearest).
// - otherwise (m > k, or pixels kept as they are): the internal pixel at the centre of the
//   output pixel (nearest neighbour).
//
// Header-only.

#include "core/std.hpp"

#include <functional>

namespace gromada::game::view {

class OutputScaler {
public:
    using Parallel = std::function<void(int n, const std::function<void(int)>& fn)>;

    // src: (lw k) x (lh k) XRGB8888, pitch srcPitch pixels; dst: (lw m) x (lh m), packed.
    void resample(const std::uint32_t* src, std::size_t srcPitch, int lw, int lh, int k, std::uint32_t* dst, int m, bool average,
                  int bands = 1, const Parallel& parallel = {}) {
        const int sw = lw * k, sh = lh * k, dw = lw * m, dh = lh * m;
        const bool box = average && m < k;
        taps(m_x, sw, dw, k, m, box);
        taps(m_y, sh, dh, k, m, box);
        const auto job = [&](int b) {
            const int y0 = static_cast<int>(static_cast<std::int64_t>(dh) * b / bands);
            const int y1 = static_cast<int>(static_cast<std::int64_t>(dh) * (b + 1) / bands);
            for (int y = y0; y < y1; ++y) {
                std::uint32_t* out = dst + static_cast<std::size_t>(y) * static_cast<std::size_t>(dw);
                const Axis& ty = m_y[static_cast<std::size_t>(y)];
                if (!box) {
                    const std::uint32_t* row = src + static_cast<std::size_t>(ty.first) * srcPitch;
                    for (int x = 0; x < dw; ++x) out[x] = row[m_x[static_cast<std::size_t>(x)].first];
                    continue;
                }
                for (int x = 0; x < dw; ++x) {
                    const Axis& tx = m_x[static_cast<std::size_t>(x)];
                    std::uint32_t r = 0, g = 0, bl = 0;
                    for (int j = 0; j < ty.count; ++j) {
                        const std::uint32_t* row = src + static_cast<std::size_t>(ty.first + j) * srcPitch + tx.first;
                        const std::uint32_t wy = ty.weight[j];
                        for (int i = 0; i < tx.count; ++i) {
                            const std::uint32_t w = wy * tx.weight[i], p = row[i];
                            r += w * ((p >> 16) & 0xFF);
                            g += w * ((p >> 8) & 0xFF);
                            bl += w * (p & 0xFF);
                        }
                    }
                    const std::uint32_t total = static_cast<std::uint32_t>(k * k), half = total / 2;
                    out[x] = ((r + half) / total) << 16 | ((g + half) / total) << 8 | ((bl + half) / total);
                }
            }
        };
        bands = std::clamp(bands, 1, std::max(1, dh));
        if (bands > 1 && parallel) parallel(bands, job);
        else for (int b = 0; b < bands; ++b) job(b);
    }

private:
    // One output pixel along an axis: the first internal pixel and, for the mean, the weights
    // of the ones it covers (sum k).
    struct Axis {
        int first = 0, count = 1;
        std::array<std::uint8_t, 6> weight{};
    };
    static void taps(std::vector<Axis>& out, int sn, int dn, int k, int m, bool box) {
        out.resize(static_cast<std::size_t>(dn));
        for (int i = 0; i < dn; ++i) {
            Axis& a = out[static_cast<std::size_t>(i)];
            if (!box) {   // the centre (2i + 1) / 2m in output units = (2i + 1) k / 2m internal pixels
                a.first = std::min(sn - 1, static_cast<int>((static_cast<std::int64_t>(2 * i + 1) * k) / (2 * m)));
                a.count = 1;
                continue;
            }
            // output pixel i covers [i k, (i + 1) k) in units of 1/m internal pixel
            const std::int64_t lo = static_cast<std::int64_t>(i) * k, hi = lo + k;
            a.first = static_cast<int>(lo / m);
            a.count = 0;
            for (std::int64_t j = a.first; j * m < hi && j < sn; ++j) {
                const std::int64_t w = std::min(hi, (j + 1) * m) - std::max(lo, j * m);
                a.weight[static_cast<std::size_t>(a.count++)] = static_cast<std::uint8_t>(w);
            }
        }
    }

    std::vector<Axis> m_x, m_y;
};

} // namespace gromada::game::view
