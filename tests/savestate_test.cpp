// Save states (issue #14), synthetic tests without game data: the binary reader/writer and
// checksum, the script VM (cells, iff patches, string memory), the engine (a battle of
// synthetic units: save, run, load, run again -> identical state bytes; run-ahead; fuzzed
// engine chunks never crash), the input mapper, the on-screen texts and the mixer (music
// position and looping voices restored sample-exactly, also into a fresh mixer).
#include "core/std.hpp"
#include "libgromada/libgromada_all.hpp"
#include "game/savestate.hpp"
#include "test.hpp"

#include <unistd.h>

using namespace gromada;
using namespace gromada::game;

namespace {

// ------------------------------------------------------------------ state_io
void testStateIo() {
    std::vector<std::uint8_t> buf;
    state::Writer w{buf};
    const auto c = w.beginChunk(state::fourcc("TEST"));
    w.u8(0xAB); w.i16(-2); w.u32(0xDEADBEEF); w.i64(-5); w.f32(1.5f); w.f64(-2.25); w.boolean(true);
    w.str("hello"); w.array(std::span<const int>{std::vector<int>{1, 2, 3}});
    w.endChunk(c);
    const std::vector<std::uint8_t> data{w.result().begin(), w.result().end()};
    state::Reader outer{data};
    state::Reader r = outer.chunk(state::fourcc("TEST"));
    CHECK_EQ(r.u8(), 0xAB);
    CHECK_EQ(r.i16(), -2);
    CHECK_EQ(r.u32(), 0xDEADBEEFu);
    CHECK_EQ(r.i64(), -5);
    CHECK_EQ(r.f32(), 1.5f);
    CHECK_EQ(r.f64(), -2.25);
    CHECK(r.boolean());
    CHECK_EQ(r.str(), std::string{"hello"});
    CHECK(r.array<int>() == (std::vector<int>{1, 2, 3}));
    CHECK(r.atEnd());
    CHECK(outer.atEnd());
    const auto throws = [](auto&& fn) {
        try { fn(); } catch (const state::Error&) { return true; }
        return false;
    };
    // every truncation throws; a wrong tag throws; absurd counts throw before allocating
    for (std::size_t n = 0; n < data.size(); ++n)
        CHECK(throws([&] {
            state::Reader o{std::span{data}.first(n)};
            state::Reader rr = o.chunk(state::fourcc("TEST"));
            rr.u8(); rr.i16(); rr.u32(); rr.i64(); rr.f32(); rr.f64(); rr.boolean(); rr.str(); rr.array<int>();
        }));
    CHECK(throws([&] { state::Reader o{data}; o.chunk(state::fourcc("NOPE")); }));
    const std::vector<std::uint8_t> big{0xFF, 0xFF, 0xFF, 0x7F};
    CHECK(throws([&] { state::Reader o{big}; o.array<std::uint64_t>(); }));
    const std::vector<std::uint8_t> nan{0, 0, 0xC0, 0x7F};
    CHECK(throws([&] { state::Reader o{nan}; o.finite32(); }));
    const std::vector<std::uint8_t> two{2};
    CHECK(throws([&] { state::Reader o{two}; o.boolean(); }));
    // checksum: any single bit flip changes it
    std::vector<std::uint8_t> blob(1000);
    for (std::size_t i = 0; i < blob.size(); ++i) blob[i] = static_cast<std::uint8_t>(i * 7);
    const auto sum = state::checksum64(blob);
    int same = 0;
    for (std::size_t i = 0; i < blob.size(); i += 13) {
        blob[i] ^= 4;
        same += state::checksum64(blob) == sum;
        blob[i] ^= 4;
    }
    CHECK_EQ(same, 0);
    CHECK(state::checksum64(std::span{blob}.first(999)) != sum);
    CHECK_EQ(state::crc32(std::span{reinterpret_cast<const std::uint8_t*>("123456789"), 9}), 0xCBF43926u);
}

// ------------------------------------------------------------------ script VM
struct CountingHost final : script::ScriptHost {
    std::int32_t counter = 0;
    script::Value callBuiltin(const script::BuiltinInfo& b, std::span<const script::Value> args) override {
        if (b.id == 88) return script::Value{++counter};                                       // GetKey
        if (b.id == 124) return script::Value{"vid" + std::to_string(script::argInt(args, 0))}; // GetVidName
        if (b.id == 103) return script::Value{static_cast<std::int32_t>(script::argString(args, 0).size())};
        return script::Value{0};
    }
};

void testVm() {
    using namespace gromada::script;
    const std::string src =
        "extern GetKey() 88;\nextern GetVidName(int nvid) 124;\nextern strlen(string str) 103;\n"
        "int n, k, j, v[4], fired; string s, t;\n"
        "main(){ n = n + GetKey(); j = n % 4; v[j]=v[j]+n; s = s + \"x\"; if (strlen(s) > 140) s = \"\";\n"
        "  t = GetVidName(n % 3) + s; iff(n > 30) fired = fired + 1; iff(n > 60) k = 7; }\n";
    const Script script = Script::compileText(src, {}, "<vm>");
    CountingHost host;
    Interpreter vm{script, host};
    for (int i = 0; i < 5; ++i) vm.tick();
    std::vector<std::uint8_t> buf;
    {
        state::Writer w{buf};
        script::SaveStateAccess::save(w, vm);
        buf.resize(w.size());
    }
    const std::int32_t counterAtSave = host.counter;
    const auto runCells = [&] {
        std::vector<std::vector<Cell>> out;
        for (int i = 0; i < 160; ++i) {
            vm.tick();
            out.emplace_back(vm.cells().begin(), vm.cells().end());
        }
        return std::pair{out, std::vector<std::uint8_t>(vm.code().begin(), vm.code().end())};
    };
    const auto a = runCells();
    CHECK(vm.iffFired(0) && vm.iffFired(1));
    const std::string sAfter = vm.get("s").toString(), tAfter = vm.get("t").toString();
    state::Reader r{buf};
    auto staged = script::SaveStateAccess::decode(r, vm.script());
    script::SaveStateAccess::apply(vm, std::move(staged));
    host.counter = counterAtSave;
    CHECK(!vm.iffFired(0));   // iff patches and counters are restored too
    const auto b = runCells();
    CHECK(a.first == b.first);
    CHECK(a.second == b.second);
    CHECK_EQ(vm.get("s").toString(), sAfter);
    CHECK_EQ(vm.get("t").toString(), tAfter);
    // another script refuses the state
    const Script other = Script::compileText("int n; main(){ n = n + 1; }", {}, "<other>");
    bool refused = false;
    try {
        state::Reader r2{buf};
        script::SaveStateAccess::decode(r2, other);
    } catch (const state::Error&) {
        refused = true;
    }
    CHECK(refused);
}

// ------------------------------------------------------------------ engine
struct VidSpec {
    std::uint8_t cls = cls::Vehicle;
    std::uint8_t category = cat::Vehicle;
    std::uint16_t flags = vf::PresentOnGrid;
    int size = 20, sizeZ = 10, maxHP = 30, speedX = 60, speedY = 0, accel = 200, dirs = 32, rot = 35, zLayer = 5;
    int army = 1, weapon = 0, visibility = 0, deathDamage = 0, deathRadius = 0;
    std::array<std::uint8_t, 16> anim{};
};

Vid makeVid(const VidSpec& s) {
    Vid v;
    v.category = static_cast<ObjectCategory>(s.category);
    v.type = static_cast<ObjectClass>(s.cls);
    v.flags = static_cast<ObjectFlags>(s.flags);
    v.collisionMask = 1;
    v.sizeX = v.sizeY = static_cast<std::uint16_t>(s.size);
    v.sizeZ = static_cast<std::uint16_t>(s.sizeZ);
    v.maxHP = static_cast<std::uint8_t>(s.maxHP);
    v.visibilityRadius = static_cast<std::uint16_t>(s.visibility);
    v.speedX = static_cast<std::uint16_t>(s.speedX);
    v.speedY = static_cast<std::uint16_t>(s.speedY);
    v.acceleration = static_cast<std::uint16_t>(s.accel);
    v.rotationPeriod = static_cast<std::uint8_t>(s.rot);
    v.directionsCount = static_cast<std::uint8_t>(s.dirs);
    v.z_layer = static_cast<std::uint8_t>(s.zLayer);
    v.army = static_cast<std::uint8_t>(s.army);
    v.someWeaponIndex = static_cast<std::uint8_t>(s.weapon);
    v.deathDamage = static_cast<std::uint8_t>(s.deathDamage);
    v.deathDamageRadius = static_cast<std::uint16_t>(s.deathRadius);
    v.animationLengths = s.anim;
    for (auto& a : v.animationLengths)
        if (!a) a = 2;
    return v;
}

struct Battle {
    std::vector<Vid> vids;
    std::vector<Weapon> weapons;
    std::unique_ptr<GameEngine> e;
    int frame = 0;

