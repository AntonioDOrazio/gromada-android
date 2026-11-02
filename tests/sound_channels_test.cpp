// The original's sound-effect channels (game/sound_channels.hpp, issue #17) with a mock sink,
// and the Mixer's looping voices. Synthetic sounds only (no game data).
#include "engine/audio.hpp"
#include "game/sound_channels.hpp"
#include "test.hpp"

#include <numbers>
#include <set>

using gromada::Mixer;
using gromada::game::SoundChannels;
using gromada::game::SoundSink;

namespace {

// A mono 16-bit WAV in memory -> SoundData (the only constructor reads a RIFF file).
SoundData makeSound(std::uint8_t kind, std::vector<std::int16_t> samples, std::uint32_t rate = 22050) {
    std::string bytes;
    auto u32 = [&](std::uint32_t v) { for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<char>(v >> (8 * i))); };
    auto u16 = [&](std::uint16_t v) { for (int i = 0; i < 2; ++i) bytes.push_back(static_cast<char>(v >> (8 * i))); };
    const auto dataSize = static_cast<std::uint32_t>(samples.size() * 2);
    bytes += "RIFF"; u32(36 + dataSize); bytes += "WAVE";
    bytes += "fmt "; u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
    bytes += "data"; u32(dataSize);
    for (auto s : samples) u16(static_cast<std::uint16_t>(s));
    std::istringstream in{bytes};
    BinaryStreamReader reader{in, bytes.size()};
    SoundData d{reader};
    d.kind = kind;
    return d;
}

struct Play {
    int nsfx;
    float volume, pan;
    bool loop;
    std::uint32_t voice;  // the channel's buffer (load() count)
    bool resumed = false; // Play on a buffer stopped mid-way (DirectSound keeps the position)
};

// A DirectSound buffer per channel: records the starts; buffers play until stop(), release()
// or finish().
struct MockSink final : SoundSink {
    struct Buffer {
        int nsfx = -1;
        std::uint32_t id = 0;
        int pan = 0, att = 0;
        bool playing = false, stopped = false;
    };
    std::array<Buffer, SoundChannels::ChannelCount> buf{};
    std::vector<Play> plays;
    std::vector<std::uint32_t> stops; // buffers stopped or released while playing
    std::set<std::uint32_t> live;
    int updates = 0, loads = 0, duplicates = 0;
    std::uint32_t next = 0;
    bool load(int ch, int nsfx, const SoundData&, int source) override {
        buf[ch] = {nsfx, ++next};
        ++loads;
        duplicates += source >= 0;
        return true;
    }
    void release(int ch) override { halt(ch); buf[ch] = {}; }
    bool playing(int ch) override { return buf[ch].playing; }
    void setPan(int ch, int pan) override { buf[ch].pan = pan; }
    void setVolume(int ch, int att) override {
        buf[ch].att = att;
        updates += buf[ch].playing;
    }
    void play(int ch, bool loop) override {
        auto& b = buf[ch];
        plays.push_back({b.nsfx, volumeOf(b.att), panOf(b.pan), loop, b.id, b.stopped});
        b.playing = true;
        b.stopped = false;
        live.insert(b.id);
    }
    void stop(int ch) override {
        if (buf[ch].playing) buf[ch].stopped = true;
        halt(ch);
    }
    void halt(int ch) {
        if (buf[ch].playing) stops.push_back(buf[ch].id);
        buf[ch].playing = false;
        live.erase(buf[ch].id);
    }
    void finish(std::uint32_t id) {
        for (auto& b : buf)
            if (b.id == id) b.playing = false;
        live.erase(id);
    }
    void finishAll() {
        for (auto& b : buf) b.playing = false;
        live.clear();
    }
    int started(int nsfx) const {
        return static_cast<int>(std::ranges::count_if(plays, [nsfx](const Play& p) { return p.nsfx == nsfx; }));
    }
    static float volumeOf(int att) { return SoundChannels::volumeOf(att); }
    static float panOf(int pan) { return SoundChannels::panOf(pan); }
};

