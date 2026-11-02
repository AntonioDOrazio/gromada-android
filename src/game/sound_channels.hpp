#pragma once
// The original's sound-effect system (gromada.exe, sound object at 0x42e0c8, docs/re/runtime.md
// §9.1), ported operation by operation and verified against the original run in an emulator
// with a fake DirectSound (tests/sound_oracle_test.cpp, tools/oracle/gen_sound.py):
//  - Queue   0x408dd0  PlaySFX/PlayPositional: up to 32 pending requests per frame; equal
//                      requests of a non-looping sound merge (volumes add, pan is averaged).
//  - Alloc   0x408910  channel choice: 8 DirectSound buffers; a sound started less than
//                      50 ms ago on a channel is not started again.
//  - Tact    0x408f20  once per frame: free finished channels, keep looping sounds alive,
//                      stop looping sounds not requested for 500 ms, drop the weakest requests
//                      when there are more than free channels, start the rest.
//  - StopSFX 0x409550, StopSFX(n) 0x408d80, PlayWAV 0x408c50 (channel part).
// Units are DirectSound's: pan and attenuation in 1/100 dB (pan > 0 attenuates the left
// channel, attenuation <= 0). Kind (type byte) 0 marks looping sounds (vehicle engines...), 100
// non-positional ones. Effect N = soundCount() is the PlayWAV slot (always looping).
//
// Playback goes through SoundSink, a DirectSound device seen per channel (one buffer per
// channel), so the logic is testable without audio and comparable with the original call by call.

#include "core/std.hpp"
#include "libgromada/resources_sound.hpp"

namespace gromada::game {

// Effect number -> sound, as the original's descriptor table (0x408730 loads fw.res sound i
// into descriptor i, N = number of fw.res sounds): nsfx 1..N-1 are the fw.res sounds (sound 0
// exists but 0x408dd0 rejects nsfx 0), nsfx N is the PlayWAV slot (`wav`, nullptr = empty).
// Verified on the real fw.res against the table built by the original (sound_oracle_test).
inline const SoundData* effectSound(std::span<const SoundData> sounds, const SoundData* wav, int nsfx) {
    const int n = static_cast<int>(sounds.size());
    if (nsfx >= 1 && nsfx < n)
        return &sounds[static_cast<std::size_t>(nsfx)];
    return nsfx == n ? wav : nullptr;
}

// The IDirectSoundBuffer operations of the original, one buffer per channel (0..7).
class SoundSink {
public:
    virtual ~SoundSink() = default;
    // CreateSoundBuffer + fill with `sound` (source < 0) or DuplicateSoundBuffer of channel
    // `source`'s buffer (same data). The new buffer is stopped at position 0. false = failed.
    virtual bool load(int channel, int nsfx, const SoundData& sound, int source) = 0;
    virtual void release(int channel) = 0;              // Release (stops the sound)
    virtual bool playing(int channel) = 0;              // GetStatus & DSBSTATUS_PLAYING
    virtual void setPan(int channel, int pan) = 0;      // SetPan, 1/100 dB, -10000..10000
    virtual void setVolume(int channel, int attenuation) = 0; // SetVolume, 1/100 dB (<= 0 valid)
    // Play from the current position (DSBPLAY_LOOPING when `loop`): a buffer stopped by stop()
    // resumes where it was, one that played to its end starts again from the beginning.
    virtual void play(int channel, bool loop) = 0;
    virtual void stop(int channel) = 0;                 // Stop: the position is kept
};

class SoundChannels {
public:
    friend struct SaveStateAccess; // save states (game/savestate.hpp)
    static constexpr int QueueSize = 32;
    static constexpr int ChannelCount = 8;
    static constexpr int MinAttenuation = -2000;   // quieter requests are ignored (0x408e11)
    static constexpr std::uint32_t RestartGuardMs = 50;  // 0x4089a1
    static constexpr std::uint32_t LoopKeepAliveMs = 500; // 0x409052
    static constexpr int LoopingKind = 0, NonPositionalKind = 100;

    // `sound(nsfx)` resolves effect numbers (index of the fw.res sound; N = the PlayWAV slot,
    // nullptr while no WAV is loaded there); `soundCount` = N.
    using SoundLookup = std::function<const SoundData*(int nsfx)>;

    SoundChannels(SoundLookup lookup, std::function<int()> soundCount)
        : m_lookup{std::move(lookup)}, m_count{std::move(soundCount)} {
        clearQueue();
    }

    // Diagnostics (tools/game_trace, issue #13): every request() as called (the channel
    // operations are seen by the SoundSink).
    std::function<void(int nsfx, int pan, int attenuation)> requestObserver;

