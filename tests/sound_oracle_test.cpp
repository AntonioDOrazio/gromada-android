// Differential oracle of the sound-effect system (issue #18): game/sound_channels.hpp must make
// exactly the DirectSound calls the original gromada.exe makes. The vectors come from the
// original's functions (0x408dd0 queue, 0x408f20 Tact, 0x408910 channel choice, 0x409550
// StopSFX, 0x408d80 StopSFX(n), 0x408c50 PlayWAV) run in an emulator with a fake DirectSound
// that records every call (tools/oracle/gen_sound.py, docs/oracle.md "suono"). They are in the
// repo: no game data needed. With $GROMADA_DATA the effect -> sound mapping and the type bytes
// of fw.res are also checked against the descriptor table built by the original (0x408730).
#include "game/sound_channels.hpp"
#include "libgromada/game_resources.hpp"
#include "test.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using gromada::game::SoundChannels;
using gromada::game::SoundSink;

namespace {

using Row = std::vector<std::int64_t>;

// The "cases" of a tests/data/oracle/*.json file: arrays of integers.
std::vector<Row> readCases(const std::string& file) {
    std::ifstream in{std::string{GROMADA_ORACLE_DIR} + "/" + file};
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string s = ss.str();
    std::vector<Row> rows;
    auto p = s.find("\"cases\"");
    if (p == std::string::npos)
        return rows;
    p = s.find('[', p) + 1;
    Row* row = nullptr;
    while (p < s.size()) {
        const char c = s[p];
        if (c == '[') {
            row = &rows.emplace_back();
            ++p;
        } else if (c == ']') {
            if (!row)
                break;
            row = nullptr;
            ++p;
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            const bool neg = c == '-';
            if (neg) ++p;
            std::int64_t v = 0;
            while (p < s.size() && s[p] >= '0' && s[p] <= '9') v = v * 10 + (s[p++] - '0');
            if (row) row->push_back(neg ? -v : v);
        } else {
            ++p;
        }
    }
    return rows;
}

// A small WAV in memory -> SoundData (the only constructor reads a RIFF file).
SoundData makeSound(std::uint8_t kind) {
    std::string bytes = "RIFF";
    auto u32 = [&](std::uint32_t v) { for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<char>(v >> (8 * i))); };
    auto u16 = [&](std::uint16_t v) { for (int i = 0; i < 2; ++i) bytes.push_back(static_cast<char>(v >> (8 * i))); };
    u32(36 + 8);
    bytes += "WAVE";
    bytes += "fmt "; u32(16); u16(1); u16(1); u32(22050); u32(22050); u16(1); u16(8);
    bytes += "data"; u32(8);
    bytes += std::string(8, '\x80');
    std::istringstream in{bytes};
    BinaryStreamReader reader{in, bytes.size()};
    SoundData d{reader};
    d.kind = kind;
    return d;
}

enum Op : int { Status = 1, Pan, Volume, Play, Stop, Load, Release, SetPos };
enum Event : int { Tact = 0, Request, StopAll, StopOne, PlayWav, Finish };

// The fake DirectSound of the generator: one buffer per channel, records every call with the
// same encoding as the vectors. Playing from Play until Stop/Release or a FINISH event.
struct DeviceSink final : SoundSink {
    struct Buffer {
        bool exists = false, playing = false, looping = false;
        int nsfx = -1;
    };
    std::array<Buffer, SoundChannels::ChannelCount> buf{};
    Row ops;
    int errors = 0;

    bool load(int ch, int nsfx, const SoundData&, int source) override {
        if (buf[ch].exists || (source >= 0 && (!buf[source].exists || buf[source].nsfx != nsfx)))
            ++errors;
        buf[ch] = {true, false, false, nsfx};
        ops.insert(ops.end(), {Load, ch, nsfx, source});
        return true;
    }
    void release(int ch) override {
        if (!buf[ch].exists) ++errors;
        buf[ch] = {};
        ops.insert(ops.end(), {Release, ch});
    }
    bool playing(int ch) override {
        if (!buf[ch].exists) ++errors;
        const int st = buf[ch].playing ? 1 | (buf[ch].looping ? 4 : 0) : 0;
        ops.insert(ops.end(), {Status, ch, st});
        return st & 1;
    }
    void setPan(int ch, int pan) override { live(ch); ops.insert(ops.end(), {Pan, ch, pan}); }
    void setVolume(int ch, int att) override { live(ch); ops.insert(ops.end(), {Volume, ch, att}); }
    void play(int ch, bool loop) override {
        live(ch);
        buf[ch].playing = true;
        buf[ch].looping = loop;
        ops.insert(ops.end(), {Play, ch, loop ? 1 : 0});
    }
    void stop(int ch) override {
        live(ch);
        buf[ch].playing = false;
        ops.insert(ops.end(), {Stop, ch});
    }
    void live(int ch) { if (!buf[ch].exists) ++errors; }
};

