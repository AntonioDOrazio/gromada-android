// Movie module on the game's movies (needs GROMADA_DATA; skipped in CI):
//  - every movie decodes completely without errors (frame counts from the AVI headers);
//  - decoding speed (ms per frame, with and without the conversion to XRGB);
//  - if ffmpeg is in PATH: decoder output compared with ffmpeg on EVERY coded frame
//    (MS Video 1: RGB555, Indeo 3: YUV 4:1:0 planes), and the XRGB picture compared with
//    ffmpeg's RGB conversion on every 10th coded frame (PSNR);
//  - playback at 60 and 120 fps through the mixer: duration in output frames and A/V sync
//    (picture on screen = frame of the audio clock) at every output frame.
#include "engine/audio.hpp"
#include "video/movie.hpp"
#include "test.hpp"

#include <chrono>
#include <cstdio>

using namespace gromada::video;
using Clock = std::chrono::steady_clock;

namespace {

struct Expected {
    const char* file;
    std::uint32_t codec;
    std::size_t frames;      // video frames (15 fps)
    std::size_t audioFrames; // PCM frames (22050 Hz)
};
constexpr Expected kMovies[] = {
    {"0.avi", fourcc("CRAM"), 235, 345435},
    {"1.avi", fourcc("IV32"), 231, 339570},
    {"2.avi", fourcc("IV32"), 2772, 4108288},
    {"3.avi", fourcc("IV32"), 1176, 2116862},
};

bool haveFfmpeg() { return std::system("ffmpeg -version > /dev/null 2>&1") == 0; }

FILE* ffmpegPipe(const std::filesystem::path& file, const std::string& args) {
    const std::string cmd = "ffmpeg -v error -nostdin -i \"" + file.string() + "\" -map 0:v " + args + " -fps_mode passthrough -f rawvideo -";
    return popen(cmd.c_str(), "r");
}

bool readExactly(FILE* f, void* dst, std::size_t n) { return f && std::fread(dst, 1, n, f) == n; }

double psnr(const std::vector<std::uint32_t>& xrgb, const std::vector<std::uint8_t>& rgb24) {
    double se = 0;
    for (std::size_t i = 0; i < xrgb.size(); ++i)
        for (int c = 0; c < 3; ++c) {
            const int a = static_cast<int>((xrgb[i] >> (16 - 8 * c)) & 0xFF), b = rgb24[3 * i + c];
            se += double(a - b) * (a - b);
        }
    const double mse = se / (xrgb.size() * 3.0);
    return mse == 0 ? INFINITY : 10 * std::log10(255.0 * 255.0 / mse);
}

// Decoder output equals ffmpeg's raw output (native format) for this frame?
bool sameAsReference(const Movie& m, const std::vector<std::uint8_t>& ref) {
    if (const auto* d = m.msvideo1()) {
        const auto px = d->rgb555();
        for (std::size_t i = 0; i < px.size(); ++i)
            if (px[i] != ((ref[2 * i] | ref[2 * i + 1] << 8) & 0x7FFF)) return false;
        return true;
    }
    const auto* d = m.indeo3();
    std::size_t off = 0;
    for (int p = 0; p < 3; ++p) {
        const auto v = d->plane(p);
        const int w = p ? (d->width() + 3) / 4 : d->width(), h = p ? (d->height() + 3) / 4 : d->height();
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                if ((v.data[y * v.pitch + x] << 1) != ref[off + static_cast<std::size_t>(y) * w + x]) return false;
        off += static_cast<std::size_t>(w) * h;
    }
    return true;
}

void decodeAndCompare(const std::filesystem::path& path, const Expected& e, bool ffmpeg) {
    // speed: decode only, then decode + conversion
    double decodeMs = 0, totalMs = 0;
    std::size_t coded = 0;
    {
        Movie m{path};
        const auto t0 = Clock::now();
        while (m.decodeNext(false)) {}
        decodeMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        coded = m.framesDecoded();
    }

    Movie m{path};
    CHECK_EQ(m.frameCount(), e.frames);
    CHECK(m.audio() != nullptr);
    CHECK(std::abs(m.audioDuration() - e.audioFrames / 22050.0) < 1e-9);
    const bool cram = m.msvideo1() != nullptr;
    CHECK_EQ(cram, e.codec == fourcc("CRAM"));
    const std::size_t pixels = static_cast<std::size_t>(m.width()) * m.height();
    FILE* nativeRef = ffmpeg ? ffmpegPipe(path, cram ? "-pix_fmt rgb555le" : "-pix_fmt yuv410p") : nullptr;
    FILE* rgbRef = ffmpeg ? ffmpegPipe(path, "-vf \"select=not(mod(n\\,10))\" -pix_fmt rgb24") : nullptr;
    std::vector<std::uint8_t> native(cram ? pixels * 2 : pixels * 9 / 8), rgb(pixels * 3);

    std::size_t exact = 0, compared = 0, sampled = 0;
    double psnrSum = 0, psnrMin = INFINITY;
    bool finiteSeen = false;
    const auto t0 = Clock::now();
    double compareMs = 0;
    while (true) {
        const std::size_t before = m.framesDecoded();
        if (!m.decodeNext(true))
            break;
        if (m.framesDecoded() == before)
            continue; // drop frame: nothing coded, ffmpeg outputs nothing in passthrough mode
        if (!ffmpeg)
            continue;
        const auto c0 = Clock::now();
        const std::size_t n = before; // index among coded frames
        if (readExactly(nativeRef, native.data(), native.size())) {
            ++compared;
            exact += sameAsReference(m, native);
        }
        if (n % 10 == 0 && readExactly(rgbRef, rgb.data(), rgb.size())) {
            const double p = psnr(m.picture(), rgb);
            ++sampled;
            psnrMin = std::min(psnrMin, p);
            if (std::isfinite(p)) { psnrSum += p; finiteSeen = true; }
        }
        compareMs += std::chrono::duration<double, std::milli>(Clock::now() - c0).count();
    }
    totalMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count() - compareMs;
    if (nativeRef) pclose(nativeRef);
    if (rgbRef) pclose(rgbRef);

    CHECK_EQ(m.decodeErrors(), std::size_t{0});
    CHECK_EQ(m.currentFrame() + 1, static_cast<long>(e.frames));
    std::fprintf(stderr, "%s %s: %zu frames (%zu coded), %zu errors; %.3f ms/frame decode, %.3f ms/frame decode+RGB\n",
                 e.file, fourccString(e.codec).c_str(), e.frames, coded, m.decodeErrors(), decodeMs / coded, totalMs / coded);
    if (ffmpeg) {
        const double avg = finiteSeen ? psnrSum / sampled : INFINITY; // (all finite for Indeo 3)
        std::fprintf(stderr, "    vs ffmpeg: decoder output bit-exact on %zu/%zu coded frames; RGB PSNR on %zu frames: avg %.2f dB, min %.2f dB%s\n",
                     exact, compared, sampled, avg, psnrMin, cram ? " (inf = identical)" : "");
        CHECK_EQ(compared, coded);
        CHECK_EQ(exact, compared);
        CHECK(sampled == (coded + 9) / 10);
        if (cram) CHECK(std::isinf(psnrMin)); // RGB555 -> RGB888 expansion identical to ffmpeg's
        else CHECK(avg >= 40.0 && psnrMin >= 38.0);
    }
}

// Plays like the core (step + mixer render per output frame) and checks every frame.
void playAt(const std::filesystem::path& path, const Expected& e, int fps) {
    Movie m{path};
    gromada::Mixer mixer{44100};
    mixer.setStream(m.audio());
    const double rate = m.audio()->sampleRate();
    std::vector<std::int16_t> out;
    double debt = 0, maxLead = 0, maxLag = 0;
    long steps = 0, syncErrors = 0, frameErrors = 0;
    const auto t0 = Clock::now();
    while (!m.finished() && steps < 1'000'000) {
        const bool videoRunning = m.time() < m.videoDuration() - 1e-9;
        m.step(1.0 / fps);
        if (m.currentFrame() != std::min<long>(steps * 15 / fps, static_cast<long>(e.frames) - 1)) ++frameErrors;
        debt += 44100.0 / fps;
        const auto n = static_cast<std::size_t>(debt);
        debt -= n;
        out.resize(2 * n);
        // audio clock at the start of this output frame vs the picture on screen
        const double audioTime = mixer.streamPosition() / rate;
        const double frameStart = m.currentFrame() / m.frameRate();
        const double offset = audioTime - frameStart; // in [0, 1/15) when in sync
        if (videoRunning) { // after the last frame only the soundtrack goes on
            maxLead = std::max(maxLead, offset);
            maxLag = std::min(maxLag, offset);
            if (offset < -1e-9 || offset >= 1.0 / m.frameRate() - 1e-9) ++syncErrors;
        }
        mixer.render(out.data(), n);
        ++steps;
    }
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    // until the end of the longer stream (only 3.avi has a long soundtrack tail)
    const long expectedSteps = static_cast<long>(std::ceil(std::max(e.frames / 15.0, e.audioFrames / 22050.0) * fps - 1e-9));
    std::fprintf(stderr, "    %3d fps: %ld output frames = %.3f s (expected %ld; video %.3f s), audio-picture offset in [%.1f, %.1f] ms, "
                 "%ld sync errors, %ld frame errors, %.3f ms/output frame\n",
                 fps, steps, steps / double(fps), expectedSteps, e.frames / 15.0, maxLag * 1000, maxLead * 1000,
                 syncErrors, frameErrors, ms / steps);
    CHECK_EQ(steps, expectedSteps);
    CHECK_EQ(syncErrors, 0L);
    CHECK_EQ(frameErrors, 0L);
}

} // namespace

int main() {
    SKIP_WITHOUT_DATA();
    const auto dir = *gromada_test::dataDir() / "avi";
    const bool ffmpeg = haveFfmpeg();
    if (!ffmpeg)
        std::fprintf(stderr, "ffmpeg not found: comparisons skipped\n");
    for (const auto& e : kMovies) {
        const auto path = dir / e.file;
        try {
            decodeAndCompare(path, e, ffmpeg);
            playAt(path, e, 60);
            playAt(path, e, 120);
        } catch (const std::exception& ex) {
            CHECK(!"exception");
            std::fprintf(stderr, "%s: %s\n", e.file, ex.what());
        }
    }
    return gromada_test::finish();
}