// Effects 1..12: 1 = looping (kind 0), 2..11 = one-shot (kind 1), 12 = non-positional (100);
// 13 = the PlayWAV slot (N, looping), holding `wav` when set.
struct Bank {
    std::vector<SoundData> sounds; // sounds[0] = fw.res sound 0 (not playable)
    SoundData wavSound = makeSound(100, std::vector<std::int16_t>(100, 1000));
    const SoundData* wav = nullptr;
    Bank() {
        sounds.push_back(makeSound(1, std::vector<std::int16_t>(100, 1000)));
        sounds.push_back(makeSound(0, std::vector<std::int16_t>(100, 1000)));
        for (int i = 2; i <= 11; ++i) sounds.push_back(makeSound(1, std::vector<std::int16_t>(100, 1000)));
        sounds.push_back(makeSound(100, std::vector<std::int16_t>(100, 1000)));
    }
    int slot() const { return static_cast<int>(sounds.size()); }
    SoundChannels channels() {
        return SoundChannels{[this](int n) { return gromada::game::effectSound(sounds, wav, n); },
                             [this] { return slot(); }};
    }
};

bool near(float a, float b, float eps = 1e-4f) { return std::abs(a - b) <= eps; }
float volumeOf(int att) { return std::pow(10.0f, att / 2000.0f); }
float panOf(int pan) {
    const float far = 1.0f - std::pow(10.0f, -std::abs(pan) / 2000.0f);
    return pan < 0 ? -far : far;
}

// A looping engine requested every 50 ms (a vehicle tick at 60 fps) starts once, follows the
// latest pan/volume and stops 500 ms after the last request.
void testLoopKeptAlive() {
    Bank bank;
    auto ch = bank.channels();
    MockSink sink;
    std::uint32_t clock = 1000, lastRequest = 0;
    for (int frame = 0; frame < 120; ++frame, clock += 17) { // ~2 s at 60 fps
        if (frame % 3 == 0) {
            ch.request(1, 100 * frame, -10 * (frame % 7));
            lastRequest = clock;
        }
        ch.tact(sink, clock);
    }
    CHECK_EQ(sink.started(1), 1);
    CHECK(sink.plays.size() == 1 && sink.plays[0].loop);
    CHECK(sink.updates >= 38);
    CHECK(sink.stops.empty());
    CHECK_EQ(ch.activeChannels(), 1);
    // no more requests: still playing up to 500 ms, stopped after
    for (; clock - lastRequest <= SoundChannels::LoopKeepAliveMs; clock += 17) {
        ch.tact(sink, clock);
        CHECK(sink.stops.empty());
    }
    ch.tact(sink, clock);
    CHECK_EQ(sink.stops.size(), std::size_t{1});
    CHECK_EQ(ch.activeChannels(), 0);
    // requested again later: a new start (the channel that held it is reused)
    ch.request(1);
    ch.tact(sink, clock + 17);
    CHECK_EQ(sink.started(1), 2);
    CHECK_EQ(ch.activeChannels(), 1);
}

// Two requests of a looping sound in one frame are not merged, but only one voice starts
// (the second hits the 50 ms guard); the PlayWAV slot (N) loops and never times out.
void testLoopDuplicatesAndExempt() {
    Bank bank;
    bank.wav = &bank.wavSound;
    auto ch = bank.channels();
    MockSink sink;
    ch.request(1, -500, -100);
    ch.request(1, 500, -200);
    CHECK_EQ(ch.pendingRequests(), 2);
    ch.request(13);
    ch.tact(sink, 0);
    CHECK_EQ(sink.started(1), 1);
    CHECK_EQ(sink.started(13), 1);
    CHECK(sink.plays[1].loop); // kind 100 in the SoundData: the slot is looping anyway
    ch.tact(sink, 5000);
    CHECK_EQ(ch.activeChannels(), 1); // sound 1 stopped, N kept
    CHECK(sink.live.contains(sink.plays[1].voice));
}