    Battle() {
        weapons.resize(2);
        weapons[1] = Weapon{.targetCategory = ObjectCategory::Vehicle, .flags = 0, .alwaysZero = 0, .weaponRange = 200,
                            .scatter = 4, .cooldown = 50};
        VidSpec tank{.army = 1, .weapon = 1, .visibility = 300};
        vids.push_back(makeVid(tank));   // 0: army 1
        tank.army = 0;
        vids.push_back(makeVid(tank));   // 1: army 0
        vids.push_back(makeVid({.cls = cls::Projectile, .category = cat::Cannon, .flags = vf::Gravity, .size = 4, .maxHP = 1,
                                .speedX = 400, .speedY = 30, .deathDamage = 6, .deathRadius = 30}));   // 2: shell
        vids.push_back(makeVid({.cls = cls::Static, .category = cat::Object, .flags = vf::PresentOnGrid | vf::Randomized,
                                .size = 30, .maxHP = 50, .speedX = 0, .zLayer = 3}));                    // 3: rock
        vids.push_back(makeVid({.cls = cls::Kassandra, .speedX = 100, .accel = 200, .army = 0}));     // 4: flagman
        for (int t : {0, 1}) {
            vids[static_cast<std::size_t>(t)].childNvid[act::fight] = 2;
            vids[static_cast<std::size_t>(t)].childrenCount[act::fight] = 1;
        }
        e = std::make_unique<GameEngine>(vids, weapons);
        e->camera().width = 640;
        e->camera().height = 480;
        e->loadEmptyMap(3000, 3000, 77);
        World& w = e->world();
        for (int i = 0; i < 12; ++i) {
            w.createUnit(0, 600 + 40 * i, 800, 0, 0, 0, nullptr);
            w.createUnit(1, 600 + 40 * i, 1100, 0, 0, 128, nullptr);
            w.createUnit(3, 300 + 200 * i, 300 + 150 * (i % 5), 0, 0, 0, nullptr);
        }
        Sprite* f = w.createUnit(4, 1500, 1500, 0, 0, 0, nullptr);
        w.players()[0].type = 1;
        w.players()[0].flagman = f->id;
    }

