// Game-level trace of our core (issue #13): replays a recorded demo with the session and the
// engine and writes, after every demo tick, the same JSON line as the emulated original
// (tools/oracle/trace_game.py): clock, rand() state, sprite count, camera, CRC per z layer,
// the "dynamic" sprites (all but terrain, scenery, menus, texts) and the script's builtin
// calls, and the sound requests and effect-channel operations of the frame (SoundChannels).
// tools/oracle/compare_traces.py compares the two traces (docs/trace-oracle.md).
//
//   game_trace --data DIR --demo N -o OUT.jsonl[.gz] [--frames N] [--full-at F,F,...]
//
// The trace contains game data: write it outside the repository.

#include "core/std.hpp"
#include "libgromada/libgromada_all.hpp"
#include "game/session.hpp"
#include "game/engine.hpp"

#include <cstdio>
#include <set>

using namespace gromada;
using namespace gromada::game;

namespace {

// zlib-compatible CRC-32 (the Python side uses zlib.crc32).
std::uint32_t crc32(std::uint32_t crc, const std::uint8_t* p, std::size_t n) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (std::size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

void jsonString(std::string& out, std::string_view s) {
    out += '"';
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c >= 0x20 && c < 0x7f) out += ch;
        else out += std::format("\\u{:04x}", c);   // bytes as latin-1 code points, like Python
    }
    out += '"';
}

bool isSessionHandle(std::int32_t v) { return (static_cast<std::uint32_t>(v) & 0x40000000u) != 0 && v > 0; }

void jsonValue(std::string& out, const script::Value& v) {
    if (v.isString()) jsonString(out, v.stringValue());
    else if (isSessionHandle(v.intValue())) out += "\"S\"";
    else out += std::to_string(v.intValue());
}

constexpr std::array<std::uint8_t, 3> NoHp{cls::Superstructure, cls::Image, cls::Effect2};
constexpr std::array<std::uint8_t, 5> Units{cls::Vehicle, cls::Building, cls::Avia, cls::Mine, cls::Kassandra};
bool in(auto& a, std::uint8_t c) { return std::ranges::find(a, c) != a.end(); }
bool sessionClass(std::uint8_t c) { return c == cls::Image || c == cls::Font; }
bool staticClass(std::uint8_t c) { return c == cls::Terrain || c == cls::Static; }

void sprite(std::string& out, const Sprite& s, int layer, int parent) {
    const int hp = in(NoHp, s.cls) ? 0 : s.hp;
    const int target = in(Units, s.cls) ? s.target : 0;
    const int ncmd = s.commands ? s.commands->count : 0;
    out += std::format("[{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}],", s.id, layer, s.cls,
                       s.nvid, s.x, s.y, s.z, s.direction, s.action, s.frame, s.state, hp, s.speed, s.vz, s.goX,
                       s.goY, s.goZ, s.timer, s.lastTact, ncmd, target, parent);
}

// Sound sink of the trace: nothing is heard; a voice "plays" for the length of its sound in
// game-clock milliseconds (looping voices until stopped). The emulated original uses the
// same model for its fake DirectSound buffers (tools/oracle/dsound.py), so the channel
// scheduler sees the same "finished" channels on both sides.
struct TraceSink final : SoundSink {
    const Engine* engine = nullptr;
    struct Voice { std::uint32_t start = 0, duration = 0; bool loop = false, active = false; };
    std::vector<Voice> voices{Voice{}};
    std::uint32_t play(int, const SoundData& s, float, float, bool loop) override {
        std::size_t samples = 0;
        std::visit([&](const auto& v) { samples = v.size(); }, s.waveData);
        const std::uint64_t frames = s.numChannels ? samples / s.numChannels : samples;
        const auto dur = s.sampleRate ? static_cast<std::uint32_t>(frames * 1000 / s.sampleRate) : 0u;
        voices.push_back({static_cast<std::uint32_t>(engine->clock()), dur, loop, true});
        return static_cast<std::uint32_t>(voices.size() - 1);
    }
    bool playing(std::uint32_t v) const override {
        if (v == 0 || v >= voices.size() || !voices[v].active) return false;
        const auto& x = voices[v];
        return x.loop || static_cast<std::uint32_t>(engine->clock()) - x.start < x.duration;
    }
    void update(std::uint32_t, float, float) override {}
    void stop(std::uint32_t v) override {
        // only audible stops are traced (the original's fake DirectSound does the same)
        if (!playing(v) && stopEntry && audio && audio->size() >= *stopEntry) audio->resize(*stopEntry);
        stopEntry.reset();
        if (v < voices.size()) voices[v].active = false;
    }
    std::string* audio = nullptr;              // the frame's audio entries
    std::optional<std::size_t> stopEntry;      // where the observer appended the last 's'
};