// The same effect is not started again within 50 ms on any channel (0x408910).
void testRestartGuard() {
    Bank bank;
    auto ch = bank.channels();
    MockSink sink;
    ch.request(2);
    ch.tact(sink, 100);
    sink.finishAll(); // a very short sound: finished at once
    ch.request(2);
    ch.tact(sink, 140);
    CHECK_EQ(sink.started(2), 1);
    ch.request(2);
    ch.tact(sink, 150); // exactly 50 ms: still blocked
    CHECK_EQ(sink.started(2), 1);
    ch.request(2);
    ch.tact(sink, 151);
    CHECK_EQ(sink.started(2), 2);
    CHECK_EQ(ch.activeChannels(), 1); // the finished channel holding sound 2 was reused
    // still playing after 50 ms: a second, overlapping channel
    ch.request(2);
    ch.tact(sink, 300);
    CHECK_EQ(sink.started(2), 3);
    CHECK_EQ(ch.activeChannels(), 2);
}

// 8 channels; when more requests than free channels are pending the weakest are dropped.
void testChannelLimit() {
    Bank bank;
    auto ch = bank.channels();
    MockSink sink;
    for (int n = 2; n <= 11; ++n) // 10 one-shot sounds, attenuation 0, -100 .. -900
        ch.request(n, 0, -100 * (n - 2));
    ch.tact(sink, 0);
    CHECK_EQ(sink.plays.size(), std::size_t{8});
    CHECK_EQ(sink.started(10), 0); // -800
    CHECK_EQ(sink.started(11), 0); // -900
    for (int n = 2; n <= 9; ++n) CHECK_EQ(sink.started(n), 1);
    CHECK_EQ(ch.activeChannels(), SoundChannels::ChannelCount);
    // all busy: a new request is dropped, nothing is cut
    ch.request(10);
    ch.tact(sink, 100);
    CHECK_EQ(sink.plays.size(), std::size_t{8});
    CHECK(sink.stops.empty());
    // two channels finish: the two loudest of three requests play
    sink.finish(sink.plays[0].voice);
    sink.finish(sink.plays[1].voice);
    ch.request(10, 0, -1500);
    ch.request(11, 0, -300);
    ch.request(1, 0, -700); // looping, not playing: competes like the others
    ch.tact(sink, 200);
    CHECK_EQ(sink.plays.size(), std::size_t{10});
    CHECK_EQ(sink.started(10), 0);
    CHECK_EQ(sink.started(11), 1);
    CHECK_EQ(sink.started(1), 1);
}

// Equal requests of a one-shot sound in a frame merge (0x408dd0).
void testMerge() {
    Bank bank;
    auto ch = bank.channels();
    MockSink sink;
    ch.request(3, -1000, -500);
    ch.request(3, 2000, -300);
    CHECK_EQ(ch.pendingRequests(), 1);
    ch.tact(sink, 0);
    CHECK_EQ(sink.plays.size(), std::size_t{1});
    // pan = ((-500+10000)*-1000 + (-300+10000)*2000) / (-500-300+20000) = 515
    // attenuation = -500 + -300 + 10000 > 0 -> 0 (full volume)
    CHECK(near(sink.plays[0].pan, panOf(515)));
    CHECK(near(sink.plays[0].volume, 1.0f));
    // three requests: pan averaged in order; pans beyond ±10000 clamped first
    ch.request(4, 30000, -1000);
    ch.request(4, -10000, -1000);
    ch.request(4, 0, -2000);
    ch.tact(sink, 100);
    // step 1: (9000*10000 + 9000*-10000)/18000 = 0, att 0; step 2: (10000*0 + 8000*0)/18000 = 0
    CHECK(near(sink.plays[1].pan, 0.0f));
    // attenuation and pan conversion of a single request
    ch.request(5, -2000, -2000);
    ch.tact(sink, 200);
    CHECK(near(sink.plays[2].volume, 0.1f));
    CHECK(near(sink.plays[2].pan, -0.9f));
    // quieter than -20 dB: ignored
    ch.request(6, 0, -2001);
    CHECK_EQ(ch.pendingRequests(), 0);
}

