// Script module against the original game scripts (needs GROMADA_DATA; skipped otherwise).
//  - every script compiles to exactly the bytecode and cell layout of the replica of the original
//    compiler (tools/re/runtime/gscript.py): sizes, counts and FNV-1a hashes below were produced
//    by gscript.py (tests/script_layout_compare.py does the full byte-by-byte comparison);
//  - the cell layout matches the int-cell snapshots recorded in the three demos;
//  - the externs of HEADER.INI match kKnownBuiltins;
//  - the iff statements reached by a jump are the known ones;
//  - every script runs for a while with a simulated host; a map change carries the cells over;
//  - levels/05.INI issues the expected builtin calls in its first frames.
#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "demo/demo_file.hpp"
#include "script/recording_host.hpp"
#include "script/script.hpp"
#include "test.hpp"

namespace fs = std::filesystem;
using namespace gromada::script;

namespace {

// From gscript.py: script, code bytes, cells, int cells, user functions, iff sites, iff sites
// reached by a jump, FNV-1a 64 of the code, FNV-1a 64 of the layout text
// ("<cell> <flags> <init|-|?> <name>\n" per cell).
struct Reference {
    const char* path;
    std::size_t codeSize, cells, intCells, functions, iffs, iffsByJump;
    std::uint64_t codeHash, layoutHash;
};
constexpr Reference kReference[] = {
    {"maps/MAPS.INI", 18742, 137, 133, 21, 10, 1, 0x45b9cdc01ad1748dull, 0xeb9273a301dcdb0aull},
    {"maps/NET.INI", 16661, 139, 135, 21, 10, 1, 0x2cec4f366a679b2dull, 0x261d4b6979766d9bull},
    {"maps/PLAYER.INI", 17248, 144, 139, 22, 10, 1, 0x1e8597bafae71697ull, 0x1a61fe568a445473ull},
    {"maps/credits.ini", 15020, 132, 129, 20, 10, 1, 0xf31fddbdb5f0921eull, 0xae149f5ac0abfc6cull},
    {"maps/levels/01.INI", 17194, 179, 176, 20, 13, 2, 0x080c3b217cc44ab1ull, 0x635e883e3c24757cull},
    {"maps/levels/02.INI", 17383, 154, 146, 20, 16, 1, 0x9d48958237d6582full, 0x4e0cb06e8283cceeull},
    {"maps/levels/03.INI", 16233, 153, 150, 20, 12, 1, 0xaa46d7d992fd2227ull, 0xf637c2994e2bffeeull},
    {"maps/levels/04.INI", 16530, 152, 149, 20, 12, 1, 0x4b82bade674c45d0ull, 0x2dc7da22e5c495e5ull},
    {"maps/levels/05.INI", 16964, 159, 156, 20, 16, 1, 0xb24161b918374b9dull, 0x9a25bffdd338a721ull},
    {"maps/levels/06.INI", 16449, 149, 146, 20, 13, 1, 0xd140669e4d401cb2ull, 0xa3dd330764edd8e8ull},
    {"maps/levels/07.INI", 15995, 134, 131, 20, 10, 1, 0xfe66ad661769cae8ull, 0x461dbb677595459eull},
    {"maps/levels/08.INI", 16258, 153, 150, 20, 13, 1, 0x9abfbf08e8c24fabull, 0xafa9c60a093aa69full},
    {"maps/levels/09.INI", 16169, 154, 151, 20, 12, 1, 0x0840227259281f4full, 0x56e2978737823092ull},
    {"maps/levels/10.INI", 16738, 160, 157, 20, 16, 1, 0x5b83741937057499ull, 0x9356aacbbec78f5dull},
    {"maps/levels/11.INI", 17205, 148, 145, 20, 17, 1, 0x1d7e52bca2f17347ull, 0x097e559cc52b33c2ull},
    {"maps/levels/12.INI", 20413, 189, 186, 20, 27, 2, 0x7ab5bac833be8a74ull, 0x9c6fd94fc2c9245cull},
    {"maps/levels/13.INI", 17783, 164, 161, 20, 12, 1, 0xf505de136b8ac7f5ull, 0x6febaf9bcb06eddaull},
    {"maps/levels/14.INI", 16246, 149, 146, 20, 12, 1, 0xcca92d1b323b0ae5ull, 0x07482ba8b8e2fa8dull},
    {"maps/levels/15.INI", 17417, 159, 156, 20, 14, 2, 0x7a82fbb4fcb3b2f2ull, 0x4f358c356b45fc96ull},
    {"maps/levels/16.INI", 17115, 223, 207, 20, 12, 1, 0xe80cac38a73dff97ull, 0x14542f3c731a9a27ull},
    {"maps/levels/17.INI", 16823, 181, 178, 20, 12, 1, 0x06f0ea7157ac715full, 0xc70be117cc7a8081ull},
    {"maps/levels/18.ini", 17851, 173, 169, 20, 14, 2, 0x8308962104448c59ull, 0x8ebbc6b9474c0fe9ull},
    {"maps/levels/19.INI", 18034, 172, 169, 20, 14, 2, 0xf81de100e0d99154ull, 0xbe5e2e4687c887c8ull},
    {"maps/levels/20.INI", 15193, 134, 131, 20, 10, 1, 0x2e8cdc01ed6d10a6ull, 0x461dbb677595459eull},
    {"maps/levels/21.INI", 15189, 134, 131, 20, 10, 1, 0x109f3affe78a0fdaull, 0x461dbb677595459eull},
    {"maps/levels/22.INI", 15189, 134, 131, 20, 10, 1, 0x109f3affe78a0fdaull, 0x461dbb677595459eull},
    {"maps/levels/23.INI", 15189, 134, 131, 20, 10, 1, 0x109f3affe78a0fdaull, 0x461dbb677595459eull},
    {"maps/levels/24.INI", 15189, 134, 131, 20, 10, 1, 0x109f3affe78a0fdaull, 0x461dbb677595459eull},
    {"maps/levels/25.INI", 15189, 134, 131, 20, 10, 1, 0x109f3affe78a0fdaull, 0x461dbb677595459eull},
    {"maps/levels/26.INI", 15189, 134, 131, 20, 10, 1, 0x109f3affe78a0fdaull, 0x461dbb677595459eull},
    {"maps/levels/27.INI", 17304, 230, 227, 20, 16, 2, 0xc90fdb7663193491ull, 0x67c2863c0a81fc31ull},
    {"maps/levels/28.INI", 15189, 134, 131, 20, 10, 1, 0x109f3affe78a0fdaull, 0x461dbb677595459eull},
    {"maps/levels/29.INI", 15189, 134, 131, 20, 10, 1, 0x109f3affe78a0fdaull, 0x461dbb677595459eull},
    {"maps/levels/30.INI", 16169, 149, 146, 20, 12, 1, 0x5bd2bfd8375fc64dull, 0xc96bcd3de244f144ull},
    {"maps/logo.ini", 14966, 133, 130, 20, 10, 1, 0x090d0add55e4b48dull, 0x3e378e477ad740feull},
    {"maps/mainmenu.ini", 15462, 135, 131, 20, 13, 1, 0x62f651cbea4e36f0ull, 0x2ae3e9d2e121469cull},
    {"maps/net/05.INI", 16700, 243, 240, 21, 10, 1, 0x7d72b8858465b1a8ull, 0xfd24eafc050fb815ull},
    {"maps/net/12.INI", 16711, 222, 219, 21, 10, 1, 0xf0f5068c41794f16ull, 0x3a706022d32f6de6ull},
    {"maps/win.ini", 15341, 135, 131, 20, 12, 1, 0xd9df40c855f76914ull, 0x2ae3e9d2e121469cull},
};

std::uint64_t fnv1a(std::string_view bytes) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (const unsigned char c : bytes) h = (h ^ c) * 0x100000001b3ull;
    return h;
}