    // 0x408dd0. nsfx outside 1..N (or N without a WAV) is ignored, attenuation below -2000
    // too; pan clamped to ±10000; non-positional sounds get attenuation 0.
    void request(int nsfx, int pan = 0, int attenuation = 0) {
        if (requestObserver)
            requestObserver(nsfx, pan, attenuation);
        const int n = m_count();
        if (nsfx <= 0 || nsfx > n || (nsfx == n && !m_lookup(n)) || attenuation < MinAttenuation)
            return;
        pan = std::clamp(pan, -10000, 10000);
        const int kind = kindOf(nsfx);
        if (kind == NonPositionalKind)
            attenuation = 0;
        if (kind != LoopingKind) { // non-looping: merge with an equal request of this frame
            for (auto& q : m_queue) {
                if (q.nsfx != nsfx)
                    continue;
                // 32-bit arithmetic of the original (positive attenuations are not clamped)
                const std::int32_t den = add32(add32(q.attenuation, attenuation), 20000);
                if (den != 0) {
                    const std::int32_t num = add32(mul32(add32(q.attenuation, 10000), q.pan),
                                                   mul32(add32(attenuation, 10000), pan));
                    if (num != std::numeric_limits<std::int32_t>::min() || den != -1) // #DE in the original
                        q.pan = num / den;
                }
                q.attenuation = add32(q.attenuation, add32(attenuation, 10000));
                if (q.attenuation > 0)
                    q.attenuation = 0;
                return;
            }
        }
        for (auto& q : m_queue)
            if (q.nsfx < 0) {
                q = {nsfx, pan, attenuation};
                return;
            }
        // queue full: the request is lost, as in the original
    }

    void clearQueue() {
        for (auto& q : m_queue) q.nsfx = -1;
    }

    // StopSFX() (0x409550): every playing channel stops (position kept: Play resumes it), except
    // the PlayWAV slot's. The queue of this frame is kept; the channels keep their sound and
    // start time (50 ms guard).
    void stopAll(SoundSink& sink) {
        const int n = m_count();
        for (int i = 0; i < ChannelCount; ++i) {
            auto& c = m_channels[i];
            if (c.active && c.loaded && c.nsfx != n) {
                sink.stop(i);
                c.active = false;
            }
        }
    }

    // StopSFX(nsfx) (0x408d80) and StopMusic() (the PlayWAV slot): the playing channels of
    // nsfx are released (0x408bf0: no buffer, no sound, no time).
    void stop(SoundSink& sink, int nsfx) {
        for (int i = 0; i < ChannelCount; ++i)
            if (m_channels[i].active && m_channels[i].loaded && m_channels[i].nsfx == nsfx)
                release(sink, i);
    }

    // PlayWAV(file) (0x408c50): if a channel is playing the same file nothing happens (returns
    // N); otherwise the channels holding slot N are released and `load()` replaces the WAV of
    // slot N (the old one is freed first: on failure the slot is empty). NotFound returns 0;
    // otherwise slot N (looping) is queued at full volume and N is returned.
    enum class WavLoad { NotFound, Invalid, Loaded };
    template <class Load>
    int playWav(SoundSink& sink, std::string_view file, Load&& load) {
        const int n = m_count();
        for (int i = 0; i < ChannelCount; ++i) {
            auto& c = m_channels[i];
            if (c.nsfx != n)
                continue;
            if (c.active && c.loaded && file == m_wavName)
                return n;
            release(sink, i);
        }
        if (load() == WavLoad::NotFound)
            return 0;
        request(n, 0, 0);
        m_wavName = file;
        return n;
    }

    // 0x408f20, once per frame at the end of the frame; `clock` = game clock in ms.
    void tact(SoundSink& sink, std::uint32_t clock) {
        // 1. channels whose sound has finished become inactive; count the free (inactive) ones
        int freeChannels = 0;
        for (int i = 0; i < ChannelCount; ++i) {
            auto& c = m_channels[i];
            if (!c.loaded || !c.active || !sink.playing(i)) {
                if (c.loaded && c.active)
                    c.active = false;
                ++freeChannels;
            }
        }
        // 2. a looping sound already active on a channel is only re-panned and kept alive
        int pending = 0;
        for (auto& q : m_queue) {
            if (q.nsfx < 0)
                continue;
            int i = ChannelCount;
            if (kindOf(q.nsfx) == LoopingKind)
                for (i = 0; i < ChannelCount; ++i)
                    if (m_channels[i].nsfx == q.nsfx && m_channels[i].active)
                        break;
            if (i == ChannelCount) {
                ++pending;
                continue;
            }
            q.nsfx = -1;
            sink.setPan(i, q.pan);
            sink.setVolume(i, q.attenuation);
            m_channels[i].time = clock;
        }
        // 3. looping sounds not requested for 500 ms stop, active or not (the PlayWAV slot is
        //    exempt); the channel is not counted as free before the next frame
        const int n = m_count();
        for (int i = 0; i < ChannelCount; ++i) {
            auto& c = m_channels[i];
            if (c.nsfx < 0 || !c.loaded || kindOf(c.nsfx) != LoopingKind || c.nsfx == n)
                continue;
            if (clock - c.time > LoopKeepAliveMs) {
                c.active = false;
                sink.stop(i);
            }
        }
        // 4. more requests than free channels: drop the weakest ones (lowest attenuation, the
        //    last one on a tie). The search starts from entry 0, compared with attenuation 0 when
        //    free: with only positive attenuations pending nothing is dropped.
        for (int k = pending - freeChannels; k > 0; --k) {
            if (m_queue[0].nsfx < 0)
                m_queue[0].attenuation = 0;
            int weakest = 0;
            for (int j = 0; j < QueueSize; ++j)
                if (m_queue[j].nsfx >= 0 && m_queue[j].attenuation <= m_queue[weakest].attenuation)
                    weakest = j;
            m_queue[weakest].nsfx = -1;
        }
        // 5. start the remaining requests
        for (auto& q : m_queue) {
            if (q.nsfx < 0)
                continue;
            if (const int i = allocate(sink, q.nsfx, clock); i >= 0) {
                sink.setPan(i, q.pan);
                sink.setVolume(i, q.attenuation);
                sink.play(i, kindOf(q.nsfx) == LoopingKind);
                m_channels[i].active = true;
                m_channels[i].time = clock;
            }
            q.nsfx = -1;
        }
    }