// kind 100: non-positional, always full volume (still dropped below -20 dB).
void testNonPositional() {
    Bank bank;
    auto ch = bank.channels();
    MockSink sink;
    ch.request(12, 3000, -1500);
    ch.tact(sink, 0);
    CHECK_EQ(sink.plays.size(), std::size_t{1});
    CHECK(near(sink.plays[0].volume, 1.0f));
    CHECK(near(sink.plays[0].pan, panOf(3000)));
    CHECK(!sink.plays[0].loop);
    ch.request(12, 0, -2500);
    CHECK_EQ(ch.pendingRequests(), 0);
}

// StopSFX() stops every channel but the last effect number's and keeps the queue;
// StopSFX(n) releases the channels of n (no 50 ms guard afterwards).
void testStop() {
    Bank bank;
    bank.wav = &bank.wavSound;
    auto ch = bank.channels();
    MockSink sink;
    ch.request(1);
    ch.request(2);
    ch.request(13);
    ch.tact(sink, 0);
    CHECK_EQ(ch.activeChannels(), 3);
    ch.request(3);
    ch.stopAll(sink);
    CHECK_EQ(sink.stops.size(), std::size_t{2});
    CHECK_EQ(ch.activeChannels(), 1);
    CHECK_EQ(ch.pendingRequests(), 1);
    ch.tact(sink, 10);
    CHECK_EQ(sink.started(3), 1);
    CHECK_EQ(ch.activeChannels(), 2);
    ch.request(2);
    ch.tact(sink, 20); // stopped by StopSFX(): the guard still applies
    CHECK_EQ(sink.started(2), 1);
    ch.stop(sink, 3);
    CHECK_EQ(ch.activeChannels(), 1);
    ch.request(3);
    ch.tact(sink, 30); // released: no guard
    CHECK_EQ(sink.started(3), 2);
}

// Mixer: a looping voice wraps without a discontinuity, follows volume/pan updates and
// stops only when asked; voice ids follow one-shot voices until they end.
void testMixerLoop() {
    // 44 samples of a sine at 1/44 of the rate: exactly one period, so the loop is seamless
    std::vector<std::int16_t> wave(44);
    for (std::size_t i = 0; i < wave.size(); ++i)
        wave[i] = static_cast<std::int16_t>(16000 * std::sin(2 * std::numbers::pi * i / 44.0));
    const SoundData loop = makeSound(0, wave, 44100);
    const SoundData shot = makeSound(1, std::vector<std::int16_t>(441, 8000), 44100);
    Mixer m{44100};
    const auto id = m.playSfx(loop, 1.0f, 0.0f, true);
    const auto shotId = m.playSfx(shot, 1.0f, 0.0f, false);
    CHECK(id != 0 && shotId != 0 && id != shotId);
    std::vector<std::int16_t> out(2 * 4410);
    m.render(out.data(), 4410); // 100 ms = 100 periods of the loop
    CHECK(m.sfxPlaying(id));
    CHECK(!m.sfxPlaying(shotId)); // 10 ms: finished
    // after the one-shot ends (sample 441), the left channel is the sine: its largest step
    // between samples equals the sine's (no jump where the loop wraps)
    int maxStep = 0, peak = 0;
    for (std::size_t i = 500; i + 1 < 4410; ++i) {
        maxStep = std::max(maxStep, std::abs(out[2 * (i + 1)] - out[2 * i]));
        peak = std::max(peak, static_cast<int>(std::abs(out[2 * i])));
    }
    const int sineStep = static_cast<int>(16000 * 0.6 * 2 * std::numbers::pi / 44.0) + 2;
    CHECK(peak > 9000);
    CHECK(maxStep <= sineStep);
    // across render calls too
    std::vector<std::int16_t> more(2 * 100);
    m.render(more.data(), 100);
    CHECK(std::abs(more[0] - out[2 * 4409]) <= sineStep);
    // volume/pan update: hard right silences the left channel
    m.setSfxVolumePan(id, 0.5f, 1.0f);
    m.render(more.data(), 100);
    int left = 0, right = 0;
    for (int i = 0; i < 100; ++i) {
        left = std::max(left, std::abs(static_cast<int>(more[2 * i])));
        right = std::max(right, std::abs(static_cast<int>(more[2 * i + 1])));
    }
    CHECK_EQ(left, 0);
    CHECK(right > 4000 && right < 5000); // 16000 * 0.6 * 0.5
    m.stopSfxVoice(id);
    CHECK(!m.sfxPlaying(id));
    CHECK_EQ(m.activeVoices(), std::size_t{0});
    // a loop shorter than one output step at a high rate ratio still wraps
    const SoundData tiny = makeSound(0, {1000, -1000}, 44100 * 3);
    const auto tid = m.playSfx(tiny, 1.0f, 0.0f, true);
    m.render(more.data(), 100);
    CHECK(m.sfxPlaying(tid));
}

