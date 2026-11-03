#pragma once
// Performance statistics on screen (core option "Statistiche prestazioni"): what the core
// costs and how regularly the frontend runs it, to tell a slow core from an irregular frontend.
// Every second:
//   CORE  mean / max time of retro_run (simulation + picture + audio), ms
//   FRAME mean / max time between two retro_run calls (the frontend's pace), ms
//   LATE  calls that came more than 1.5 frame periods after the previous one
//   OVER  frames whose core time exceeded the frame period
// Drawn on the output picture (XRGB8888) with a 5 x 7 font, scaled with the picture.
//
// Header-only.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace gromada {

class PerfOverlay {
public:
    using Clock = std::chrono::steady_clock;

    void beginFrame() {
        const auto now = Clock::now();
        if (m_haveLast) {
            const double gap = ms(now - m_lastStart);
            m_gapSum += gap;
            m_gapMax = std::max(m_gapMax, gap);
            ++m_gaps;
            if (m_periodMs > 0 && gap > 1.5 * m_periodMs) ++m_late;
        }
        m_lastStart = now;
        m_haveLast = true;
    }
    // After the frame was computed, before it is handed over; fps: the core's frame rate.
    void endFrame(int fps) {
        m_periodMs = fps > 0 ? 1000.0 / fps : 0;
        const double t = ms(Clock::now() - m_lastStart);
        m_coreSum += t;
        m_coreMax = std::max(m_coreMax, t);
        if (m_periodMs > 0 && t > m_periodMs) ++m_over;
        if (++m_frames >= std::max(1, fps)) {   // about once a second
            char buf[4][48];
            std::snprintf(buf[0], sizeof buf[0], "CORE %.2f MAX %.2f MS", m_coreSum / m_frames, m_coreMax);
            std::snprintf(buf[1], sizeof buf[1], "FRAME %.2f MAX %.2f MS", m_gaps ? m_gapSum / m_gaps : 0.0, m_gapMax);
            std::snprintf(buf[2], sizeof buf[2], "LATE %d OVER %d", m_late, m_over);
            std::snprintf(buf[3], sizeof buf[3], "%d FPS", fps);
            for (int i = 0; i < 4; ++i) m_lines[static_cast<std::size_t>(i)] = buf[i];
            m_frames = m_gaps = m_late = m_over = 0;
            m_coreSum = m_coreMax = m_gapSum = m_gapMax = 0;
        }
    }

    // The text at the top-left corner of the picture; `scale` = output pixels per logical pixel.
    void draw(std::uint32_t* fb, int width, int height, std::size_t pitchPixels, int scale) const {
        const int s = std::max(1, scale);
        int y = 4 * s;
        for (const std::string& line : m_lines) {
            if (line.empty()) continue;
            const int w = static_cast<int>(line.size()) * 6 * s + 4 * s, h = 9 * s;
            for (int yy = std::max(0, y - 2 * s); yy < std::min(height, y - 2 * s + h); ++yy)
                for (int xx = 2 * s; xx < std::min(width, 2 * s + w); ++xx) {
                    std::uint32_t& p = fb[static_cast<std::size_t>(yy) * pitchPixels + static_cast<std::size_t>(xx)];
                    p = (p >> 2) & 0x3F3F3Fu;   // darkened background
                }
            int x = 4 * s;
            for (const char c : line) {
                const auto& g = glyph(c);
                for (int gy = 0; gy < 7; ++gy)
                    for (int gx = 0; gx < 5; ++gx)
                        if (g[static_cast<std::size_t>(gy)] & (0x10 >> gx))
                            for (int py = 0; py < s; ++py)
                                for (int px = 0; px < s; ++px) {
                                    const int X = x + gx * s + px, Y = y + gy * s + py;
                                    if (X >= 0 && X < width && Y >= 0 && Y < height)
                                        fb[static_cast<std::size_t>(Y) * pitchPixels + static_cast<std::size_t>(X)] = 0xFFFF40u;
                                }
                x += 6 * s;
            }
            y += h + s;
        }
    }

private:
    static double ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }
    // 5 x 7 glyphs, one byte per row (bit 4 = left column).
    static const std::array<std::uint8_t, 7>& glyph(char c) {
        static constexpr std::string_view chars = "0123456789.ACEFLMOPRSTVX ";
        static constexpr std::array<std::array<std::uint8_t, 7>, 25> g{{
            {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
            {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
            {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
            {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
            {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
            {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C},                                              // .
            {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},  // A C
            {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},  // E F
            {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}, {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11},  // L M
            {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},  // O P
            {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}, {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E},  // R S
            {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04},  // T V
            {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},  // X space
        }};
        const auto i = chars.find(c);
        return g[i == std::string_view::npos ? chars.size() - 1 : i];
    }

    Clock::time_point m_lastStart{};
    bool m_haveLast = false;
    double m_periodMs = 0;
    int m_frames = 0, m_gaps = 0, m_late = 0, m_over = 0;
    double m_coreSum = 0, m_coreMax = 0, m_gapSum = 0, m_gapMax = 0;
    std::array<std::string, 4> m_lines{};
};

} // namespace gromada
