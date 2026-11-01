// Music streams: FLAC (dr_flac), WAV (dr_wav) and OGG (stb_vorbis) through the Mixer.
// The FLAC and WAV files are synthetic and written by this test; the OGG is a tiny
// synthetic sine (tests/data/sine440.ogg, made with ffmpeg + libvorbis).
#include "engine/audio.hpp"
#include "test.hpp"

namespace fs = std::filesystem;
using gromada::Mixer;

namespace {

// ---- minimal FLAC writer: verbatim subframes, independent channels, CRCs ----
struct BitWriter {
    std::vector<std::uint8_t> bytes;
    unsigned acc = 0;
    int used = 0;
    void put(std::uint64_t v, int bits) {
        for (int i = bits - 1; i >= 0; --i) {
            acc = (acc << 1) | static_cast<unsigned>((v >> i) & 1);
            if (++used == 8) { bytes.push_back(static_cast<std::uint8_t>(acc)); acc = 0; used = 0; }
        }
    }
    void align() { while (used) put(0, 1); }
};

std::uint8_t crc8(const std::uint8_t* p, std::size_t n) {
    unsigned c = 0;
    for (std::size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c & 0x80) ? ((c << 1) ^ 0x07) & 0xFF : (c << 1) & 0xFF;
    }
    return static_cast<std::uint8_t>(c);
}

std::uint16_t crc16(const std::uint8_t* p, std::size_t n) {
    unsigned c = 0;
    for (std::size_t i = 0; i < n; ++i) {
        c ^= static_cast<unsigned>(p[i]) << 8;
        for (int k = 0; k < 8; ++k) c = (c & 0x8000) ? ((c << 1) ^ 0x8005) & 0xFFFF : (c << 1) & 0xFFFF;
    }
    return static_cast<std::uint16_t>(c);
}

// 44.1 kHz, 16-bit; `pcm` interleaved with `channels` channels.
void writeFlac(const fs::path& path, const std::vector<std::int16_t>& pcm, int channels, int block) {
    const std::size_t frames = pcm.size() / channels;
    BitWriter w;
    for (char c : std::string{"fLaC"}) w.put(static_cast<std::uint8_t>(c), 8);
    w.put(1, 1); w.put(0, 7); w.put(34, 24);              // last block, STREAMINFO
    w.put(block, 16); w.put(block, 16); w.put(0, 24); w.put(0, 24);
    w.put(44100, 20); w.put(channels - 1, 3); w.put(15, 5); w.put(frames, 36);
    for (int i = 0; i < 16; ++i) w.put(0, 8);              // MD5 unknown
    std::size_t frameNo = 0;
    for (std::size_t start = 0; start < frames; start += block, ++frameNo) {
        const std::size_t n = std::min<std::size_t>(block, frames - start);
        const std::size_t frameStart = w.bytes.size();
        w.put(0x3FFE, 14); w.put(0, 1); w.put(0, 1);
        w.put(7, 4);                                       // block size: 16 bits follow
        w.put(9, 4);                                       // 44.1 kHz
        w.put(channels - 1, 4);                            // independent channels
        w.put(4, 3); w.put(0, 1);                          // 16 bits per sample
        w.put(frameNo, 8);                                 // UTF-8 frame number (< 128)
        w.put(n - 1, 16);
        w.put(crc8(&w.bytes[frameStart], w.bytes.size() - frameStart), 8);
        for (int ch = 0; ch < channels; ++ch) {
            w.put(0, 1); w.put(1, 6); w.put(0, 1);         // verbatim subframe
            for (std::size_t i = 0; i < n; ++i)
                w.put(static_cast<std::uint16_t>(pcm[(start + i) * channels + ch]), 16);
        }
        w.align();
        w.put(crc16(&w.bytes[frameStart], w.bytes.size() - frameStart), 16);
    }
    std::ofstream{path, std::ios::binary}.write(reinterpret_cast<const char*>(w.bytes.data()), static_cast<std::streamsize>(w.bytes.size()));
}

void writeWav(const fs::path& path, const std::vector<std::int16_t>& pcm, int channels, int rate) {
    std::ofstream f{path, std::ios::binary};
    auto u32 = [&](std::uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](std::uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const auto bytes = static_cast<std::uint32_t>(pcm.size() * 2);
    f.write("RIFF", 4); u32(36 + bytes); f.write("WAVEfmt ", 8);
    u32(16); u16(1); u16(static_cast<std::uint16_t>(channels)); u32(static_cast<std::uint32_t>(rate));
    u32(static_cast<std::uint32_t>(rate * channels * 2)); u16(static_cast<std::uint16_t>(channels * 2)); u16(16);
    f.write("data", 4); u32(bytes);
    f.write(reinterpret_cast<const char*>(pcm.data()), bytes);
}

// What the Mixer outputs for one music sample at volume 1 (see Mixer::mixMusic/render).
std::int16_t expected(float s) {
    return static_cast<std::int16_t>(std::clamp(s * (1.0f / 32768.0f), -1.0f, 1.0f) * 32767.0f);
}

std::vector<std::int16_t> ramp(std::size_t frames) {
    std::vector<std::int16_t> v(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
        const int l = static_cast<int>(i * 13 % 60000) - 30000;
        v[2 * i] = static_cast<std::int16_t>(l);
        v[2 * i + 1] = static_cast<std::int16_t>(-l - 1);
    }
    return v;
}

std::vector<std::int16_t> render(Mixer& m, std::size_t frames) {
    std::vector<std::int16_t> out(frames * 2);
    m.render(out.data(), frames);
    return out;
}

} // namespace

