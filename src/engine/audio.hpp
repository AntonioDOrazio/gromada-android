#pragma once
// Software mixer for the libretro core. The frontend pulls exactly the number of
// samples that match the frame rate, so everything is mixed synchronously in retro_run:
//  - sound effects: SoundData from fw.res (8/16-bit or float PCM, any rate), resampled
//    with linear interpolation to the output rate;
//  - music: the original CD audio tracks, provided by the user as music/trackNN.flac,
//    .ogg or .wav (engine/music_stream.hpp), streamed and cycled as a 4-entry playlist,
//    mirroring the game's PlayMusic(m1, m2, m3, m4) script call;
//  - one streaming source (AudioStream, e.g. a movie soundtrack), any rate, resampled.

#include "core/std.hpp"
#include "engine/audio_stream.hpp"
#include "engine/music_stream.hpp"
#include "libgromada/resources_sound.hpp"

namespace gromada {

class Mixer {
public:
    explicit Mixer(int outputRate = 44100) : m_outRate{outputRate} {}
    ~Mixer() { closeMusic(); }
    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;

    void setMusicDirectories(std::vector<std::filesystem::path> dirs) { m_musicDirs = std::move(dirs); }
    void setMusicVolume(float v) noexcept { m_musicVolume = std::clamp(v, 0.0f, 1.0f); }
    void setSfxVolume(float v) noexcept { m_sfxVolume = std::clamp(v, 0.0f, 1.0f); }

    // volume 0..1, pan -1 (left) .. +1 (right). Returns a voice id (0 = not started) that
    // can be used to follow, update or stop the voice; `loop` repeats the sound until stopped.
    std::uint32_t playSfx(const SoundData& sound, float volume = 1.0f, float pan = 0.0f, bool loop = false) {
        if (m_voices.size() >= MaxVoices)
            m_voices.erase(m_voices.begin()); // drop the oldest voice
        const auto frames = std::visit([](const auto& v) { return v.size(); }, sound.waveData) / std::max<std::uint16_t>(sound.numChannels, 1);
        if (frames == 0)
            return 0;
        if (++m_nextVoiceId == 0)
            ++m_nextVoiceId;
        Voice v{
            .sound = &sound,
            .frames = frames,
            .step = static_cast<double>(sound.sampleRate) / m_outRate,
            .id = m_nextVoiceId,
            .loop = loop,
        };
        setGains(v, volume, pan);
        m_voices.push_back(v);
        return v.id;
    }

    [[nodiscard]] bool sfxPlaying(std::uint32_t id) const noexcept {
        return id && std::ranges::any_of(m_voices, [id](const Voice& v) { return v.id == id && !v.paused; });
    }
    // DirectSound Stop/Play: a paused voice keeps its position and resumes from it. A one-shot
    // voice that reached its end is gone: resumeSfxVoice() returns false (play it again).
    void pauseSfxVoice(std::uint32_t id) noexcept {
        for (auto& v : m_voices)
            if (v.id == id)
                v.paused = true;
    }
    bool resumeSfxVoice(std::uint32_t id, bool loop) noexcept {
        for (auto& v : m_voices)
            if (id && v.id == id) {
                v.paused = false;
                v.loop = loop;
                return true;
            }
        return false;
    }
    void setSfxVolumePan(std::uint32_t id, float volume, float pan) noexcept {
        for (auto& v : m_voices)
            if (v.id == id)
                setGains(v, volume, pan);
    }
    void stopSfxVoice(std::uint32_t id) {
        std::erase_if(m_voices, [id](const Voice& v) { return v.id == id; });
    }

    // Stops every sound effect (script StopSFX()), or the voices playing one sound.
    void stopSfx() { m_voices.clear(); }
    void stopSfx(const SoundData* sound) {
        std::erase_if(m_voices, [sound](const Voice& v) { return v.sound == sound; });
    }