int opArgs(std::int64_t op) {
    switch (op) {
    case Stop: case Release: return 1;
    case Load: return 3;
    default: return 2;
    }
}

int eventArgs(std::int64_t ev) {
    switch (ev) {
    case Tact: case StopOne: case Finish: return 1;
    case Request: case PlayWav: return 3;
    default: return 0;
    }
}

std::string fmt(const Row& r, std::size_t from, std::size_t to) {
    std::string s;
    for (std::size_t i = from; i < to && i < r.size(); ++i) s += std::to_string(r[i]) + (i + 1 < to ? "," : "");
    return s;
}

struct Stats {
    int sequences = 0, events = 0, ops = 0, diffs = 0;
};

// Replays one sequence through SoundChannels; returns false at the first difference.
bool replay(const Row& row, const std::string& family, int index, Stats& st) {
    std::size_t p = 0;
    auto next = [&] { return p < row.size() ? row[p++] : std::int64_t{-999999}; };
    const int n = static_cast<int>(next());
    std::vector<SoundData> sounds;
    for (int i = 0; i <= n; ++i) {
        const auto t = next();
        if (i < n) sounds.push_back(makeSound(static_cast<std::uint8_t>(t)));
    }
    const SoundData wavSound = makeSound(100); // like Session::playWav (the slot is looping anyway)
    bool wavLoaded = false;
    SoundChannels ch{[&](int nsfx) { return gromada::game::effectSound(sounds, wavLoaded ? &wavSound : nullptr, nsfx); },
                     [n] { return n; }};
    DeviceSink sink;
    const auto events = next();
    auto fail = [&](std::size_t e, const std::string& what) {
        if (st.diffs++ < 12)
            std::fprintf(stderr, "%s[%d] event %zu: %s\n", family.c_str(), index, e, what.c_str());
        return false;
    };
    for (std::int64_t e = 0; e < events; ++e) {
        const std::size_t evStart = p;
        const auto code = next();
        std::array<std::int64_t, 3> a{};
        for (int i = 0; i < eventArgs(code); ++i) a[i] = next();
        sink.ops.clear();
        switch (code) {
        case Tact: ch.tact(sink, static_cast<std::uint32_t>(a[0])); break;
        case Request: ch.request(static_cast<int>(a[0]), static_cast<int>(a[1]), static_cast<int>(a[2])); break;
        case StopAll: ch.stopAll(sink); break;
        case StopOne: ch.stop(sink, static_cast<int>(a[0])); break;
        case PlayWav: {
            const int ret = ch.playWav(sink, std::format("wav\\{:02}.wav", a[0]), [&] {
                wavLoaded = a[1] == 2;
                return static_cast<SoundChannels::WavLoad>(a[1]);
            });
            if (ret != a[2])
                return fail(e, std::format("PlayWAV returned {}, original {}", ret, a[2]));
            break;
        }
        case Finish:
            if (!sink.buf[a[0]].playing || sink.buf[a[0]].looping)
                return fail(e, "FINISH on a channel that is not playing a one-shot");
            sink.buf[a[0]].playing = false;
            break;
        default: return fail(e, "bad event code");
        }
        Row expected;
        const auto nops = next();
        for (std::int64_t k = 0; k < nops; ++k) {
            const auto op = next();
            expected.push_back(op);
            for (int i = 0; i < opArgs(op); ++i) expected.push_back(next());
        }
        ++st.events;
        st.ops += static_cast<int>(nops);
        if (sink.ops != expected)
            return fail(e, std::format("event [{}]\n   original: [{}]\n   port:     [{}]", fmt(row, evStart, evStart + 1 + eventArgs(code)),
                                       fmt(expected, 0, expected.size()), fmt(sink.ops, 0, sink.ops.size())));
        if (sink.errors)
            return fail(e, "operation on a channel without a buffer");
    }
    // final state of the channels and of the queue
    Row expected(row.begin() + static_cast<std::ptrdiff_t>(std::min(p, row.size())), row.end());
    Row got;
    for (const auto& c : ch.channels())
        got.insert(got.end(), {c.active ? 1 : 0, c.loaded ? 1 : 0, c.nsfx, static_cast<std::int64_t>(c.time)});
    Row q;
    for (int i = 0; i < SoundChannels::QueueSize; ++i)
        if (const auto& r = ch.queue()[i]; r.nsfx >= 0)
            q.insert(q.end(), {i, r.nsfx, r.pan, r.attenuation});
    got.push_back(static_cast<std::int64_t>(q.size() / 4));
    got.insert(got.end(), q.begin(), q.end());
    if (got != expected)
        return fail(static_cast<std::size_t>(events), std::format("final state\n   original: [{}]\n   port:     [{}]",
                                                                    fmt(expected, 0, expected.size()), fmt(got, 0, got.size())));
    ++st.sequences;
    return true;
}