    // ---- state (tests) ----
    struct Channel {
        bool active = false;  // playing (or looping)
        bool loaded = false;  // holds a DirectSound buffer
        int nsfx = 0;         // sound whose buffer the channel holds (0 before the first use, as
                              // the original's .bss; -1 after a release)
        std::uint32_t time = 0; // start or last keep-alive, game clock
    };
    struct Request {
        int nsfx = -1;
        int pan = 0;          // 1/100 dB
        int attenuation = 0;  // 1/100 dB
    };
    [[nodiscard]] std::span<const Channel, ChannelCount> channels() const { return m_channels; }
    [[nodiscard]] std::span<const Request, QueueSize> queue() const { return m_queue; }
    [[nodiscard]] int activeChannels() const {
        return static_cast<int>(std::ranges::count_if(m_channels, [](const Channel& c) { return c.active; }));
    }
    [[nodiscard]] int pendingRequests() const {
        return static_cast<int>(std::ranges::count_if(m_queue, [](const Request& q) { return q.nsfx >= 0; }));
    }

    // DirectSound units -> gromada::Mixer gains: volume 0..1, pan -1..1 (Mixer convention).
    static float volumeOf(int attenuation) { return std::pow(10.0f, static_cast<float>(attenuation) / 2000.0f); }
    // DirectSound pan: positive values attenuate the left channel, negative the right.
    static float panOf(int pan) {
        const float far = 1.0f - std::pow(10.0f, -static_cast<float>(std::abs(pan)) / 2000.0f);
        return pan < 0 ? -far : far;
    }

private:
    static std::int32_t add32(std::int32_t a, std::int32_t b) {
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
    }
    static std::int32_t mul32(std::int32_t a, std::int32_t b) {
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) * static_cast<std::uint32_t>(b));
    }

    // Type byte of the descriptor; the PlayWAV slot is looping (0x408c50 sets 0).
    int kindOf(int nsfx) const {
        if (nsfx == m_count())
            return LoopingKind;
        const SoundData* s = m_lookup(nsfx);
        return s ? s->kind : 1;
    }

    // 0x408bf0
    void release(SoundSink& sink, int i) {
        if (m_channels[i].loaded)
            sink.release(i);
        m_channels[i] = {false, false, -1, 0};
    }

    // 0x408910: the channel that will play nsfx, -1 = none.
    int allocate(SoundSink& sink, int nsfx, std::uint32_t clock) {
        const int n = m_count();
        if (nsfx < 0 || nsfx > n)
            return -1;
        const SoundData* sound = m_lookup(nsfx);
        if (!sound)
            return -1; // PlayWAV slot without a WAV: "Can't load wav file for play"
        for (int i = 0; i < ChannelCount; ++i) {
            const auto& c = m_channels[i];
            if (c.nsfx != nsfx)
                continue;
            if (clock - c.time <= RestartGuardMs)
                return -1;   // started less than 50 ms ago: not again (active or not)
            if (!c.active)
                return i;    // reuse the channel that already holds this sound (no reload)
        }
        int i = 0;
        while (i < ChannelCount && m_channels[i].loaded)
            ++i;             // a channel without a buffer
        if (i == ChannelCount) {
            for (i = 0; i < ChannelCount && m_channels[i].active; ++i) {}
            if (i == ChannelCount)
                return -1;   // all 8 busy
            release(sink, i); // free a finished channel
        }
        auto& c = m_channels[i];
        c.nsfx = nsfx;
        // DuplicateSoundBuffer of the first channel holding the same sound, else a new buffer
        int source = -1;
        for (int j = 0; j < ChannelCount && source < 0; ++j)
            if (j != i && m_channels[j].nsfx == nsfx && m_channels[j].loaded)
                source = j;
        if (!sink.load(i, nsfx, *sound, source)) {
            c = {false, false, -1, 0}; // the original would then crash on the missing buffer
            return -1;
        }
        c.loaded = true;
        return i;
    }

    SoundLookup m_lookup;
    std::function<int()> m_count;
    std::array<Request, QueueSize> m_queue{};
    std::array<Channel, ChannelCount> m_channels{};
    std::string m_wavName; // 0x42e390: file of the last PlayWAV that opened
};

} // namespace gromada::game