// DirectSound Stop keeps the position and the channel keeps its buffer: a looping engine that
// timed out (500 ms) and is requested again on the same channel resumes (Play without reload),
// as does a one-shot cut by StopSFX(); a one-shot that played to its end starts over (#18).
void testResumeAfterStop() {
    Bank bank;
    auto ch = bank.channels();
    MockSink sink;
    ch.request(1);
    ch.tact(sink, 1000);
    ch.tact(sink, 1501); // not requested for 501 ms: Stop
    CHECK_EQ(sink.stops.size(), std::size_t{1});
    ch.tact(sink, 1600); // the original calls Stop again on the stopped buffer
    ch.request(1);
    ch.tact(sink, 2000);
    CHECK_EQ(sink.loads, 1);              // same buffer
    CHECK_EQ(sink.started(1), 2);
    CHECK(sink.plays[1].resumed);         // from where it stopped
    CHECK(sink.plays[1].loop);
    // one-shot cut by StopSFX() and requested again: resumed
    ch.request(2);
    ch.tact(sink, 3000);
    ch.stopAll(sink);
    ch.request(2);
    ch.tact(sink, 3100);
    CHECK_EQ(sink.started(2), 2);
    CHECK(sink.plays.back().resumed);
    // finished on its own: from the start, still the same buffer
    sink.finish(sink.plays.back().voice);
    ch.request(2);
    ch.tact(sink, 3200);
    CHECK_EQ(sink.started(2), 3);
    CHECK(!sink.plays.back().resumed);
    CHECK_EQ(sink.loads, 2);
}

