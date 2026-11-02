#pragma once
// The original's sound-effect scheduler (gromada.exe, object at the Sound instance):
//  - Queue  0x408dd0  PlaySFX/PlayPositional: up to 32 pending requests per frame; equal
//                     requests of a non-looping sound merge (volumes add, pan is averaged).
//  - Alloc  0x408910  channel choice: 8 DirectSound channels; a sound started less than
//                     50 ms ago on a channel is not started again.
//  - Tact   0x408f20  once per frame: free finished channels, keep looping sounds alive,
//                     stop looping sounds not requested for 500 ms, drop the weakest requests
//                     when there are more than free channels, start the rest.
// Units are DirectSound's: pan and attenuation in 1/100 dB (pan > 0 attenuates the left
// channel, attenuation <= 0). SoundData::kind 0 marks looping sounds (vehicle engines...),
// 100 non-positional ones.
//
// Playback goes through a small sink interface so the logic is testable without audio.

#include "core/std.hpp"
#include "libgromada/resources_sound.hpp"

namespace gromada::game {

class SoundSink {
public:
    virtual ~SoundSink() = default;
    // volume 0..1, pan -1..1 (gromada::Mixer convention). Returns a voice id, 0 = failed.
    virtual std::uint32_t play(int nsfx, const SoundData& sound, float volume, float pan, bool loop) = 0;
    virtual bool playing(std::uint32_t voice) const = 0;
    virtual void update(std::uint32_t voice, float volume, float pan) = 0;
    virtual void stop(std::uint32_t voice) = 0;
};

class SoundChannels {
public:
    friend struct SaveStateAccess; // save states (game/savestate.hpp)
    static constexpr int QueueSize = 32;
    static constexpr int ChannelCount = 8;
    static constexpr int MinAttenuation = -2000;   // quieter requests are ignored (0x408e11)
    static constexpr std::uint32_t RestartGuardMs = 50;  // 0x4089a1
    static constexpr std::uint32_t LoopKeepAliveMs = 500; // 0x409052

    // `sound(nsfx)` resolves 1-based effect numbers (fw.res + PlayWAV); `soundCount` is the
    // highest valid number (the original compares nsfx with it, see tact()).
    using SoundLookup = std::function<const SoundData*(int nsfx)>;

    SoundChannels(SoundLookup lookup, std::function<int()> soundCount)
        : m_lookup{std::move(lookup)}, m_count{std::move(soundCount)} {}

