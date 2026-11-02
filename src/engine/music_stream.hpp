#pragma once
// Streaming decoders for the CD soundtrack (music/trackNN.*):
//  - FLAC with dr_flac (lossless rip, what the Android installer writes),
//  - Ogg Vorbis with stb_vorbis,
//  - WAV (PCM) with dr_wav.
// Every stream yields interleaved stereo int16 at the file's own sample rate (mono is
// duplicated, extra channels are dropped); the Mixer resamples to the output rate.
// The decoders are C units: src/third_party/stb_vorbis_impl.c and dr_libs_impl.c.

#include "core/std.hpp"
#include <cctype>
#include "third_party/dr_flac.h" // declarations only (no DR_FLAC_IMPLEMENTATION here)
#include "third_party/dr_wav.h"

extern "C" {
struct stb_vorbis;
stb_vorbis* stb_vorbis_open_filename(const char* filename, int* error, const void* alloc_buffer);
void stb_vorbis_close(stb_vorbis* f);
int stb_vorbis_get_samples_short_interleaved(stb_vorbis* f, int channels, short* buffer, int num_shorts);
int stb_vorbis_seek(stb_vorbis* f, unsigned int sample_number);
}
// stb_vorbis_get_info returns a struct by value; wrapped in the C unit (stb_vorbis_impl.c).
extern "C" void gromada_vorbis_info(stb_vorbis* f, unsigned* sample_rate, int* channels);

namespace gromada {

class MusicStream {
public:
    virtual ~MusicStream() = default;

    // Music file extensions in lookup order: the lossless rip first.
    static constexpr std::array<std::string_view, 3> Extensions{".flac", ".ogg", ".wav"};

    // Opens a file by extension (case-insensitive); nullptr if unsupported or unreadable.
    static std::unique_ptr<MusicStream> open(const std::filesystem::path& path);

    [[nodiscard]] unsigned sampleRate() const noexcept { return m_rate; }

    // Reads up to `frames` stereo frames into `out` (2 * frames values); 0 at the end.
    virtual std::size_t read(std::int16_t* out, std::size_t frames) = 0;
    // Moves to source frame `frame` (save states); false if the decoder cannot.
    virtual bool seek(std::uint64_t frame) { (void)frame; return false; }

protected:
    unsigned m_rate = 0;
    unsigned m_channels = 0;
    std::vector<std::int16_t> m_tmp;

    // Converts `n` frames of m_channels channels in m_tmp to stereo in `out`.
    void toStereo(std::int16_t* out, std::size_t n) const {
        for (std::size_t i = 0; i < n; ++i) {
            const std::int16_t* f = &m_tmp[i * m_channels];
            out[2 * i] = f[0];
            out[2 * i + 1] = m_channels > 1 ? f[1] : f[0];
        }
    }
};

class VorbisStream final : public MusicStream {
public:
    explicit VorbisStream(const std::filesystem::path& path) {
        int err = 0;
        m_file = stb_vorbis_open_filename(path.string().c_str(), &err, nullptr);
        if (m_file) {
            int channels = 0;
            gromada_vorbis_info(m_file, &m_rate, &channels);
            m_channels = static_cast<unsigned>(channels);
        }
    }
    ~VorbisStream() override { if (m_file) stb_vorbis_close(m_file); }
    [[nodiscard]] bool ok() const noexcept { return m_file && m_rate > 0; }

    std::size_t read(std::int16_t* out, std::size_t frames) override {
        const int got = stb_vorbis_get_samples_short_interleaved(m_file, 2, out, static_cast<int>(frames * 2));
        return got > 0 ? static_cast<std::size_t>(got) : 0;
    }
    bool seek(std::uint64_t frame) override {
        return frame <= 0xFFFFFFFFu && stb_vorbis_seek(m_file, static_cast<unsigned>(frame)) != 0;
    }

private:
    stb_vorbis* m_file = nullptr;
};

class FlacStream final : public MusicStream {
public:
    explicit FlacStream(const std::filesystem::path& path) : m_file{drflac_open_file(path.string().c_str(), nullptr)} {
        if (m_file) {
            m_rate = m_file->sampleRate;
            m_channels = m_file->channels;
        }
    }
    ~FlacStream() override { if (m_file) drflac_close(m_file); }
    [[nodiscard]] bool ok() const noexcept { return m_file && m_rate > 0 && m_channels > 0; }

    std::size_t read(std::int16_t* out, std::size_t frames) override {
        m_tmp.resize(frames * m_channels);
        const auto got = static_cast<std::size_t>(drflac_read_pcm_frames_s16(m_file, frames, m_tmp.data()));
        toStereo(out, got);
        return got;
    }
    bool seek(std::uint64_t frame) override { return drflac_seek_to_pcm_frame(m_file, frame) != 0; }

private:
    drflac* m_file;
};

class WavStream final : public MusicStream {
public:
    explicit WavStream(const std::filesystem::path& path) {
        m_open = drwav_init_file(&m_wav, path.string().c_str(), nullptr);
        if (m_open) {
            m_rate = m_wav.sampleRate;
            m_channels = m_wav.channels;
        }
    }
    ~WavStream() override { if (m_open) drwav_uninit(&m_wav); }
    [[nodiscard]] bool ok() const noexcept { return m_open && m_rate > 0 && m_channels > 0; }

    std::size_t read(std::int16_t* out, std::size_t frames) override {
        m_tmp.resize(frames * m_channels);
        const auto got = static_cast<std::size_t>(drwav_read_pcm_frames_s16(&m_wav, frames, m_tmp.data()));
        toStereo(out, got);
        return got;
    }
    bool seek(std::uint64_t frame) override { return drwav_seek_to_pcm_frame(&m_wav, frame) != 0; }

private:
    drwav m_wav{};
    bool m_open = false;
};

inline std::unique_ptr<MusicStream> MusicStream::open(const std::filesystem::path& path) {
    auto ext = path.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto keep = [](auto s) -> std::unique_ptr<MusicStream> { return s->ok() ? std::move(s) : nullptr; };
    if (ext == ".flac") return keep(std::make_unique<FlacStream>(path));
    if (ext == ".ogg") return keep(std::make_unique<VorbisStream>(path));
    if (ext == ".wav") return keep(std::make_unique<WavStream>(path));
    return nullptr;
}

} // namespace gromada