    // One frame; the operations depend on the frame number and the world only.
    void step() {
        using script::Value;
        OriginalInput in;
        in.mouseScreenX = static_cast<std::int16_t>(320 + (frame * 7) % 200);
        in.mouseScreenY = static_cast<std::int16_t>(240 + (frame * 3) % 100);
        in.mouseWorldX = static_cast<std::int16_t>(in.mouseScreenX + e->camera().x);
        in.mouseWorldY = static_cast<std::int16_t>(in.mouseScreenY + e->camera().y);
        if (frame % 50 == 0) in.bits |= OriginalInput::LClick | OriginalInput::LDown;
        if (frame % 70 > 50) in.bits |= OriginalInput::RDown | (frame % 70 == 51 ? OriginalInput::RClick : 0);
        e->beginFrame(frame % 3 ? 17 : 16);
        e->setInput(in);
        const auto call = [&](int id, std::initializer_list<Value> a) {
            std::vector<Value> v(a);
            return e->callBuiltin(id, v).value_or(Value{0}).toInt();
        };
        if (frame % 25 == 0) call(68, {frame % 2, 500 + call(90, {1500}), 900 + call(90, {300}), 0});   // Create(Random)
        if (frame % 40 == 5) {
            const int u = call(95, {frame % 2, cat::Vehicle});   // FirstUnit
            const int t = call(95, {1 - frame % 2, cat::Vehicle});
            if (u && t) call(87, {u, act::attack, t, 0});         // AddCommand
        }
        if (frame % 33 == 7)
            if (const int u = call(95, {1, cat::Vehicle})) call(67, {u, act::damage, 9, 0});
        if (frame == 60) call(127, {0, 1, 45});                  // SetVid maxHP
        if (frame == 70) call(127, {2, 22, vf::Gravity | vf::Wind});
        if (frame == 80) call(79, {1, 40});                      // MaxUnit
        if (frame == 90) call(107, {3, 64});                     // SetWind
        if (frame == 100) call(89, {1});                         // Pause
        if (frame == 130) call(89, {0});
        if (frame == 140) call(80, {1});                         // Effect
        if (frame == 150) call(106, {0x579});                    // SetGamma
        e->tactWorld();
        e->tactPlayers();
        e->tactGroupAi();
        e->updateCamera();
        e->takeSounds();
        ++frame;
    }