std::string layoutText(const Script& s) {
    std::string t;
    for (std::size_t i = 0; i < s.cells().size(); ++i) {
        const auto& c = s.cells()[i];
        const std::string init = !c.initialized ? "-" : c.hasInitValue ? std::to_string(c.initValue) : "?";
        t += std::format("{} {} {} {}\n", i, int(c.flags), init, c.name);
    }
    return t;
}

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct Run {
    RecordingHost host;
    SimulatedWorld world;
    std::unique_ptr<Interpreter> vm;
    explicit Run(const Script& s, ExecConfig cfg = {}) {
        host.setResponder([this](const BuiltinInfo& b, std::span<const Value> a) { return world(b, a); });
        vm = std::make_unique<Interpreter>(s, host, cfg);
    }
    void ticks(int n, int ms = 50) {
        for (int i = 0; i < n; ++i) {
            host.setTick(vm->tickCount() + 1);
            vm->tick();
            world.timeMs += ms;
        }
    }
};

int levelNumber(const std::string& path) {
    const auto l = lower(path);
    if (!l.starts_with("maps/levels/")) return 0;
    return std::atoi(l.c_str() + std::string_view{"maps/levels/"}.size());
}

} // namespace

int main() {
    SKIP_WITHOUT_DATA();
    const fs::path root = *gromada_test::dataDir();
    FileSystemSourceLoader loader{root};
    const BuiltinRegistry known = knownBuiltinRegistry();
    CompileOptions fragment;
    fragment.predeclared = &known;
    fragment.requireMain = false;

    // Include-only fragments compile on their own; HEADER.INI declares exactly kKnownBuiltins.
    CompileOptions noMain;
    noMain.requireMain = false; // (with the builtins predeclared too, HEADER.INI would exceed 128 functions)
    const Script header = Script::compile(loader, "maps/HEADER.INI", noMain);
    for (const char* f : {"maps/LOCALE.INI", "maps/music.ini"}) {
        try {
            Script::compile(loader, f, fragment);
        } catch (const ScriptError& e) {
            ++gromada_test::failures;
            std::fprintf(stderr, "%s\n", e.what());
        }
    }
    {
        const auto decls = header.builtins().declarations();
        CHECK_EQ(decls.size(), kKnownBuiltins.size());
        std::size_t matched = 0;
        for (std::size_t i = 0; i < decls.size(); ++i) {
            const BuiltinInfo* b = decls[i];
            const KnownBuiltin* k = findKnownBuiltin(b->id);
            std::string sig;
            for (const auto& p : b->params) sig += p.type == ParamType::Int ? 'i' : p.type == ParamType::String ? 's' : 'a';
            if (k && k->name == b->name && k->params == sig) ++matched;
            else std::fprintf(stderr, "HEADER.INI: %s differs from kKnownBuiltins\n", b->signature().c_str());
        }
        CHECK_EQ(matched, kKnownBuiltins.size());
    }
    const std::size_t headerCells = header.cellCount();

    // Every script: same bytecode and layout as the replica; the header's cells come first.
    std::set<std::string> jumpIffs;
    std::map<std::string, Script> scripts;
    for (const Reference& ref : kReference) {
        try {
            const Script s = Script::compile(loader, ref.path);
            std::size_t ints = 0, byJump = 0;
            for (const auto& c : s.cells()) ints += (c.flags & cellflag::Int) ? 1 : 0;
            for (const auto& site : s.iffSites())
                if (site.reachableByJump) {
                    ++byJump;
                    {
                    std::string file = lower(*site.loc.file);
                    std::ranges::replace(file, '\\', '/');
                    jumpIffs.insert(std::format("{}:{}", file.substr(file.rfind('/') + 1), site.loc.line));
                }
                }
            const std::string_view code{reinterpret_cast<const char*>(s.code().data()), s.code().size()};
            const bool ok = s.code().size() == ref.codeSize && s.cellCount() == ref.cells && ints == ref.intCells &&
                            s.functions().size() == ref.functions && s.iffSiteCount() == ref.iffs &&
                            byJump == ref.iffsByJump && fnv1a(code) == ref.codeHash && fnv1a(layoutText(s)) == ref.layoutHash;
            if (!ok) {
                ++gromada_test::failures;
                std::fprintf(stderr, "%s: differs from gscript.py (code %zu cells %zu ints %zu funcs %zu iff %u)\n", ref.path,
                             s.code().size(), s.cellCount(), ints, s.functions().size(), s.iffSiteCount());
            }
            for (std::size_t i = 0; i < headerCells && i < s.cellCount(); ++i)
                if (s.cells()[i].name != header.cells()[i].name || s.cells()[i].flags != header.cells()[i].flags) {
                    ++gromada_test::failures;
                    std::fprintf(stderr, "%s: cell %zu is not the header's\n", ref.path, i);
                    break;
                }
            scripts.emplace(ref.path, s);
        } catch (const ScriptError& e) {
            ++gromada_test::failures;
            std::fprintf(stderr, "%s\n", e.what());
        }
    }
    CHECK_EQ(scripts.size(), std::size(kReference));
    // The iff statements whose start is a jump target (docs/re/runtime.md §1.7, §2.9).
    CHECK(jumpIffs == (std::set<std::string>{"header.ini:751", "01.ini:128", "12.ini:114", "15.ini:87", "18.ini:125",
                                             "19.ini:132", "27.ini:82"}));

    // Demos: the int cells recorded right after compiling the level's script.
    for (const char* demoName : {"demo1.dem", "demo2.dem", "demo3.dem"}) {
        std::ifstream in(root / "maps" / demoName, std::ios::binary);
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in), {}};
        const auto demo = gromada::demo::parseDemo(bytes);
        CHECK(demo.has_value());
        if (!demo) continue;
        std::string ini = demo->mapName;
        std::ranges::replace(ini, '\\', '/');
        ini = ini.substr(0, ini.size() - 4) + ".INI";
        const Script s = Script::compile(loader, ini);
        RecordingHost host;
        Interpreter vm{s, host};
        const auto fresh = vm.intGlobalValues();
        CHECK_EQ(fresh.size(), demo->intGlobals.size());
        // Same parse when the parser is given our count instead of finding it.
        CHECK(gromada::demo::parseDemo(bytes, fresh.size()).has_value());
        // Every initialized int cell holds its initializer in the recording.
        std::size_t k = 0, initialized = 0;
        for (const auto& c : s.cells()) {
            if (!(c.flags & cellflag::Int)) continue;
            if (c.initialized && k < demo->intGlobals.size()) {
                ++initialized;
                if (demo->intGlobals[k] != c.initValue) {
                    ++gromada_test::failures;
                    std::fprintf(stderr, "%s: cell %s = %u, initializer %u\n", demoName, c.name.c_str(), demo->intGlobals[k], c.initValue);
                }
            }
            ++k;
        }
        CHECK(initialized > 20);
        // Loaded into the interpreter, the header's variables read back by name.
        CHECK(vm.setIntGlobalValues(demo->intGlobals));
        CHECK_EQ(vm.get("n_map").toInt(), levelNumber(ini));
        CHECK_EQ(vm.get("centr_x").toInt(), 320);
        CHECK_EQ(vm.get("centr_y").toInt(), 240);
        CHECK(vm.intGlobalValues() == demo->intGlobals);
    }

    // Every script runs (fresh state, then levels as entered from the map menu).
    std::size_t warnings = 0;
    for (const auto& [path, s] : scripts) {
        try {
            Run fresh{s};
            fresh.ticks(400);
            warnings += fresh.host.warnings().size();
            if (const int n = levelNumber(path)) {
                Run fromMenu{s};
                fromMenu.vm->set("n_map", Value{n});
                fromMenu.vm->set("centr_x", Value{320});
                fromMenu.vm->set("centr_y", Value{240});
                fromMenu.ticks(400);
                warnings += fromMenu.host.warnings().size();
                // The game menu prints the level's description file maps\levels\NN.txt
                // (Tact() opens no menu on the last level).
                bool printedOwnText = false;
                for (const auto* c : fromMenu.host.callsTo("PrintFile"))
                    if (c->args.size() == 4 && lower(c->args[2].toString()).ends_with(std::format("{:02}.txt", n))) printedOwnText = true;
                if (printedOwnText != (n < 30)) {
                    ++gromada_test::failures;
                    std::fprintf(stderr, "%s: PrintFile of the level description %s\n", path.c_str(), printedOwnText ? "unexpected" : "missing");
                }
            }
        } catch (const ScriptError& e) {
            ++gromada_test::failures;
            std::fprintf(stderr, "%s: %s\n", path.c_str(), e.what());
        }
    }
    std::fprintf(stderr, "%zu scripts run, %zu runtime warnings\n", scripts.size(), warnings);
    CHECK_EQ(warnings, std::size_t{0});

    // Map change menu -> level: the cells carry over by index.
    {
        Run menu{scripts.at("maps/MAPS.INI")};
        menu.vm->set("n_map", Value{5});
        menu.ticks(50);
        const GlobalSnapshot before = menu.vm->snapshot();
        const Script& level = scripts.at("maps/levels/05.INI");
        RecordingHost host;
        Interpreter vm{level, host};
        vm.inheritGlobals(before);
        std::size_t kept = 0;
        for (std::size_t i = 0; i < level.cellCount(); ++i) {
            const CellDecl& c = level.cells()[i];
            const Cell& now = vm.cell(i);
            if (c.flags & cellflag::String) CHECK_EQ(vm.cellValue(i).toString(), std::string{});
            else if (c.initialized) CHECK_EQ(now.value, c.initValue);
            else if (!(before.cells[i].flags & cellflag::String) || i >= before.globalCount) {
                CHECK_EQ(now.value, before.cells[i].value);
                ++kept;
            } else {
                CHECK_EQ(now.value, 0u); // the menu's string cell, freed
            }
        }
        std::fprintf(stderr, "menu -> level 05: %zu int cells kept\n", kept);
        CHECK(kept > 80);
        CHECK_EQ(vm.get("n_map").toInt(), 5);
    }

    // Level 5 from a fresh state: the expected calls of its first frame.
    {
        ExecConfig cfg;
        cfg.traceUserFunctions = true;
        Run r{scripts.at("maps/levels/05.INI"), cfg};
        r.ticks(1);
        const auto& h = r.host;
        CHECK(h.callsTo("SetVid").size() > 10);
        CHECK_EQ(h.callsTo("SetGamma").size(), std::size_t{1});
        const auto pos = h.callsTo("SetPosition");
        const auto create = h.callsTo("Create");
        const auto flag = h.callsTo("SetFlagman");
        const auto music = h.callsTo("PlayMusic");
        const auto audio = h.callsTo("PlayAudio");
        CHECK_EQ(pos.size(), std::size_t{1});
        CHECK(create.size() >= 2);
        CHECK_EQ(flag.size(), std::size_t{1});
        CHECK_EQ(music.size(), std::size_t{1});
        CHECK_EQ(audio.size(), std::size_t{1});
        if (!pos.empty() && !create.empty() && !flag.empty() && !music.empty() && !audio.empty()) {
            // The player's vehicle (vid `kassandra`) is created where the camera is placed and made flagman.
            CHECK_EQ(create[0]->args.size(), std::size_t{4});
            CHECK_EQ(create[0]->args[0], r.vm->get("kassandra"));
            CHECK_EQ(create[0]->args[1], pos[0]->args[0]);
            CHECK_EQ(create[0]->args[2], pos[0]->args[1]);
            CHECK_EQ(flag[0]->args.at(1), create[0]->result);
            // PlayMusic (user function of music.ini) forwards its tracks to the PlayAudio builtin.
            CHECK_EQ(music[0]->args, audio[0]->args);
            CHECK_EQ(audio[0]->id, toInt(BuiltinId::PlayAudio));
            for (int i = 0; i < 4 && i < int(audio[0]->args.size()); ++i)
                CHECK_EQ(audio[0]->args[std::size_t(i)], r.vm->get(std::format("music{}", i + 1)));
            const auto index = [&](const BuiltinCallRecord* c) { return c - h.calls().data(); };
            CHECK(index(pos[0]) < index(create[0]));
            CHECK(index(create[0]) < index(flag[0]));
            CHECK(index(flag[0]) < index(audio[0]));
        }
        const std::size_t createdAtStart = create.size();
        r.ticks(400); // 20 s at 50 ms per frame: the level keeps spawning objects
        CHECK(h.callsTo("Create").size() > createdAtStart);
        CHECK_EQ(h.callsTo("SetPosition").size(), std::size_t{1}); // the initialization ran once
        CHECK_EQ(h.callsTo("PlayAudio").size(), std::size_t{1});
    }
    return gromada_test::finish();
}