    // 0x408dd0. pan clamped to ±10000; attenuation below -2000 is dropped.
    void request(int nsfx, int pan = 0, int attenuation = 0) {
        const SoundData* s = m_lookup(nsfx);
        if (!s || attenuation < MinAttenuation)
            return;
        pan = std::clamp(pan, -10000, 10000);
        if (s->kind == 100)
            attenuation = 0;
        if (s->kind != 0) { // non-looping: merge with an equal request of this frame
            for (auto& q : m_queue) {
                if (q.nsfx != nsfx)
                    continue;
                const int den = q.attenuation + attenuation + 20000;
                if (den != 0)
                    q.pan = static_cast<int>((static_cast<std::int64_t>(q.attenuation + 10000) * q.pan +
                                              static_cast<std::int64_t>(attenuation + 10000) * pan) / den);
                q.attenuation += attenuation + 10000;
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

    // StopSFX() (0x409550): every playing channel stops, except the one playing the last
    // effect number (the PlayWAV slot). The queue of this frame is kept; the channels keep
    // their sound and start time (50 ms guard).
    void stopAll(SoundSink& sink) {
        const int last = m_count();
        for (auto& c : m_channels)
            if (c.active && c.nsfx != last) {
                sink.stop(c.voice);
                c.active = false;
                c.voice = 0;
            }
    }

    // StopSFX(nsfx) (0x408d80, unreachable from the scripts) and StopMusic() (the PlayWAV
    // slot): the channels playing nsfx stop and are released (0x408bf0: no sound, no time).
    void stop(SoundSink& sink, int nsfx) {
        for (auto& c : m_channels)
            if (c.active && c.nsfx == nsfx) {
                sink.stop(c.voice);
                c = {};
            }
    }

    // 0x408f20, once per frame at the end of the frame; `clock` = game clock in ms.
    void tact(SoundSink& sink, std::uint32_t clock) {
        // 1. channels whose (non-looping) sound has finished become free
        int freeChannels = 0;
        for (auto& c : m_channels) {
            if (c.active && c.voice && !sink.playing(c.voice)) {
                c.active = false;
                c.voice = 0;
            }
            if (!c.active)
                ++freeChannels;
        }
        // 2. a looping sound that is already playing is only re-panned and kept alive
        int pending = 0;
        for (auto& q : m_queue) {
            if (q.nsfx < 0)
                continue;
            const SoundData* s = m_lookup(q.nsfx);
            if (!s) {
                q.nsfx = -1;
                continue;
            }
            if (s->kind == 0) {
                if (auto* c = findActive(q.nsfx)) {
                    sink.update(c->voice, volumeOf(q.attenuation), panOf(q.pan));
                    c->time = clock;
                    q.nsfx = -1;
                    continue;
                }
            }
            ++pending;
        }
        // 3. looping sounds not requested for 500 ms stop (the last effect number is exempt);
        //    the channel is not counted as free before the next frame
        for (auto& c : m_channels) {
            if (!c.active)
                continue;
            const SoundData* s = m_lookup(c.nsfx);
            if (!s || s->kind != 0 || c.nsfx == m_count())
                continue;
            if (clock - c.time > LoopKeepAliveMs) {
                sink.stop(c.voice);
                c.active = false;
                c.voice = 0;
            }
        }
        // 4. more requests than free channels: drop the weakest ones
        for (; pending > freeChannels; --pending) {
            Request* weakest = nullptr;
            for (auto& q : m_queue)
                if (q.nsfx >= 0 && (!weakest || q.attenuation <= weakest->attenuation))
                    weakest = &q;
            if (!weakest)
                break;
            weakest->nsfx = -1;
        }
        // 5. start the remaining requests
        for (auto& q : m_queue) {
            if (q.nsfx < 0)
                continue;
            if (Channel* c = allocate(q.nsfx, clock)) {
                const SoundData* s = m_lookup(q.nsfx);
                if (c->voice)
                    sink.stop(c->voice);
                c->voice = sink.play(q.nsfx, *s, volumeOf(q.attenuation), panOf(q.pan), s->kind == 0);
                c->nsfx = q.nsfx;
                c->active = c->voice != 0;
                c->time = clock;
            }
            q.nsfx = -1;
        }
    }

    [[nodiscard]] int activeChannels() const {
        return static_cast<int>(std::ranges::count_if(m_channels, [](const Channel& c) { return c.active; }));
    }
    [[nodiscard]] int pendingRequests() const {
        return static_cast<int>(std::ranges::count_if(m_queue, [](const Request& q) { return q.nsfx >= 0; }));
    }

private:
    struct Request {
        int nsfx = -1;
        int pan = 0;          // 1/100 dB
        int attenuation = 0;  // 1/100 dB, <= 0
    };
    struct Channel {
        int nsfx = -1;        // sound whose buffer the channel holds
        bool active = false;  // playing (or looping)
        std::uint32_t voice = 0;
        std::uint32_t time = 0; // start or last keep-alive, game clock
    };

    static float volumeOf(int attenuation) { return std::pow(10.0f, static_cast<float>(attenuation) / 2000.0f); }
    // DirectSound pan: positive values attenuate the left channel, negative the right.
    static float panOf(int pan) {
        const float far = 1.0f - std::pow(10.0f, -static_cast<float>(std::abs(pan)) / 2000.0f);
        return pan < 0 ? -far : far;
    }

    Channel* findActive(int nsfx) {
        for (auto& c : m_channels)
            if (c.active && c.nsfx == nsfx)
                return &c;
        return nullptr;
    }

    // 0x408910
    Channel* allocate(int nsfx, std::uint32_t clock) {
        for (auto& c : m_channels) {
            if (c.nsfx != nsfx)
                continue;
            if (clock - c.time <= RestartGuardMs)
                return nullptr;   // started less than 50 ms ago: not again
            if (!c.active)
                return &c;        // reuse the channel that already holds this sound
        }
        for (auto& c : m_channels)
            if (c.nsfx < 0)
                return &c;        // a channel that never held a sound
        for (auto& c : m_channels)
            if (!c.active)
                return &c;        // free a finished channel
        return nullptr;           // all 8 busy
    }

    SoundLookup m_lookup;
    std::function<int()> m_count;
    std::array<Request, QueueSize> m_queue{};
    std::array<Channel, ChannelCount> m_channels{};
};

} // namespace gromada::game