    // Track numbers are CD track numbers (2..12), as used by the game scripts.
    void playMusic(std::array<int, 4> playlist) {
        if (m_playlist == playlist && m_music)
            return; // already playing this playlist: don't restart
        m_playlist = playlist;
        openTrack(m_playlist[0]);
    }

    void stopMusic() {
        closeMusic();
        m_playlist = {};
    }

    [[nodiscard]] int currentTrack() const noexcept { return m_music ? m_currentTrack : 0; }
    [[nodiscard]] std::size_t activeVoices() const noexcept { return m_voices.size(); }

    // Mixes `frames` stereo frames (interleaved int16) into `out`.
    void render(std::int16_t* out, std::size_t frames) {
        m_mix.assign(frames * 2, 0.0f);
        mixMusic(frames);
        mixVoices(frames);
        mixStream(frames);
        for (std::size_t i = 0; i < frames * 2; ++i)
            out[i] = static_cast<std::int16_t>(std::clamp(m_mix[i], -1.0f, 1.0f) * 32767.0f);
    }

private:
    static constexpr std::size_t MaxVoices = 24;

    struct Voice {
        const SoundData* sound;
        std::size_t frames;
        double step;
        double pos = 0.0;
        float gainL = 1.0f, gainR = 1.0f;
        std::uint32_t id = 0;
        bool loop = false;
        bool paused = false;
    };

    static void setGains(Voice& v, float volume, float pan) noexcept {
        pan = std::clamp(pan, -1.0f, 1.0f);
        v.gainL = volume * std::min(1.0f, 1.0f - pan);
        v.gainR = volume * std::min(1.0f, 1.0f + pan);
    }

    static float sampleAt(const SoundData& s, std::size_t frame) noexcept {
        const std::size_t idx = frame * s.numChannels; // mono mixdown: first channel
        return std::visit([idx](const auto& v) -> float {
            using T = typename std::decay_t<decltype(v)>::value_type;
            if constexpr (std::is_same_v<T, std::uint8_t>) return (static_cast<int>(v[idx]) - 128) / 128.0f;
            else if constexpr (std::is_same_v<T, std::int16_t>) return v[idx] / 32768.0f;
            else return static_cast<float>(v[idx]);
        }, s.waveData);
    }

    void mixVoices(std::size_t frames) {
        const float master = m_sfxVolume * 0.6f; // headroom: several voices at once
        for (auto& v : m_voices) {
            if (v.paused)
                continue;
            for (std::size_t i = 0; i < frames; ++i) {
                while (v.loop && v.pos >= static_cast<double>(v.frames))
                    v.pos -= static_cast<double>(v.frames); // seamless wrap of looping sounds
                const auto i0 = static_cast<std::size_t>(v.pos);
                if (i0 >= v.frames) break;
                const float t = static_cast<float>(v.pos - i0);
                const float a = sampleAt(*v.sound, i0);
                const float b = i0 + 1 < v.frames ? sampleAt(*v.sound, i0 + 1) : (v.loop ? sampleAt(*v.sound, 0) : a);
                const float s = (a + (b - a) * t) * master;
                m_mix[2 * i] += s * v.gainL;
                m_mix[2 * i + 1] += s * v.gainR;
                v.pos += v.step;
            }
        }
        std::erase_if(m_voices, [](const Voice& v) { return !v.loop && v.pos >= v.frames; });
    }

    // --- music ---

    // Lookup order: for each music directory, extension (.flac, .ogg, .wav), then
    // spelling (trackNN, TrackNN, "track NN", "Track NN"). The first readable file wins.
    std::unique_ptr<MusicStream> findTrack(int track) const {
        for (const auto& dir : m_musicDirs) {
            for (const auto ext : MusicStream::Extensions) {
                for (const auto* fmt : {"track{:02}", "Track{:02}", "track {:02}", "Track {:02}"}) {
                    auto p = dir / (std::vformat(fmt, std::make_format_args(track)) + std::string{ext});
                    std::error_code ec;
                    if (!std::filesystem::exists(p, ec))
                        continue;
                    if (auto s = MusicStream::open(p))
                        return s;
                }
            }
        }
        return nullptr;
    }