    std::vector<std::uint8_t> save() const {
        std::vector<std::uint8_t> buf;
        state::Writer w{buf};
        game::SaveStateAccess::saveEngine(w, *e);
        buf.resize(w.size());
        return buf;
    }
    void load(const std::vector<std::uint8_t>& buf, int f) {
        state::Reader r{buf};
        auto st = game::SaveStateAccess::decodeEngine(r, *e);
        game::SaveStateAccess::applyEngine(*e, std::move(st));
        frame = f;
    }
};

void testEngine() {
    Battle b;
    for (int i = 0; i < 40; ++i) b.step();
    const auto s0 = b.save();
    const std::size_t live0 = b.e->world().liveCount();
    std::vector<std::vector<std::uint8_t>> h1;
    for (int i = 0; i < 200; ++i) {
        b.step();
        h1.push_back(b.save());
    }
    std::fprintf(stderr, "battle: %zu -> %zu sprites, %llu shots\n", live0, b.e->world().liveCount(),
                 static_cast<unsigned long long>(b.e->world().fireCount));
    CHECK(b.e->world().fireCount > 0);
    b.load(s0, 40);
    CHECK(b.save() == s0);
    bool same = true;
    for (int i = 0; i < 200; ++i) {
        b.step();
        same &= b.save() == h1[static_cast<std::size_t>(i)];
    }
    CHECK(same);

    // into another engine (its own pool): the same future
    Battle other;
    for (int i = 0; i < 5; ++i) other.step();
    other.load(s0, 40);
    same = true;
    for (int i = 0; i < 200; ++i) {
        other.step();
        same &= other.save() == h1[static_cast<std::size_t>(i)];
    }
    CHECK(same);

    // run-ahead pattern: run, save, run one more, load
    b.load(s0, 40);
    same = true;
    for (int i = 0; i < 120; ++i) {
        b.step();
        same &= b.save() == h1[static_cast<std::size_t>(i)];
        const auto t = b.save();
        b.step();
        b.load(t, b.frame - 1);
    }
    CHECK(same);

    // fuzzed engine chunks: refused or accepted, never a crash; accepted ones keep running
    std::uint64_t x = 0xA0761D6478BD642Full;
    auto rnd = [&] { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
    int accepted = 0;
    for (int i = 0; i < 1500; ++i) {
        auto bad = s0;
        if (i % 5 == 4) bad.resize(rnd() % bad.size());
        else
            for (int k = 0; k < 1 + static_cast<int>(rnd() % 3); ++k)
                bad[8 + rnd() % (bad.size() - 8)] ^= static_cast<std::uint8_t>(1u << (rnd() % 8));
        try {
            other.load(bad, 40);
        } catch (const state::Error& e) {
            if (std::getenv("FUZZ_REASONS")) std::fprintf(stderr, "%s\n", e.what());
            continue;
        }
        ++accepted;
        for (int k = 0; k < 10; ++k) other.step();
    }
    std::fprintf(stderr, "engine fuzz: %d of 1500 corrupted chunks accepted (and ran)\n", accepted);
    other.load(s0, 40);
    CHECK(other.save() == s0);
}

// ------------------------------------------------------------------ input mapper
void testMapper() {
    using namespace gromada::input;
    auto rawAt = [](int f) {
        RawInput in;
        in.leftStick = {static_cast<float>(std::sin(f * 0.1)), static_cast<float>(std::cos(f * 0.07))};
        if (f % 20 < 3) in.set(pad::A);
        if (f % 31 < 2) in.set(pad::R);
        if (f % 50 > 40) {
            in.touchCount = 1;
            in.touches[0] = {0.3f + f * 0.001f, 0.6f};
        }
        in.mouseDelta = {static_cast<float>(f % 5 - 2), 0};
        return in;
    };
    FrameInfo fi;
    const auto run = [&](Mapper& m) {
        std::vector<std::string> out;
        for (int f = 60; f < 200; ++f) {
            const PcState& s = m.update(rawAt(f), fi);
            out.push_back(std::format("{} {} {} {} {} {}", s.cursor.x, s.cursor.y, s.left.pressed, s.left.down, s.weaponSlot,
                                      s.keys[0].pressed));
        }
        return out;
    };
    Mapper m;
    for (int f = 0; f < 60; ++f) m.update(rawAt(f), fi);
    std::vector<std::uint8_t> buf;
    {
        state::Writer w{buf};
        input::SaveStateAccess::save(w, m);
        buf.resize(w.size());
    }
    const auto a = run(m);
    state::Reader r{buf};
    input::SaveStateAccess::apply(m, input::SaveStateAccess::decode(r));
    CHECK(a == run(m));
    Mapper fresh;
    state::Reader r2{buf};
    input::SaveStateAccess::apply(fresh, input::SaveStateAccess::decode(r2));
    CHECK(a == run(fresh));
}

// ------------------------------------------------------------------ texts
void testTexts() {
    ui::TextManager t{[](int) { return ui::TypewriterTiming{.stepSeconds = 0.05f, .lineBreakSeconds = 0.15f, .charSfx = 23, .lineSfx = 24}; }};
    const int h = t.print(10, 20, "Hello\r\nworld of Gromada", 127);
    t.setVisibleCount(h, 0);
    for (int i = 0; i < 5; ++i) t.update(1.0f / 60);
    std::vector<std::uint8_t> buf;
    {
        state::Writer w{buf};
        ui::SaveStateAccess::save(w, t);
        buf.resize(w.size());
    }
    const auto run = [&] {
        std::vector<std::size_t> v;
        std::vector<int> sounds;
        for (int i = 0; i < 120; ++i) {
            t.update(1.0f / 60);
            v.push_back(t.find(h)->visible);
            for (const int s : t.takeSounds()) sounds.push_back(s);
        }
        return std::pair{v, sounds};
    };
    const auto a = run();
    state::Reader r{buf};
    ui::SaveStateAccess::apply(t, ui::SaveStateAccess::decode(r));
    CHECK(a == run());
}

// ------------------------------------------------------------------ mixer
// A mono 16-bit WAV in memory -> SoundData (its only constructor reads a RIFF file).
SoundData makeSound(const std::vector<std::int16_t>& samples, std::uint32_t rate) {
    std::string bytes;
    auto u32 = [&](std::uint32_t v) { for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<char>(v >> (8 * i))); };
    auto u16 = [&](std::uint16_t v) { for (int i = 0; i < 2; ++i) bytes.push_back(static_cast<char>(v >> (8 * i))); };
    const auto dataSize = static_cast<std::uint32_t>(samples.size() * 2);
    bytes += "RIFF"; u32(36 + dataSize); bytes += "WAVE";
    bytes += "fmt "; u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
    bytes += "data"; u32(dataSize);
    for (auto v : samples) u16(static_cast<std::uint16_t>(v));
    std::istringstream in{bytes};
    BinaryStreamReader reader{in, bytes.size()};
    return SoundData{reader};
}

void writeWav(const std::filesystem::path& path, int seconds) {
    const int rate = 22050, n = rate * seconds;
    std::vector<std::int16_t> pcm(static_cast<std::size_t>(n) * 2);
    for (int i = 0; i < n; ++i) {
        pcm[static_cast<std::size_t>(2 * i)] = static_cast<std::int16_t>(8000 * std::sin(i * 0.031) + 3000 * std::sin(i * 0.0007));
        pcm[static_cast<std::size_t>(2 * i + 1)] = static_cast<std::int16_t>(i * 13);
    }
    std::ofstream out{path, std::ios::binary};
    const auto p32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    const auto p16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4); p32(36 + n * 4); out.write("WAVEfmt ", 8); p32(16); p16(1); p16(2); p32(rate); p32(rate * 4);
    p16(4); p16(16); out.write("data", 4); p32(n * 4);
    out.write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(pcm.size() * 2));
}

