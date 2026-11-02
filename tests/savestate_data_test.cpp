// Save states on the original data (issue #14; skipped without GROMADA_DATA): a real level is
// played, saved, played on, loaded and played again with the same input -> identical pictures,
// sound (effects and CD music) and state bytes; the run-ahead pattern (save, run one frame,
// load) leaves the game exactly where a plain run goes; a demo is restored mid-playback;
// corrupted states (bit flips with a valid CRC, truncations, foreign data) are refused without
// crashing and without touching the session. Prints size and timings on level 19.
#include "core/std.hpp"
#include "libgromada/libgromada_all.hpp"
#include "game/savestate.hpp"
#include "test.hpp"

#include <unistd.h>

using namespace gromada;
using namespace gromada::game;

namespace {

std::uint64_t fnv(const void* p, std::size_t n, std::uint64_t h = 0xcbf29ce484222325ull) {
    return state::fnv1a(std::span{static_cast<const std::uint8_t*>(p), n}, h);
}

std::filesystem::path tempDir(const char* name) {
    auto p = std::filesystem::temp_directory_path() / std::format("gromada_{}_{}", name, ::getpid());
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p;
}

// A session with its mixer, rendering and capturing one frame of audio per frame.
struct Rig {
    const GameResources& res;
    Mixer mixer{44100};
    std::filesystem::path save;
    std::unique_ptr<Session> s;
    SaveStateManager states;
    std::vector<std::int16_t> audio;
    int frame = 0;

    Rig(const GameResources& r, const std::filesystem::path& game, const char* name) : res{r}, save{tempDir(name)} {
        mixer.setMusicDirectories({game / "music", game / "MUSIC"});
        SessionConfig cfg;
        cfg.gameDir = game;
        cfg.saveDir = save;
        s = std::make_unique<Session>(res.vids(), res.sounds(), std::make_unique<GameEngine>(res), &mixer, nullptr, cfg);
        s->setVolumes(0.8f, 1.0f);
    }
    ~Rig() { s.reset(); std::filesystem::remove_all(save); }

    // Scripted input: a function of the frame number only, so a replay gets the same input.
    input::PcState inputAt(int f) const {
        input::PcState pc;
        pc.cursor = {static_cast<float>(320 + 200 * std::sin(f * 0.031)), static_cast<float>(240 + 150 * std::cos(f * 0.017))};
        const auto button = [&](input::ButtonState& b, bool down, bool before) { b = {down, down && !before, !down && before}; };
        const auto leftAt = [](int k) { return k % 90 < 4; };
        const auto rightAt = [](int k) { return k % 150 > 100; };
        button(pc.left, leftAt(f), leftAt(f - 1));
        button(pc.right, rightAt(f), rightAt(f - 1));
        const auto enterAt = [](int k) { return k < 400 && k % 40 < 2; };   // briefing, dialogs
        button(pc.keys[static_cast<std::size_t>(input::Key::Enter)], enterAt(f), enterAt(f - 1));
        const auto weaponAt = [](int k) { return k % 333 < 2; };
        button(pc.keys[static_cast<std::size_t>(input::Key::Num2)], weaponAt(f), weaponAt(f - 1));
        return pc;
    }

