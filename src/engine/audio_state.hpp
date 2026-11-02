#pragma once
// Save states of the mixer (issue #14, docs/savestates.md): the CD playlist, the current track
// and the exact playback position (source frame, fractional), plus the sound-effect voices
// (which sound, position, gains, voice id, looping) and the position of the streaming source
// (movie soundtrack, attached again by the session). Volumes are set again by the session.
//
// Restoring the music position: if the same track is open and its decoded window still holds
// (or has not yet reached) the position, only the position moves -- the common case of rewind
// and run-ahead, which go back a few frames; otherwise the track is reopened and the decoder
// seeks (FLAC and WAV are sample exact; Vorbis is checked by tests), falling back to decoding
// from the start. Without the music files the music simply stays silent, as before the load.
// Header-only.

#include "core/state_io.hpp"
#include "engine/audio.hpp"

#include <functional>

namespace gromada {

struct MixerState {
    std::array<int, 4> playlist{};
    int currentTrack = 0;
    bool musicOpen = false;
    double musicPos = 0;
    struct Voice {
        const SoundData* sound = nullptr;
        double pos = 0;
        float gainL = 0, gainR = 0;
        std::uint32_t id = 0;
        bool loop = false, paused = false;
    };
    std::vector<Voice> voices;
    std::uint32_t nextVoiceId = 0;
    // streaming source (movie soundtrack): attached by the session before apply()
    bool stream = false, streamDone = false;
    float streamVolume = 1.0f;
    double streamPos = 0;
};

struct SaveStateAccess {
    static constexpr std::uint32_t Tag = state::fourcc("MIX1");
    // Sound reference used in the state (the session's numbering; < 0 = unknown).
    using SoundRef = std::function<std::int32_t(const SoundData*)>;
    using SoundLookup = std::function<const SoundData*(std::int32_t)>;

    static void save(state::Writer& w, const Mixer& m, const SoundRef& ref) {
        const std::size_t chunk = w.beginChunk(Tag);
        for (const int t : m.m_playlist) w.i32(t);
        w.i32(m.m_currentTrack);
        w.boolean(static_cast<bool>(m.m_music));
        w.f64(m.m_musicPos);
        std::size_t n = 0;
        for (const auto& v : m.m_voices) n += ref(v.sound) >= 0 ? 1 : 0;
        w.count(n);
        for (const auto& v : m.m_voices) {
            const std::int32_t id = ref(v.sound);
            if (id < 0) continue; // a sound the session does not know: dropped
            w.i32(id);
            w.f64(v.pos);
            w.f32(v.gainL);
            w.f32(v.gainR);
            w.u32(v.id);
            w.boolean(v.loop);
            w.boolean(v.paused);
        }
        w.u32(m.m_nextVoiceId);
        w.boolean(m.m_stream != nullptr);
        w.boolean(m.m_streamDone);
        w.f32(m.m_streamVolume);
        w.f64(m.m_streamPos);
        w.endChunk(chunk);
    }