void testMixer() {
    const auto dir = std::filesystem::temp_directory_path() / std::format("gromada_savestate_mixer_{}", ::getpid());
    std::filesystem::create_directories(dir);
    writeWav(dir / "track02.wav", 3);
    std::vector<std::int16_t> wave(3000);
    for (std::size_t i = 0; i < wave.size(); ++i) wave[i] = static_cast<std::int16_t>(static_cast<int>(i % 50) * 400 - 10000);
    const std::vector<SoundData> sounds{makeSound(wave, 11025)};
    const auto ref = [&](const SoundData* p) -> std::int32_t { return p == &sounds[0] ? 1 : -1; };
    const auto lookup = [&](std::int32_t id) -> const SoundData* { return id == 1 ? &sounds[0] : nullptr; };

    Mixer m{44100};
    m.setMusicDirectories({dir});
    m.playMusic({2, 2, 2, 2});
    m.playSfx(sounds[0], 0.7f, -0.3f, true);   // looping
    std::vector<std::int16_t> out(735 * 2);
    for (int i = 0; i < 100; ++i) m.render(out.data(), 735);   // 1.7 s
    m.playSfx(sounds[0], 0.5f, 0.2f);
    std::vector<std::uint8_t> buf;
    {
        state::Writer w{buf};
        gromada::SaveStateAccess::save(w, m, ref);
        buf.resize(w.size());
    }
    const auto run = [&](Mixer& mx) {
        std::vector<std::int16_t> all;
        for (int i = 0; i < 150; ++i) {   // crosses the end of the track (it restarts)
            mx.render(out.data(), 735);
            all.insert(all.end(), out.begin(), out.end());
        }
        return all;
    };
    const auto a = run(m);
    state::Reader r{buf};
    gromada::SaveStateAccess::apply(m, gromada::SaveStateAccess::decode(r, lookup));   // reopened, decoded forward
    CHECK(a == run(m));
    Mixer fresh{44100};
    fresh.setMusicDirectories({dir});
    state::Reader r2{buf};
    gromada::SaveStateAccess::apply(fresh, gromada::SaveStateAccess::decode(r2, lookup));   // opened, seek
    CHECK(a == run(fresh));
    // a small step back inside the decoded window (run-ahead, rewind): only the position moves
    Mixer near{44100};
    near.setMusicDirectories({dir});
    near.playMusic({2, 2, 2, 2});
    for (int i = 0; i < 50; ++i) near.render(out.data(), 735);
    std::vector<std::uint8_t> nb;
    {
        state::Writer w{nb};
        gromada::SaveStateAccess::save(w, near, ref);
        nb.resize(w.size());
    }
    const auto c = run(near);
    state::Reader r3{nb};
    gromada::SaveStateAccess::apply(near, gromada::SaveStateAccess::decode(r3, lookup));
    CHECK(c == run(near));
    std::filesystem::remove_all(dir);
}

} // namespace

int main() {
    testStateIo();
    testVm();
    testEngine();
    testMapper();
    testTexts();
    testMixer();
    return gromada_test::finish();
}