    void openTrack(int track) {
        closeMusic();
        m_music = findTrack(track);
        if (!m_music)
            return;
        m_musicStep = static_cast<double>(m_music->sampleRate()) / m_outRate;
        m_musicPos = 0.0;
        m_musicBuf.clear();
        m_musicBufStart = 0;
        m_currentTrack = track;
    }

    void closeMusic() { m_music.reset(); }

    [[nodiscard]] int nextTrack(int cur) const noexcept {
        for (std::size_t i = 0; i + 1 < m_playlist.size(); ++i)
            if (cur == m_playlist[i])
                return m_playlist[i + 1];
        return m_playlist[0];
    }

    // Ensures the decoded window covers source frame index `needed`; false at end of track.
    bool fillMusic(std::size_t needed) {
        while (needed >= m_musicBufStart + m_musicBuf.size() / 2) {
            // keep only the last frame (needed for interpolation), then decode more
            if (m_musicBuf.size() >= 2) {
                const std::size_t total = m_musicBuf.size() / 2;
                m_musicBufStart += total - 1;
                m_musicBuf.erase(m_musicBuf.begin(), m_musicBuf.end() - 2);
            }
            std::int16_t chunk[4096];
            const std::size_t got = m_music->read(chunk, 2048);
            if (got == 0)
                return false;
            m_musicBuf.insert(m_musicBuf.end(), chunk, chunk + got * 2);
        }
        return true;
    }

    void mixMusic(std::size_t frames) {
        if (!m_music || m_musicVolume <= 0.0f)
            return;
        const float gain = m_musicVolume / 32768.0f;
        for (std::size_t i = 0; i < frames; ++i) {
            const auto i0 = static_cast<std::size_t>(m_musicPos);
            if (!fillMusic(i0)) {
                // track finished: the next one by the original's rule (0x409230, MM_MCINOTIFY):
                // next = cur==t1 ? t2 : cur==t2 ? t3 : cur==t3 ? t4 : t1 (first match), so
                // 8,9,8,9 alternates and 4,4,2,3 repeats track 4 (docs/re/runtime.md §9).
                openTrack(nextTrack(m_currentTrack));
                if (!m_music)
                    return;
                --i;
                continue;
            }
            // The last frame of a track has no successor: hold it instead of skipping it.
            const bool hasNext = fillMusic(i0 + 1);
            const float t = static_cast<float>(m_musicPos - i0);
            const std::size_t a = (i0 - m_musicBufStart) * 2;
            const std::size_t b = hasNext ? a + 2 : a;
            m_mix[2 * i] += (m_musicBuf[a] + (m_musicBuf[b] - m_musicBuf[a]) * t) * gain;
            m_mix[2 * i + 1] += (m_musicBuf[a + 1] + (m_musicBuf[b + 1] - m_musicBuf[a + 1]) * t) * gain;
            m_musicPos += m_musicStep;
        }
    }

    int m_outRate;
    float m_musicVolume = 0.8f, m_sfxVolume = 1.0f;
    std::vector<Voice> m_voices;
    std::uint32_t m_nextVoiceId = 0;
    std::vector<float> m_mix;

    std::vector<std::filesystem::path> m_musicDirs;
    std::array<int, 4> m_playlist{};
    int m_currentTrack = 0;
    std::unique_ptr<MusicStream> m_music;
    double m_musicStep = 1.0, m_musicPos = 0.0;
    std::vector<short> m_musicBuf;   // decoded window, interleaved stereo
    std::size_t m_musicBufStart = 0; // source frame index of m_musicBuf[0]