int main() {
    const auto dir = fs::temp_directory_path() / std::format("gromada_music_test_{}", std::random_device{}());
    fs::create_directories(dir);

    constexpr std::size_t N = 2500; // > one FLAC block, last block partial
    const auto pcm = ramp(N);
    writeFlac(dir / "track02.flac", pcm, 2, 1000);

    // FLAC at 44.1 kHz: sample-exact, then the 4-entry playlist loops the same track.
    {
        Mixer m{44100};
        m.setMusicVolume(1.0f);
        m.setMusicDirectories({dir});
        m.playMusic({2, 2, 2, 2});
        CHECK_EQ(m.currentTrack(), 2);
        const auto out = render(m, N + 100);
        int bad = 0;
        for (std::size_t i = 0; i < N + 100; ++i) {
            const std::size_t s = i % N;
            if (out[2 * i] != expected(pcm[2 * s]) || out[2 * i + 1] != expected(pcm[2 * s + 1])) ++bad;
        }
        CHECK_EQ(bad, 0);
        CHECK_EQ(m.currentTrack(), 2);
    }

    // Mono WAV at 22.05 kHz with a spaced name: upsampled x2, duplicated on both channels.
    {
        std::vector<std::int16_t> mono(1000);
        for (std::size_t i = 0; i < mono.size(); ++i) mono[i] = static_cast<std::int16_t>(static_cast<int>(i) * 50 - 25000);
        writeWav(dir / "track 06.wav", mono, 1, 22050);
        Mixer m{44100};
        m.setMusicVolume(1.0f);
        m.setMusicDirectories({dir});
        m.playMusic({6, 6, 6, 6});
        CHECK_EQ(m.currentTrack(), 6);
        const auto out = render(m, 1000);
        int bad = 0;
        for (std::size_t j = 0; j + 2 < 1000; ++j) {
            const float src = j % 2 == 0 ? mono[j / 2] : (mono[j / 2] + mono[j / 2 + 1]) * 0.5f;
            if (std::abs(out[2 * j] - expected(src)) > 1 || out[2 * j] != out[2 * j + 1]) ++bad;
        }
        CHECK_EQ(bad, 0);
    }

    // Lookup order: .flac wins over .ogg for the same track; .ogg alone still plays.
    const auto ogg = fs::path{GROMADA_TEST_DATA_DIR} / "sine440.ogg";
    CHECK(fs::exists(ogg));
    fs::copy_file(ogg, dir / "track04.ogg");
    writeFlac(dir / "track04.flac", pcm, 2, 4096);
    fs::copy_file(ogg, dir / "Track05.ogg");
    {
        Mixer m{44100};
        m.setMusicVolume(1.0f);
        m.setMusicDirectories({dir / "missing", dir});
        m.playMusic({4, 4, 4, 4});
        CHECK_EQ(m.currentTrack(), 4);
        const auto out = render(m, 500);
        CHECK(out[0] == expected(pcm[0]) && out[999] == expected(pcm[999]));

        m.playMusic({5, 5, 5, 5});
        CHECK_EQ(m.currentTrack(), 5);
        const auto sine = render(m, 4410);
        int peak = 0;
        for (auto v : sine) peak = std::max(peak, std::abs(static_cast<int>(v)));
        CHECK(peak > 3000); // a 440 Hz sine, not silence

        m.playMusic({9, 9, 9, 9}); // no such track: silence, no crash
        CHECK_EQ(m.currentTrack(), 0);
        const auto quiet = render(m, 100);
        CHECK(std::ranges::all_of(quiet, [](std::int16_t v) { return v == 0; }));
    }

    // Unsupported or broken files are skipped.
    {
        std::ofstream{dir / "track07.flac"} << "not a flac file";
        CHECK(gromada::MusicStream::open(dir / "track07.flac") == nullptr);
        CHECK(gromada::MusicStream::open(dir / "track02.mp3") == nullptr);
        auto s = gromada::MusicStream::open(dir / "track02.flac");
        CHECK(s != nullptr && s->sampleRate() == 44100);
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
    return gromada_test::finish();
}