    // Runs one frame; returns the hash of picture and sound.
    std::uint64_t step() {
        s->runFrame(inputAt(frame), 1.0 / 60);
        audio.assign(735 * 2, 0);
        mixer.render(audio.data(), 735);
        ++frame;
        std::uint64_t h = fnv(s->xrgb(), static_cast<std::size_t>(s->width()) * s->height() * 4);
        return fnv(audio.data(), audio.size() * 2, h);
    }
    std::vector<std::uint64_t> run(int n) {
        std::vector<std::uint64_t> h;
        for (int i = 0; i < n; ++i) h.push_back(step());
        return h;
    }
    std::vector<std::uint8_t> saveState() {
        std::vector<std::uint8_t> out;
        const bool ok = states.save(*s, out);
        if (!ok) std::fprintf(stderr, "save failed: %s\n", states.error().c_str());
        CHECK(ok);
        return out;
    }
    bool loadState(std::span<const std::uint8_t> data, int frameOfState) {
        const bool ok = states.load(*s, data);
        if (ok) frame = frameOfState;
        return ok;
    }
};

std::size_t sprites(Session& s) { return dynamic_cast<GameEngine&>(s.engine()).world().liveCount(); }

void recomputeCrc(std::vector<std::uint8_t>& st) {
    std::uint64_t size;
    std::memcpy(&size, st.data() + 16, 8);
    const std::uint64_t sum = stateChecksum(std::span{st}.first(SaveStateHeaderSize), std::span{st}.subspan(SaveStateHeaderSize, size));
    std::memcpy(st.data() + 24, &sum, 8);
}

// Level: save, run, load, run again; run-ahead; timings.
void level(const GameResources& res, const std::filesystem::path& game, int n, bool timings) {
    Rig r{res, game, "savestate_level"};
    r.s->start(std::format("maps\\levels\\{:02}.map", n));
    r.run(2);
    // the game's fw.ini has the CD music off; levels started directly keep it off: switch it on
    const std::array<script::Value, 1> volume{script::Value{80}};
    r.s->dispatch(static_cast<int>(script::BuiltinId::SetMusic), volume);
    r.run(598);
    const std::string mapAtSave = r.s->currentMap();
    std::fprintf(stderr, "level %02d: %zu sprites, map %s, menu %d, CD track %d\n", n, sprites(*r.s), mapAtSave.c_str(),
                 r.s->ui().hasMenu(), r.mixer.currentTrack());

    const auto s0 = r.saveState();
    const int f0 = r.frame;
    const auto h1 = r.run(400);
    const auto s1 = r.saveState();
    if (r.s->currentMap() != mapAtSave)   // the scripted input may end the level: a map change
        std::fprintf(stderr, "  replayed across a map change: %s -> %s\n", mapAtSave.c_str(), r.s->currentMap().c_str());
    CHECK(r.loadState(s0, f0));
    // the loaded state saves back to the same bytes
    CHECK(r.saveState() == s0);
    const auto h2 = r.run(400);
    const auto s1b = r.saveState();
    CHECK(h1 == h2);
    CHECK(s1 == s1b);
    std::size_t firstDiff = h1.size();
    for (std::size_t i = 0; i < h1.size() && i < h2.size(); ++i)
        if (h1[i] != h2[i]) { firstDiff = i; break; }
    if (firstDiff < h1.size()) std::fprintf(stderr, "  replay differs from frame %zu\n", firstDiff);

    // run-ahead: every frame run, save, run one more, load; the real frames match h1
    CHECK(r.loadState(s0, f0));
    std::vector<std::uint64_t> h3;
    std::vector<std::uint8_t> tmp;
    for (int i = 0; i < 200; ++i) {
        h3.push_back(r.step());
        CHECK(r.states.save(*r.s, tmp));
        const int at = r.frame;
        r.step();
        CHECK(r.loadState(tmp, at));
    }
    CHECK(std::equal(h3.begin(), h3.end(), h1.begin()));

    if (timings) {
        double save = 0, load = 0, saveMax = 0, loadMax = 0;
        constexpr int N = 60;
        for (int i = 0; i < N; ++i) {
            CHECK(r.states.save(*r.s, tmp));
            save += r.states.lastSaveMs();
            saveMax = std::max(saveMax, r.states.lastSaveMs());
            CHECK(r.states.load(*r.s, tmp));
            load += r.states.lastLoadMs();
            loadMax = std::max(loadMax, r.states.lastLoadMs());
        }
        std::fprintf(stderr, "level %02d: state %zu bytes (limit %zu), save avg %.3f ms max %.3f ms, load avg %.3f ms max %.3f ms\n",
                     n, tmp.size(), SaveStateMaxSize, save / N, saveMax, load / N, loadMax);
        CHECK(tmp.size() * 2 < SaveStateMaxSize);   // generous margin under the fixed size
    }
}

// Corrupted and foreign states are refused and change nothing.
void robustness(const GameResources& res, const std::filesystem::path& game) {
    Rig r{res, game, "savestate_fuzz"};
    r.s->start("maps\\levels\\01.map");
    r.run(300);
    const auto good = r.saveState();
    std::uint64_t used;
    std::memcpy(&used, good.data() + 16, 8);
    used += SaveStateHeaderSize;

    std::uint64_t x = 0x243F6A8885A308D3ull;
    auto rnd = [&] { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
    int accepted = 0, rejected = 0;
    std::map<std::string, int> reasons;
    // truncations: every length up to 4 KB, then sampled
    for (std::size_t n = 0; n < used; n += n < 4096 ? 1 : 1 + rnd() % 997) {
        if (r.states.load(*r.s, std::span{good}.first(n))) ++accepted; else ++rejected;
    }
    CHECK_EQ(accepted, 0);
    // bit flips without fixing the CRC: always refused
    for (int i = 0; i < 300; ++i) {
        auto bad = good;
        bad[rnd() % used] ^= static_cast<std::uint8_t>(1u << (rnd() % 8));
        if (r.states.load(*r.s, bad)) ++accepted;
    }
    CHECK_EQ(accepted, 0);
    // foreign data, other version
    {
        auto bad = good;
        bad[32] ^= 1;   // fingerprint of fw.res
        recomputeCrc(bad);
        CHECK(!r.states.load(*r.s, bad));
        bad = good;
        bad[8] = 2;     // version
        recomputeCrc(bad);
        CHECK(!r.states.load(*r.s, bad));
        bad = good;
        bad[0] = 'X';
        CHECK(!r.states.load(*r.s, bad));
    }
    // the session is untouched by all of the above
    CHECK(r.saveState() == good);

    // structure-aware fuzzing: corrupt the payload and fix the CRC, so the decoders and
    // validators see the bytes. Must not crash; whatever is accepted must keep running.
    int fuzzAccepted = 0;
    for (int i = 0; i < 3000; ++i) {
        auto bad = good;
        const int kind = i % 3;
        if (kind == 0) {
            for (int k = 0; k < 1 + static_cast<int>(rnd() % 4); ++k)
                bad[SaveStateHeaderSize + rnd() % (used - SaveStateHeaderSize)] ^= static_cast<std::uint8_t>(1u << (rnd() % 8));
        } else if (kind == 1) {
            const std::size_t at = SaveStateHeaderSize + rnd() % (used - SaveStateHeaderSize);
            std::uint32_t v = static_cast<std::uint32_t>(rnd());
            if (rnd() % 2) v = static_cast<std::uint32_t>(rnd() % 4) == 0 ? 0xFFFFFFFFu : v & 0xFFFF;
            std::memcpy(bad.data() + at, &v, std::min<std::size_t>(4, used - at));
        } else {
            // shorter payload with a matching size field
            const std::uint64_t n = SaveStateHeaderSize + rnd() % (used - SaveStateHeaderSize);
            const std::uint64_t size = n - SaveStateHeaderSize;
            std::memcpy(bad.data() + 16, &size, 8);
        }
        recomputeCrc(bad);
        if (!r.states.load(*r.s, bad)) {
            ++reasons[r.states.error().substr(0, 40)];
            continue;
        }
        ++fuzzAccepted;
        r.run(20);                     // accepted (e.g. a changed coordinate): keeps running
        CHECK(r.states.load(*r.s, good));
    }
    std::fprintf(stderr, "fuzz: %d of 3000 structurally corrupted states accepted (they ran 20 frames)\n", fuzzAccepted);
    int shown = 0;
    for (const auto& [why, count] : reasons)
        if (shown++ < 12) std::fprintf(stderr, "  refused %5d x %s\n", count, why.c_str());
    CHECK(r.states.load(*r.s, good));
    CHECK(r.saveState() == good);
}

// A demo restored mid-playback replays identically (its frames without a tick show the
// stored picture).
void demoPlayback(const GameResources& res, const std::filesystem::path& game) {
    Rig r{res, game, "savestate_demo"};
    r.s->start("maps\\mainmenu.map");
    r.s->playDemo(1);
    // no input: a key would end the demo
    const auto quiet = [&](int n) {
        std::vector<std::uint64_t> h;
        for (int i = 0; i < n; ++i) {
            r.s->runFrame(input::PcState{}, 1.0 / 60);
            r.audio.assign(735 * 2, 0);
            r.mixer.render(r.audio.data(), 735);
            h.push_back(fnv(r.audio.data(), r.audio.size() * 2, fnv(r.s->xrgb(), static_cast<std::size_t>(r.s->width()) * r.s->height() * 4)));
        }
        return h;
    };
    quiet(300);
    CHECK(r.s->demoPlaying());
    const auto s0 = r.saveState();
    const auto h1 = quiet(300);
    CHECK(r.states.load(*r.s, s0));
    const auto h2 = quiet(300);
    CHECK(h1 == h2);
    // and into a session that is not playing it
    Rig other{res, game, "savestate_demo2"};
    other.s->start("maps\\mainmenu.map");
    CHECK(other.states.load(*other.s, s0));
    CHECK(other.s->demoPlaying());
    std::vector<std::uint64_t> h3;
    for (int i = 0; i < 300; ++i) {
        other.s->runFrame(input::PcState{}, 1.0 / 60);
        other.audio.assign(735 * 2, 0);
        other.mixer.render(other.audio.data(), 735);
        h3.push_back(fnv(other.audio.data(), other.audio.size() * 2, fnv(other.s->xrgb(), static_cast<std::size_t>(other.s->width()) * other.s->height() * 4)));
    }
    CHECK(h1 == h3);
}

// A state loaded into another session (fresh process situation): same frames afterwards.
void crossSession(const GameResources& res, const std::filesystem::path& game) {
    Rig a{res, game, "savestate_cross_a"};
    a.s->start("maps\\levels\\05.map");
    a.run(500);
    const auto st = a.saveState();
    const int f = a.frame;
    const std::string mapAtSave = a.s->currentMap();
    const auto ha = a.run(300);
    Rig b{res, game, "savestate_cross_b"};
    b.s->start("maps\\mainmenu.map");
    b.run(30);
    CHECK(b.loadState(st, f));
    CHECK(b.s->currentMap() == mapAtSave);
    const auto hb = b.run(300);
    CHECK(ha == hb);
}

// The CD music of the game (Ogg Vorbis): a state restored into a fresh mixer reopens the track
// and seeks; the samples must be those of the uninterrupted playback.
void music(const std::filesystem::path& game) {
    const std::vector<std::filesystem::path> dirs{game / "music", game / "MUSIC"};
    Mixer m{44100};
    m.setMusicDirectories(dirs);
    m.playMusic({3, 4, 3, 4});
    if (m.currentTrack() != 3) {
        std::fprintf(stderr, "music: track 3 not found, skipped\n");
        return;
    }
    std::vector<std::int16_t> out(735 * 2);
    const auto refNone = [](const SoundData*) { return std::int32_t{-1}; };
    const auto lookupNone = [](std::int32_t) -> const SoundData* { return nullptr; };
    for (const int frames : {1, 97, 1234, 4000}) {
        Mixer a{44100};
        a.setMusicDirectories(dirs);
        a.playMusic({3, 4, 3, 4});
        for (int i = 0; i < frames; ++i) a.render(out.data(), 735);
        std::vector<std::uint8_t> buf;
        state::Writer w{buf};
        gromada::SaveStateAccess::save(w, a, refNone);
        buf.resize(w.size());
        std::vector<std::int16_t> ra, rb;
        for (int i = 0; i < 120; ++i) {
            a.render(out.data(), 735);
            ra.insert(ra.end(), out.begin(), out.end());
        }
        Mixer b{44100};
        b.setMusicDirectories(dirs);
        state::Reader r{buf};
        gromada::SaveStateAccess::apply(b, gromada::SaveStateAccess::decode(r, lookupNone));
        for (int i = 0; i < 120; ++i) {
            b.render(out.data(), 735);
            rb.insert(rb.end(), out.begin(), out.end());
        }
        std::size_t diff = 0;
        for (std::size_t i = 0; i < ra.size(); ++i) diff += ra[i] != rb[i];
        std::fprintf(stderr, "music: restored after %d frames into a fresh mixer: %zu of %zu samples differ\n", frames, diff, ra.size());
        CHECK_EQ(diff, std::size_t{0});
    }
}

} // namespace

int main() {
    SKIP_WITHOUT_DATA();
    const auto game = *gromada_test::dataDir();
    GameResources res{game / "fw.res"};
    music(game);
    level(res, game, 19, true);
    level(res, game, 1, false);
    crossSession(res, game);
    demoPlayback(res, game);
    robustness(res, game);
    return gromada_test::finish();
}
