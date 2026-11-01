#pragma once
// Movie player for the game's AVI files: demuxing (avi.hpp), video decoding (MS Video 1 or
// Indeo 3), conversion to XRGB8888, a clock independent of the output frame rate and the
// soundtrack as a gromada::AudioStream for the mixer.
//
// Timing: the frontend calls step(dt) once per output frame (60 or 120 fps). step() first
// shows the frame due at the current time t (frame floor(t * fps_movie)), then advances t;
// the audio mixed during the same output frame covers [t, t + dt). So the picture is the
// one whose interval contains the audio being played: audio and video can never be more
// than one movie frame apart, at any output rate. The movie is finished once t reaches
// frames / fps_movie.

#include "engine/audio_stream.hpp"
#include "video/avi.hpp"
#include "video/indeo3.hpp"
#include "video/msvideo1.hpp"

#include <array>
#include <cmath>
#include <memory>

namespace gromada::video {

// --- colour conversion ------------------------------------------------------------------

// RGB555 -> XRGB8888, replicating the high bits into the low ones (31 -> 255).
inline void rgb555ToXrgb(std::span<const std::uint16_t> src, std::uint32_t* dst) {
    for (std::size_t i = 0; i < src.size(); ++i) {
        const std::uint32_t v = src[i];
        const std::uint32_t r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
        dst[i] = (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
    }
}

inline void paletteToXrgb(std::span<const std::uint8_t> src, const std::vector<std::uint32_t>& palette, std::uint32_t* dst) {
    for (std::size_t i = 0; i < src.size(); ++i)
        dst[i] = src[i] < palette.size() ? palette[src[i]] : 0;
}

// YUV 4:1:0 with 7-bit samples (Indeo 3) -> XRGB8888. Samples are scaled to 8 bits (x2),
// then converted with BT.601 limited-range coefficients (what FFmpeg uses for these
// files); chroma is upsampled bilinearly (chroma samples sit at the centre of their 4x4
// block, so the weights are 5/8, 7/8, 1/8, 3/8).
class YuvToRgb {
public:
    YuvToRgb() {
        constexpr double Scale = 1 << 16;
        for (int v = 0; v < 256; ++v) {
            m_y[v] = static_cast<int>(std::lround(1.164383 * (v - 16) * Scale));
            m_rv[v] = static_cast<int>(std::lround(1.596027 * (v - 128) * Scale));
            m_gu[v] = static_cast<int>(std::lround(-0.391762 * (v - 128) * Scale));
            m_gv[v] = static_cast<int>(std::lround(-0.812968 * (v - 128) * Scale));
            m_bu[v] = static_cast<int>(std::lround(2.017232 * (v - 128) * Scale));
        }
    }

    void convert(const Indeo3Decoder& dec, std::uint32_t* dst) {
        const auto Y = dec.plane(0), U = dec.plane(1), V = dec.plane(2);
        const int w = dec.width(), h = dec.height(), cw = U.width, ch = U.height;
        m_u.resize(static_cast<std::size_t>(cw) + 2);
        m_v.resize(static_cast<std::size_t>(cw) + 2);
        static constexpr int Weight[4] = {5, 7, 1, 3}; // weight of the right/lower sample, in 1/8
        for (int y = 0; y < h; ++y) {
            // vertical pass into a padded row (x8), then horizontal pass (x8)
            const int j = y & 3, wy = Weight[j];
            const int upper = j >= 2 ? (y >> 2) : std::max((y >> 2) - 1, 0);
            const int lower = j >= 2 ? std::min((y >> 2) + 1, ch - 1) : (y >> 2);
            for (int x = 0; x < cw; ++x) {
                const int ua = U.data[upper * U.pitch + x], ub = U.data[lower * U.pitch + x];
                const int va = V.data[upper * V.pitch + x], vb = V.data[lower * V.pitch + x];
                m_u[x + 1] = ua * (8 - wy) + ub * wy;
                m_v[x + 1] = va * (8 - wy) + vb * wy;
            }
            m_u[0] = m_u[1]; m_u[cw + 1] = m_u[cw];
            m_v[0] = m_v[1]; m_v[cw + 1] = m_v[cw];
            const std::uint8_t* ys = Y.data + y * Y.pitch;
            std::uint32_t* out = dst + static_cast<std::size_t>(y) * w;
            for (int x = 0; x < w; ++x) {
                const int i = x & 3, wx = Weight[i];
                const int left = (x >> 2) + (i >= 2 ? 1 : 0); // index in the padded row
                // samples are 7-bit (x2 = 8-bit): the bilinear sum is x64, so x2/64 = /32
                const int u = (m_u[left] * (8 - wx) + m_u[left + 1] * wx + 16) >> 5;
                const int v = (m_v[left] * (8 - wx) + m_v[left + 1] * wx + 16) >> 5;
                const int yy = m_y[ys[x] << 1];
                const int r = (yy + m_rv[v] + (1 << 15)) >> 16;
                const int g = (yy + m_gu[u] + m_gv[v] + (1 << 15)) >> 16;
                const int b = (yy + m_bu[u] + (1 << 15)) >> 16;
                out[x] = std::uint32_t(clamp8(r)) << 16 | std::uint32_t(clamp8(g)) << 8 | std::uint32_t(clamp8(b));
            }
        }
    }

private:
    static int clamp8(int v) noexcept { return v < 0 ? 0 : (v > 255 ? 255 : v); }
    std::array<int, 256> m_y{}, m_rv{}, m_gu{}, m_gv{}, m_bu{};
    std::vector<int> m_u, m_v;
};

// --- player -------------------------------------------------------------------------------

class Movie {
public:
    // Throws std::runtime_error if the file cannot be read or uses an unsupported codec.
    explicit Movie(const std::filesystem::path& path) : m_avi{std::make_unique<AviFile>(path)} {
        const int vs = m_avi->findStream(fourcc("vids"));
        if (vs < 0)
            throw std::runtime_error("AVI without video stream");
        const AviStream& s = m_avi->streams()[vs];
        m_video = &s;
        m_width = s.video.width;
        m_height = std::abs(s.video.height);
        if (m_width <= 0 || m_height <= 0 || m_width > 2048 || m_height > 2048)
            throw std::runtime_error("invalid movie size");
        m_fps = s.rate > 0 ? static_cast<double>(s.rate) / s.scale
                           : (m_avi->microSecPerFrame() ? 1e6 / m_avi->microSecPerFrame() : 15.0);
        m_frameStart = static_cast<std::size_t>(s.start);

        switch (s.video.compression) {
        case fourcc("CRAM"): case fourcc("cram"): case fourcc("MSVC"): case fourcc("msvc"):
        case fourcc("WHAM"): case fourcc("wham"):
            if (s.video.bitCount != 8 && s.video.bitCount != 16)
                throw std::runtime_error("MS Video 1: unsupported bit depth");
            m_msvideo1 = std::make_unique<MsVideo1Decoder>(m_width, m_height, s.video.bitCount);
            break;
        case fourcc("IV31"): case fourcc("iv31"): case fourcc("IV32"): case fourcc("iv32"):
            m_indeo3 = std::make_unique<Indeo3Decoder>(m_width, m_height);
            m_yuv = std::make_unique<YuvToRgb>();
            break;
        default:
            throw std::runtime_error("unsupported video codec " + fourccString(s.video.compression));
        }
        m_picture.assign(static_cast<std::size_t>(m_width) * m_height, 0);

        if (const int as = m_avi->findStream(fourcc("auds")); as >= 0) {
            const AviStream& a = m_avi->streams()[as];
            const auto& f = a.audio;
            if (f.formatTag == 1 && (f.bitsPerSample == 8 || f.bitsPerSample == 16) && (f.channels == 1 || f.channels == 2) &&
                f.sampleRate > 0)
                m_audio = std::make_unique<Audio>(*m_avi, a);
        }
    }

    [[nodiscard]] int width() const noexcept { return m_width; }
    [[nodiscard]] int height() const noexcept { return m_height; }
    [[nodiscard]] double frameRate() const noexcept { return m_fps; }
    [[nodiscard]] std::size_t frameCount() const noexcept { return m_frameStart + m_video->chunks.size(); }
    [[nodiscard]] double duration() const noexcept { return static_cast<double>(frameCount()) / m_fps; }
    [[nodiscard]] double time() const noexcept { return m_time; }
    [[nodiscard]] bool finished() const noexcept { return m_time >= duration() - 1e-9; }
    // Index of the frame on screen (-1 before the first step).
    [[nodiscard]] long currentFrame() const noexcept { return m_shown; }
    [[nodiscard]] std::size_t decodeErrors() const noexcept { return m_errors; }
    [[nodiscard]] std::size_t framesDecoded() const noexcept { return m_decoded; }
    // XRGB8888 picture, width() x height().
    [[nodiscard]] const std::vector<std::uint32_t>& picture() const noexcept { return m_picture; }
    // Soundtrack for Mixer::setStream (nullptr if the movie has none or it is unsupported).
    [[nodiscard]] AudioStream* audio() noexcept { return m_audio.get(); }

    // Shows the frame due at the current time, then advances the clock by dt seconds.
    // Returns true if the picture changed.
    bool step(double dt) {
        const auto due = static_cast<long>(std::floor(m_time * m_fps + 1e-6));
        const long last = static_cast<long>(frameCount()) - 1;
        const long target = std::min(due, last);
        bool changed = false;
        while (m_shown < target) {
            ++m_shown;
            // decode every frame (INTER frames need their references), convert only the last
            changed |= decodeFrame(static_cast<std::size_t>(m_shown), m_shown == target);
        }
        m_time += dt;
        return changed;
    }

    // Decodes the next frame without the clock (benchmarks, tests). False at the end.
    bool decodeNext(bool convert = true) {
        if (m_shown + 1 >= static_cast<long>(frameCount()))
            return false;
        ++m_shown;
        decodeFrame(static_cast<std::size_t>(m_shown), convert);
        return true;
    }

    // Native decoder output, for comparisons with reference decoders.
    [[nodiscard]] const MsVideo1Decoder* msvideo1() const noexcept { return m_msvideo1.get(); }
    [[nodiscard]] const Indeo3Decoder* indeo3() const noexcept { return m_indeo3.get(); }

    // Draws the picture into an XRGB8888 framebuffer, scaled to fit (aspect ratio kept,
    // black bars): 640x480 fills a 4:3 framebuffer and is pillarboxed in a 16:9 one.
    void blit(std::uint32_t* dst, int dstW, int dstH, std::size_t dstPitchPixels) const {
        const double scale = std::min(static_cast<double>(dstW) / m_width, static_cast<double>(dstH) / m_height);
        const int outW = std::clamp(static_cast<int>(std::lround(m_width * scale)), 1, dstW);
        const int outH = std::clamp(static_cast<int>(std::lround(m_height * scale)), 1, dstH);
        const int x0 = (dstW - outW) / 2, y0 = (dstH - outH) / 2;
        for (int y = 0; y < dstH; ++y) {
            std::uint32_t* row = dst + static_cast<std::size_t>(y) * dstPitchPixels;
            if (y < y0 || y >= y0 + outH) {
                std::fill(row, row + dstW, 0u);
                continue;
            }
            std::fill(row, row + x0, 0u);
            std::fill(row + x0 + outW, row + dstW, 0u);
            const std::uint32_t* src = m_picture.data() + static_cast<std::size_t>((y - y0) * m_height / outH) * m_width;
            if (outW == m_width) {
                std::copy(src, src + m_width, row + x0);
            } else {
                for (int x = 0; x < outW; ++x)
                    row[x0 + x] = src[x * m_width / outW];
            }
        }
    }

private:
    // Soundtrack: PCM chunks read on demand, converted to stereo int16.
    class Audio final : public AudioStream {
    public:
        Audio(AviFile& avi, const AviStream& s) : m_avi{avi}, m_stream{s} {
            const auto& f = s.audio;
            m_bytesPerFrame = static_cast<std::size_t>(f.channels) * (f.bitsPerSample / 8);
            // A delayed start (strh.dwStart, in blocks) is played as silence.
            if (s.rate > 0 && s.start > 0)
                m_silence = static_cast<std::size_t>(std::llround(static_cast<double>(s.start) * s.scale / s.rate * f.sampleRate));
        }
        [[nodiscard]] int sampleRate() const override { return static_cast<int>(m_stream.audio.sampleRate); }
        std::size_t read(std::int16_t* out, std::size_t frames) override {
            std::size_t done = 0;
            for (; done < frames && m_silence > 0; ++done, --m_silence)
                out[2 * done] = out[2 * done + 1] = 0;
            while (done < frames) {
                if (m_pos + m_bytesPerFrame > m_buf.size()) {
                    if (!nextChunk())
                        break;
                    continue;
                }
                const std::uint8_t* p = m_buf.data() + m_pos;
                std::int16_t l, r;
                if (m_stream.audio.bitsPerSample == 8) {
                    l = static_cast<std::int16_t>((p[0] - 128) * 256);
                    r = m_stream.audio.channels == 2 ? static_cast<std::int16_t>((p[1] - 128) * 256) : l;
                } else {
                    l = static_cast<std::int16_t>(p[0] | p[1] << 8);
                    r = m_stream.audio.channels == 2 ? static_cast<std::int16_t>(p[2] | p[3] << 8) : l;
                }
                out[2 * done] = l;
                out[2 * done + 1] = r;
                m_pos += m_bytesPerFrame;
                ++done;
            }
            return done;
        }

    private:
        bool nextChunk() {
            // a partial frame at the end of a chunk continues in the next one
            std::vector<std::uint8_t> rest(m_buf.begin() + static_cast<std::ptrdiff_t>(std::min(m_pos, m_buf.size())), m_buf.end());
            while (m_chunk < m_stream.chunks.size()) {
                if (!m_avi.read(m_stream.chunks[m_chunk++], m_buf))
                    return false;
                if (m_buf.empty())
                    continue;
                m_buf.insert(m_buf.begin(), rest.begin(), rest.end());
                m_pos = 0;
                return true;
            }
            return false;
        }

        AviFile& m_avi;
        const AviStream& m_stream;
        std::size_t m_bytesPerFrame = 2;
        std::size_t m_chunk = 0, m_pos = 0, m_silence = 0;
        std::vector<std::uint8_t> m_buf;
    };

    bool decodeFrame(std::size_t index, bool convert) {
        if (index < m_frameStart)
            return false; // delayed video start: black
        const AviChunk& c = m_video->chunks[index - m_frameStart];
        if (c.size == 0)
            return false; // drop frame: the previous picture stays
        if (!m_avi->read(c, m_buf)) {
            ++m_errors;
            return false;
        }
        bool ok = true;
        if (m_msvideo1) {
            ok = m_msvideo1->decode(m_buf);
            if (convert) {
                if (m_msvideo1->palettised()) paletteToXrgb(m_msvideo1->indices(), m_video->video.palette, m_picture.data());
                else rgb555ToXrgb(m_msvideo1->rgb555(), m_picture.data());
            }
        } else {
            const auto status = m_indeo3->decode(m_buf);
            ok = status != Indeo3Decoder::Status::Error;
            if (status != Indeo3Decoder::Status::Decoded)
                convert = false;
            if (convert && (m_indeo3->width() != m_width || m_indeo3->height() != m_height))
                convert = false; // size change mid-stream: not supported by the player
            if (convert) m_yuv->convert(*m_indeo3, m_picture.data());
        }
        ++m_decoded;
        if (!ok) ++m_errors;
        return convert;
    }

    std::unique_ptr<AviFile> m_avi;
    const AviStream* m_video = nullptr;
    int m_width = 0, m_height = 0;
    double m_fps = 15.0;
    std::size_t m_frameStart = 0;
    std::unique_ptr<MsVideo1Decoder> m_msvideo1;
    std::unique_ptr<Indeo3Decoder> m_indeo3;
    std::unique_ptr<YuvToRgb> m_yuv;
    std::unique_ptr<Audio> m_audio;
    std::vector<std::uint32_t> m_picture;
    std::vector<std::uint8_t> m_buf;
    double m_time = 0.0;
    long m_shown = -1;
    std::size_t m_errors = 0, m_decoded = 0;
};

} // namespace gromada::video