    // --- streaming source (movie soundtrack) ---
public:
    // Attaches a stream (not owned; nullptr detaches), mixed at `volume` from its start.
    void setStream(AudioStream* stream, float volume = 1.0f) {
        m_stream = stream;
        m_streamVolume = std::clamp(volume, 0.0f, 1.0f);
        m_streamStep = stream ? static_cast<double>(stream->sampleRate()) / m_outRate : 1.0;
        m_streamPos = 0.0;
        m_streamBuf.clear();
        m_streamBufStart = 0;
        m_streamEof = m_streamDone = false;
    }
    // True while a stream is attached and not fully played.
    [[nodiscard]] bool streamPlaying() const noexcept { return m_stream && !m_streamDone; }
    // Source frame (at the stream's rate) of the next output sample: the playback clock.
    [[nodiscard]] double streamPosition() const noexcept { return m_streamPos; }

private:
    // Same windowed reading as the music: keeps the source frames around m_streamPos.
    bool fillStream(std::size_t needed) {
        while (needed >= m_streamBufStart + m_streamBuf.size() / 2) {
            if (m_streamEof)
                return false;
            if (m_streamBuf.size() >= 2) {
                m_streamBufStart += m_streamBuf.size() / 2 - 1;
                m_streamBuf.erase(m_streamBuf.begin(), m_streamBuf.end() - 2);
            }
            std::int16_t chunk[2 * 1024];
            const std::size_t got = m_stream->read(chunk, 1024);
            m_streamEof = got < 1024;
            m_streamBuf.insert(m_streamBuf.end(), chunk, chunk + got * 2);
        }
        return true;
    }

    void mixStream(std::size_t frames) {
        if (!m_stream || m_streamDone)
            return;
        const float gain = m_streamVolume / 32768.0f;
        for (std::size_t i = 0; i < frames; ++i) {
            const auto i0 = static_cast<std::size_t>(m_streamPos);
            if (!fillStream(i0)) {
                m_streamDone = true;
                return;
            }
            const bool hasNext = fillStream(i0 + 1);
            const float t = static_cast<float>(m_streamPos - i0);
            const std::size_t a = (i0 - m_streamBufStart) * 2;
            const std::size_t b = hasNext ? a + 2 : a;
            m_mix[2 * i] += (m_streamBuf[a] + (m_streamBuf[b] - m_streamBuf[a]) * t) * gain;
            m_mix[2 * i + 1] += (m_streamBuf[a + 1] + (m_streamBuf[b + 1] - m_streamBuf[a + 1]) * t) * gain;
            m_streamPos += m_streamStep;
        }
    }

    AudioStream* m_stream = nullptr;
    float m_streamVolume = 1.0f;
    double m_streamStep = 1.0, m_streamPos = 0.0;
    std::vector<std::int16_t> m_streamBuf; // source window, interleaved stereo
    std::size_t m_streamBufStart = 0;      // source frame index of m_streamBuf[0]
    bool m_streamEof = false, m_streamDone = false;
};

// The viewer milestone has no script interpreter yet: pull the level's playlist
// straight out of its script (maps/levels/NN.INI), e.g. PlayMusic(music1=8,music2=9,...).
inline std::optional<std::array<int, 4>> findLevelPlaylist(const std::filesystem::path& script) {
    std::ifstream in{script, std::ios::binary};
    if (!in)
        return std::nullopt;
    const std::string text{std::istreambuf_iterator<char>{in}, {}};
    const auto pos = text.find("PlayMusic(");
    if (pos == std::string::npos)
        return std::nullopt;
    const auto end = text.find(')', pos);
    std::array<int, 4> list{};
    std::size_t n = 0;
    for (std::size_t i = pos + 9; i < end && n < 4; ++i) {
        if (text[i] == '=' || text[i] == '(' || text[i] == ',') {
            std::size_t j = i + 1;
            while (j < end && text[j] == ' ') ++j;
            if (j < end && std::isdigit(static_cast<unsigned char>(text[j])))
                list[n++] = std::atoi(text.c_str() + j);
        }
    }
    if (n == 0)
        return std::nullopt;
    for (; n < 4; ++n) list[n] = list[0];
    return list;
}

} // namespace gromada
