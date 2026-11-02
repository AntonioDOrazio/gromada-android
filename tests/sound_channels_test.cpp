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
    std::uint32_t voice;
};

// Records what the scheduler asks; voices play until stop() or finish().
struct MockSink final : SoundSink {
    std::vector<Play> plays;
    std::vector<std::uint32_t> stops;
    std::set<std::uint32_t> live;
    int updates = 0;
    std::uint32_t next = 0;
    std::uint32_t play(int nsfx, const SoundData&, float volume, float pan, bool loop) override {
        plays.push_back({nsfx, volume, pan, loop, ++next});
        live.insert(next);
        return next;
    }
    bool playing(std::uint32_t v) const override { return live.contains(v); }
    void update(std::uint32_t, float, float) override { ++updates; }
    void stop(std::uint32_t v) override { stops.push_back(v); live.erase(v); }
    void finishAll() { live.clear(); }
    int started(int nsfx) const {
        return static_cast<int>(std::ranges::count_if(plays, [nsfx](const Play& p) { return p.nsfx == nsfx; }));
    }
};

// Sounds 1..12: 1 = looping (kind 0), 2..11 = one-shot (kind 1), 12 = non-positional (100).
struct Bank {
    std::vector<SoundData> sounds;
    Bank() {
        sounds.push_back(makeSound(0, std::vector<std::int16_t>(100, 1000)));
        for (int i = 2; i <= 11; ++i) sounds.push_back(makeSound(1, std::vector<std::int16_t>(100, 1000)));
        sounds.push_back(makeSound(100, std::vector<std::int16_t>(100, 1000)));
    }
    SoundChannels channels() {
        return SoundChannels{[this](int n) -> const SoundData* {
                                 return n >= 1 && n <= static_cast<int>(sounds.size()) ? &sounds[n - 1] : nullptr;
                             },
                             [this] { return static_cast<int>(sounds.size()); }};
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
// (the second hits the 50 ms guard); the "last effect number" loop never times out.
void testLoopDuplicatesAndExempt() {
    Bank bank;
    bank.sounds.back().kind = 0; // sound 12 = count: the PlayWAV slot of the original
    auto ch = bank.channels();
    MockSink sink;
    ch.request(1, -500, -100);
    ch.request(1, 500, -200);
    CHECK_EQ(ch.pendingRequests(), 2);
    ch.request(12);
    ch.tact(sink, 0);
    CHECK_EQ(sink.started(1), 1);
    CHECK_EQ(sink.started(12), 1);
    ch.tact(sink, 5000);
    CHECK_EQ(ch.activeChannels(), 1); // sound 1 stopped, 12 kept
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
    sink.live.erase(sink.plays[0].voice);
    sink.live.erase(sink.plays[1].voice);
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
    bank.sounds.back().kind = 0;
    auto ch = bank.channels();
    MockSink sink;
    ch.request(1);
    ch.request(2);
    ch.request(12);
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

} // namespace

int main() {
    testLoopKeptAlive();
    testLoopDuplicatesAndExempt();
    testRestartGuard();
    testChannelLimit();
    testMerge();
    testNonPositional();
    testStop();
    testMixerLoop();
    return gromada_test::finish();
}