    static MixerState decode(state::Reader& outer, const SoundLookup& lookup) {
        state::Reader r = outer.chunk(Tag);
        MixerState s;
        for (int& t : s.playlist) t = r.i32();
        s.currentTrack = r.i32();
        s.musicOpen = r.boolean();
        s.musicPos = r.finite64();
        if (s.musicPos < 0 || s.musicPos > 1e12) throw state::Error("save state: music position out of range");
        const std::size_t n = r.count(26, Mixer::MaxVoices);
        for (std::size_t i = 0; i < n; ++i) {
            MixerState::Voice v;
            const std::int32_t id = r.i32();
            v.sound = lookup(id);
            if (!v.sound) throw state::Error("save state: unknown sound in a voice");
            v.pos = r.finite64();
            if (v.pos < 0) throw state::Error("save state: bad voice position");
            v.gainL = r.finite32();
            v.gainR = r.finite32();
            v.id = r.u32();
            v.loop = r.boolean();
            v.paused = r.boolean();
            const auto frames = std::visit([](const auto& d) { return d.size(); }, v.sound->waveData) /
                                std::max<std::uint16_t>(v.sound->numChannels, 1);
            if (frames == 0 || (!v.loop && v.pos >= static_cast<double>(frames)) || v.pos > 1e12)
                throw state::Error("save state: voice past the end of its sound");
            if (v.id == 0 || std::ranges::any_of(s.voices, [&](const MixerState::Voice& o) { return o.id == v.id; }))
                throw state::Error("save state: bad voice id");
            s.voices.push_back(v);
        }
        s.nextVoiceId = r.u32();
        s.stream = r.boolean();
        s.streamDone = r.boolean();
        s.streamVolume = r.finite32();
        s.streamPos = r.finite64();
        if (s.streamPos < 0 || s.streamPos > 1e12) throw state::Error("save state: stream position out of range");
        r.expectEnd("mixer");
        return s;
    }

#if defined(__x86_64__) && defined(_LIBCPP_VERSION) && !defined(GROMADA_NO_SAVESTATE_TRIPWIRE)
    // A new field of a voice must be saved too (see game/savestate_session.hpp, tripwire).
    static_assert(sizeof(Mixer::Voice) == 48, "save states: Mixer::Voice changed size; serialize the new field (docs/savestates.md)");
#endif

    static void apply(Mixer& m, MixerState&& s) {
        m.m_voices.clear();
        for (const auto& v : s.voices) {
            const auto frames = std::visit([](const auto& d) { return d.size(); }, v.sound->waveData) /
                                std::max<std::uint16_t>(v.sound->numChannels, 1);
            m.m_voices.push_back(Mixer::Voice{
                .sound = v.sound,
                .frames = frames,
                .step = static_cast<double>(v.sound->sampleRate) / m.m_outRate,
                .pos = v.pos,
                .gainL = v.gainL,
                .gainR = v.gainR,
                .id = v.id,
                .loop = v.loop,
                .paused = v.paused,
            });
        }
        m.m_nextVoiceId = s.nextVoiceId;
        if (s.stream && m.m_stream) {
            // Same rule as the music: inside or ahead of the window only the position moves (the
            // source is read on demand), behind it the stream seeks.
            const auto target = static_cast<std::size_t>(s.streamPos);
            if (target < m.m_streamBufStart) {
                if (m.m_stream->seek(target)) {
                    m.m_streamBuf.clear();
                    m.m_streamBufStart = target;
                    m.m_streamEof = false;
                    m.m_streamPos = s.streamPos;
                }
            } else {
                m.m_streamPos = s.streamPos;
            }
            m.m_streamDone = s.streamDone;
            m.m_streamVolume = s.streamVolume;
        }
        m.m_playlist = s.playlist;
        if (!s.musicOpen) {
            m.closeMusic();
            m.m_currentTrack = s.currentTrack;
            return;
        }
        const auto target = static_cast<std::size_t>(s.musicPos);
        if (m.m_music && m.m_currentTrack == s.currentTrack && target >= m.m_musicBufStart) {
            m.m_musicPos = s.musicPos; // inside or ahead of the decoded window: decoded on demand
            return;
        }
        m.openTrack(s.currentTrack);
        if (!m.m_music) {
            m.m_currentTrack = s.currentTrack;
            return;
        }
        if (target > 0 && !m.m_music->seek(target)) {
            // No seek: decode from the start up to the position.
            std::int16_t chunk[4096];
            std::size_t at = 0;
            while (at < target) {
                const std::size_t got = m.m_music->read(chunk, std::min<std::size_t>(2048, target - at));
                if (!got) break;
                at += got;
            }
        }
        m.m_musicBuf.clear();
        m.m_musicBufStart = target;
        m.m_musicPos = s.musicPos;
    }
};

} // namespace gromada