void testFamily(const std::string& family, std::size_t minSequences) {
    const auto rows = readCases(family + ".json");
    CHECK(rows.size() >= minSequences);
    Stats st;
    for (std::size_t i = 0; i < rows.size(); ++i)
        replay(rows[i], family, static_cast<int>(i), st);
    std::fprintf(stderr, "%s: %d/%zu sequences identical (%d events, %d DirectSound operations), %d different\n",
                 family.c_str(), st.sequences, rows.size(), st.events, st.ops, st.diffs);
    CHECK_EQ(st.diffs, 0);
    CHECK_EQ(static_cast<std::size_t>(st.sequences), rows.size());
}

std::uint32_t crc32(std::span<const std::byte> data) {
    std::uint32_t c = 0xFFFFFFFFu;
    for (const auto b : data) {
        c ^= static_cast<std::uint8_t>(b);
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

// fw.res as loaded by the core (GameResources) vs the descriptor table built by the original.
void testDescriptorTable() {
    const auto dir = gromada_test::dataDir();
    if (!dir) {
        std::fprintf(stderr, "sound_table: SKIP (set GROMADA_DATA to the game folder)\n");
        return;
    }
    const auto table = readCases("sound_table.json");
    GameResources res{*dir / "fw.res"};
    const auto sounds = res.sounds();
    const int n = static_cast<int>(table.size()) - 1;
    CHECK_EQ(static_cast<int>(sounds.size()), n); // N: the PlayWAV slot
    const SoundData wav = makeSound(100);
    CHECK(gromada::game::effectSound(sounds, nullptr, n) == nullptr);
    CHECK(gromada::game::effectSound(sounds, &wav, n) == &wav);
    CHECK(gromada::game::effectSound(sounds, &wav, 0) == nullptr);      // rejected by 0x408dd0
    CHECK(gromada::game::effectSound(sounds, &wav, n + 1) == nullptr);
    int checked = 0;
    for (int i = 0; i < n && i < static_cast<int>(sounds.size()); ++i) {
        const auto& t = table[static_cast<std::size_t>(i)];
        const auto& s = sounds[static_cast<std::size_t>(i)];
        const auto bytes = std::visit([](const auto& v) { return std::as_bytes(std::span{v}); }, s.waveData);
        const bool same = t[0] == i && t[1] == s.kind && t[2] == static_cast<std::int64_t>(bytes.size()) &&
                          t[4] == s.numChannels && t[5] == s.sampleRate && t[9] == crc32(bytes);
        if (!same)
            std::fprintf(stderr, "sound %d: original type %lld, %lld bytes, %lld Hz, crc %lld; core kind %d, %zu bytes, %u Hz, crc %u\n",
                         i, static_cast<long long>(t[1]), static_cast<long long>(t[2]), static_cast<long long>(t[5]),
                         static_cast<long long>(t[9]), s.kind, bytes.size(), s.sampleRate, crc32(bytes));
        CHECK(same);
        if (i >= 1)
            CHECK(gromada::game::effectSound(sounds, nullptr, i) == &s);
        checked += same;
    }
    std::fprintf(stderr, "sound_table: %d/%d fw.res sounds identical to the original's descriptors (N = %d)\n", checked, n, n);
}

} // namespace

int main() {
    testFamily("sound_edge", 200);
    testFamily("sound_random", 1000);
    testDescriptorTable();
    return gromada_test::finish();
}