struct Options {
    std::filesystem::path data;
    int demo = 1;
    std::string out;
    std::size_t frames = 0;
    std::set<std::size_t> fullAt;
};

int usage() {
    std::fprintf(stderr, "usage: game_trace --data DIR --demo N -o OUT.jsonl[.gz] [--frames N] [--full-at F,F,...]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (const char* d = std::getenv("GROMADA_DATA")) opt.data = d;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string{}; };
        if (a == "--data") opt.data = next();
        else if (a == "--demo") opt.demo = std::stoi(next());
        else if (a == "-o" || a == "--out") opt.out = next();
        else if (a == "--frames") opt.frames = std::stoul(next());
        else if (a == "--full-at") {
            std::stringstream ss(next());
            std::string item;
            while (std::getline(ss, item, ',')) if (!item.empty()) opt.fullAt.insert(std::stoul(item));
        } else return usage();
    }
    if (opt.data.empty() || opt.out.empty()) return usage();

    GameResources res{opt.data / "fw.res"};
    SessionConfig cfg;
    cfg.gameDir = opt.data;
    cfg.render = false;
    cfg.log = [](LogLevel l, std::string_view t) {
        if (l >= LogLevel::Warn) std::fprintf(stderr, "[session] %.*s\n", static_cast<int>(t.size()), t.data());
    };
    auto enginePtr = std::make_unique<GameEngine>(res);
    GameEngine& engine = *enginePtr;
    Session session{res.vids(), res.sounds(), std::move(enginePtr), nullptr, nullptr, cfg};

    std::string builtins;
    std::vector<std::string> posted;
    session.builtinObserver = [&](int id, std::span<const script::Value> args, const script::Value& r) {
        builtins += '[' + std::to_string(id) + ",[";
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (i) builtins += ',';
            jsonValue(builtins, args[i]);
        }
        builtins += ']';
        if (script::knownBuiltinPushes(id, args)) {
            builtins += ',';
            jsonValue(builtins, r);
        }
        builtins += "],";
        if (id == 64) posted.push_back(script::argString(args, 0)); // Load(): posted in the original
    };

    const bool gz = opt.out.ends_with(".gz");
    std::FILE* out = gz ? popen(std::format("gzip -3 > '{}'", opt.out).c_str(), "w") : std::fopen(opt.out.c_str(), "w");
    if (!out) { std::perror(opt.out.c_str()); return 1; }
    const std::string demoName = std::format("maps\\\\demo{}.dem", opt.demo);
    std::fprintf(out, "{\"trace\": \"gromada-game-trace\", \"version\": 1, \"source\": \"core\", \"demo\": \"%s\", "
                      "\"fields\": [\"uid\", \"layer\", \"cls\", \"nvid\", \"x\", \"y\", \"z\", \"dir\", \"act\", \"frame\", "
                      "\"state\", \"hp\", \"speed\", \"vz\", \"goX\", \"goY\", \"goZ\", \"timer\", \"lastTact\", \"ncmd\", "
                      "\"target\", \"parent\"]}\n", demoName.c_str());

    TraceSink sink;
    sink.engine = &engine;
    session.setSoundSink(&sink);
    std::string audio;
    sink.audio = &audio;
    auto& channels = session.soundChannels();
    channels.requestObserver = [&](int nsfx, int pan, int att) {
        audio += std::format("[\"r\",{},{},{}],", nsfx, pan, att);
    };
    channels.channelObserver = [&](char op, int ch, int nsfx, int att, int pan, bool loop) {
        if (op == 's') { sink.stopEntry = audio.size(); audio += std::format("[\"s\",{},{}],", ch, nsfx); }
        else if (op == 'u') audio += std::format("[\"u\",{},{},{},{}],", ch, nsfx, att, pan);
        else audio += std::format("[\"p\",{},{},{},{},{}],", ch, nsfx, att, pan, loop ? 1 : 0);
    };

    session.playDemo(opt.demo);
    if (!session.demoPlaying()) { std::fprintf(stderr, "demo %d did not start\n", opt.demo); return 1; }
    builtins.clear();
    audio.clear();
    input::PcState pc;
    std::size_t frame = 0;
    std::string line;
    const auto t0 = std::chrono::steady_clock::now();
    while (session.demoPlaying() && (opt.frames == 0 || frame < opt.frames)) {
        builtins.clear();
        posted.clear();
        audio.clear();
        const auto ticks = session.ticks();
        // Real time of the call = the recorded step (plus 1 us): every call replays exactly one
        // record, and what the session times with real time (typewriter, menu animations)
        // advances like the game clock, as in the recording.
        const int due = session.demo() ? std::max(0, session.demo()->due()) : 0;
        session.runFrame(pc, due * 0.001 + 1e-6);
        if (session.ticks() == ticks) break;      // the demo ended (end of records)
        if (session.ticks() != ticks + 1) { std::fprintf(stderr, "frame %zu: %llu ticks\n", frame, static_cast<unsigned long long>(session.ticks() - ticks)); return 1; }

        World& w = engine.world();
        line.clear();
        std::array<std::uint32_t, World::MaxLayers> crcs{};
        std::string sprites;
        std::size_t total = 0;
        const bool full = opt.fullAt.contains(frame);
        for (int l = 0; l < World::MaxLayers; ++l) {
            std::uint32_t crc = 0;
            for (const Sprite* s = w.layerHead(l); s; s = s->next) {
                if (sessionClass(s->cls)) continue;
                ++total;
                std::uint8_t rec[14];
                auto put16 = [&](int o, int v) { rec[o] = static_cast<std::uint8_t>(v); rec[o + 1] = static_cast<std::uint8_t>(v >> 8); };
                put16(0, s->nvid); put16(2, s->x); put16(4, s->y); put16(6, s->z);
                rec[8] = s->direction; rec[9] = s->action;
                put16(10, s->frame); put16(12, s->state);
                crc = crc32(crc, rec, sizeof rec);
                if (full || !staticClass(s->cls)) {
                    sprite(sprites, *s, l, 0);
                    int parent = s->id;
                    for (const Sprite* c = s->child; c; c = c->child) {
                        sprite(sprites, *c, -1, parent);
                        parent = c->id;
                    }
                }
            }
            crcs[static_cast<std::size_t>(l)] = crc;
        }
        if (!sprites.empty()) sprites.pop_back();
        std::string b = builtins;
        if (!b.empty()) b.pop_back();
        line = std::format("{{\"f\":{},\"clock\":{},\"rng\":{},\"n\":{},\"cam\":[{},{}],\"crc\":[", frame, engine.clock(),
                           w.rng().state, total, engine.camera().x, engine.camera().y);
        for (int l = 0; l < World::MaxLayers; ++l) line += std::format("{}{}", l ? "," : "", crcs[static_cast<std::size_t>(l)]);
        std::string a = audio;
        if (!a.empty()) a.pop_back();
        line += "],\"s\":[" + sprites + "],\"b\":[" + b + "],\"a\":[" + a + "]";
        if (!posted.empty()) {
            line += ",\"post\":[";
            for (std::size_t i = 0; i < posted.size(); ++i) {
                if (i) line += ',';
                jsonString(line, posted[i]);
            }
            line += ']';
        }
        line += "}\n";
        std::fwrite(line.data(), 1, line.size(), out);
        ++frame;
        if (!posted.empty()) break;                  // the script loaded another map: the demo ended
        if (frame % 1000 == 0) {
            const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::fprintf(stderr, "frame %zu clock %d sprites %zu (%.1fs)\n", frame, engine.clock(), total, s);
        }
    }
    if (gz) pclose(out); else std::fclose(out);
    std::fprintf(stderr, "demo%d: %zu frames -> %s\n", opt.demo, frame, opt.out.c_str());
    return 0;
}