// PlayWAV (0x408c50): slot N is queued at once at full volume and loops (it was silent before
// #18); the same file while it plays is left alone; another file releases the channel and
// replaces the slot; a missing file returns 0 and empties the slot; StopSFX() spares the slot,
// StopMusic() (StopSFX(N)) releases it.
void testPlayWav() {
    Bank bank;
    auto ch = bank.channels();
    MockSink sink;
    using WavLoad = SoundChannels::WavLoad;
    int loads = 0;
    auto loader = [&](WavLoad result) {
        return [&, result] {
            ++loads;
            bank.wav = result == WavLoad::Loaded ? &bank.wavSound : nullptr;
            return result;
        };
    };
    CHECK_EQ(ch.playWav(sink, "a.wav", loader(WavLoad::Loaded)), 13);
    CHECK_EQ(ch.pendingRequests(), 1);
    ch.tact(sink, 100);
    CHECK_EQ(sink.started(13), 1);
    CHECK(sink.plays[0].loop && sink.plays[0].volume == 1.0f && sink.plays[0].pan == 0.0f);
    CHECK_EQ(ch.playWav(sink, "a.wav", loader(WavLoad::Loaded)), 13); // already playing
    CHECK_EQ(loads, 1);
    CHECK_EQ(ch.pendingRequests(), 0);
    ch.stopAll(sink);
    ch.tact(sink, 10000); // no expiry for the slot
    CHECK_EQ(ch.activeChannels(), 1);
    CHECK(sink.stops.empty());
    CHECK_EQ(ch.playWav(sink, "b.wav", loader(WavLoad::Loaded)), 13); // other file: released, reloaded
    CHECK_EQ(sink.stops.size(), std::size_t{1});
    CHECK_EQ(loads, 2);
    ch.tact(sink, 10100); // released: no 50 ms guard
    CHECK_EQ(sink.started(13), 2);
    CHECK_EQ(ch.playWav(sink, "missing.wav", loader(WavLoad::NotFound)), 0);
    CHECK_EQ(ch.activeChannels(), 0);
    CHECK_EQ(ch.pendingRequests(), 0);
    ch.request(13); // empty slot: ignored
    CHECK_EQ(ch.pendingRequests(), 0);
    CHECK_EQ(ch.playWav(sink, "bad.wav", loader(WavLoad::Invalid)), 13); // opened, no data
    CHECK_EQ(ch.pendingRequests(), 0);
    CHECK_EQ(ch.playWav(sink, "a.wav", loader(WavLoad::Loaded)), 13);
    ch.tact(sink, 10200);
    CHECK_EQ(ch.activeChannels(), 1);
    ch.stop(sink, 13); // StopMusic()
    CHECK_EQ(ch.activeChannels(), 0);
}

// The drop loop starts from queue entry 0, compared with attenuation 0 when that entry is
// free: with only positive attenuations pending (never produced by the game) nothing is dropped.
void testDropStartsFromEntryZero() {
    Bank bank;
    auto ch = bank.channels();
    MockSink sink;
    ch.request(1);
    ch.tact(sink, 0);  // looping sound on channel 0
    ch.request(1);     // entry 0: only a keep-alive, consumed before the drop loop
    for (int n = 2; n <= 11; ++n) ch.request(n, 0, 10 * (n - 1)); // entries 1..10, attenuation 10..100
    ch.tact(sink, 100); // 10 pending, 7 free channels: nothing dropped, the first 7 start
    for (int n = 2; n <= 8; ++n) CHECK_EQ(sink.started(n), 1);
    for (int n = 9; n <= 11; ++n) CHECK_EQ(sink.started(n), 0);
}

// Mixer: a paused voice keeps its position and resumes from it; a finished one-shot is gone.
void testMixerPauseResume() {
    std::vector<std::int16_t> ramp(4410);
    for (std::size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<std::int16_t>(i * 7);
    const SoundData s = makeSound(1, ramp, 44100);
    Mixer m{44100};
    const auto id = m.playSfx(s, 1.0f, 0.0f, false);
    std::vector<std::int16_t> out(2 * 1000);
    m.render(out.data(), 1000);
    const int before = out[2 * 999];
    m.pauseSfxVoice(id);
    CHECK(!m.sfxPlaying(id));
    m.render(out.data(), 1000);
    CHECK(std::ranges::all_of(out, [](std::int16_t v) { return v == 0; }));
    CHECK(m.resumeSfxVoice(id, false));
    CHECK(m.sfxPlaying(id));
    m.render(out.data(), 10);
    CHECK(out[0] > before && out[0] - before < 50); // continues the ramp
    out.resize(2 * 4000);
    m.render(out.data(), 4000);
    CHECK(!m.sfxPlaying(id));
    CHECK(!m.resumeSfxVoice(id, false)); // finished: must be played again
}

} // namespace

int main() {
    testLoopKeptAlive();
    testLoopDuplicatesAndExempt();
    testRestartGuard();
    testChannelLimit();
    testMerge();
    testNonPositional();
    testStop();
    testResumeAfterStop();
    testPlayWav();
    testDropStartsFromEntryZero();
    testMixerLoop();
    testMixerPauseResume();
    return gromada_test::finish();
}
