// Script module against the original game scripts (needs GROMADA_DATA; skipped otherwise).
//  - every maps/**/*.INI compiles (preprocess, parse, link) without errors;
//  - printing the AST and compiling the result gives the same AST (parser/printer consistency);
//  - the extern declarations of HEADER.INI match kKnownBuiltins;
//  - every script with main() runs for a while with a simulated host without runtime errors;
//  - levels/05.INI issues the expected builtin calls in its first ticks.
#include <algorithm>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include "script/printer.hpp"
#include "script/recording_host.hpp"
#include "script/script.hpp"
#include "test.hpp"

namespace fs = std::filesystem;
using namespace gromada::script;

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> gameScripts(const fs::path& root) {
    std::vector<std::string> out;
    for (const auto& e : fs::recursive_directory_iterator(root / "maps")) {
        if (!e.is_regular_file() || lower(e.path().extension().string()) != ".ini") continue;
        out.push_back(fs::relative(e.path(), root).generic_string());
    }
    std::ranges::sort(out);
    return out;
}

struct Run {
    RecordingHost host;
    SimulatedWorld world;
    std::unique_ptr<Interpreter> vm;
    Run(const Script& s, ExecConfig cfg = {}) {
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

// Level number from "maps/levels/NN.INI", or 0.
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
    CompileOptions opt;
    opt.predeclared = &known; // include-only fragments (music.ini) use builtins declared by HEADER.INI

    const auto files = gameScripts(root);
    CHECK(files.size() >= 40);
    int levels = 0, withMain = 0;
    for (const auto& path : files) {
        try {
            const Script s = Script::compile(loader, path, opt);
            // Parser/printer consistency on real data.
            const std::string printed = AstPrinter::program(s.ast());
            const Script again = Script::compileText(printed, opt, path + " (printed)");
            if (AstPrinter::program(again.ast()) != printed) {
                ++gromada_test::failures;
                std::fprintf(stderr, "%s: printing is not stable\n", path.c_str());
            }
            if (levelNumber(path)) ++levels;
            if (!s.findFunction("main")) continue;
            ++withMain;
            // Run from a fresh state, then (for levels) as if entered from the map menu.
            Run fresh{s};
            fresh.ticks(400);
            if (const int n = levelNumber(path)) {
                Run fromMenu{s};
                fromMenu.vm->set("n_map", Value{n});
                fromMenu.vm->set("centr_x", Value{320});
                fromMenu.vm->set("centr_y", Value{240});
                fromMenu.ticks(400);
                // The game menu prints the level's description file: maps\levels\NN.txt
                // (Tact() opens no menu on the last level).
                bool printedOwnText = false;
                for (const auto* c : fromMenu.host.callsTo("PrintFile"))
                    if (c->args.size() == 4 && lower(c->args[2].toString()).ends_with(std::format("{:02}.txt", n))) printedOwnText = true;
                if (printedOwnText != (n < 30)) {
                    ++gromada_test::failures;
                    std::fprintf(stderr, "%s: PrintFile of the level description %s\n", path.c_str(),
                                 printedOwnText ? "unexpected" : "missing");
                }
            }
        } catch (const ScriptError& e) {
            ++gromada_test::failures;
            std::fprintf(stderr, "%s\n", e.what());
        }
    }
    std::fprintf(stderr, "%zu scripts compiled, %d with main() run\n", files.size(), withMain);
    CHECK_EQ(levels, 30);
    CHECK(withMain >= 30);

    // HEADER.INI declares exactly the builtins of kKnownBuiltins.
    {
        const Script header = Script::compile(loader, "maps/HEADER.INI", opt);
        const auto all = header.builtins().all();
        CHECK_EQ(all.size(), kKnownBuiltins.size());
        for (std::size_t i = 0; i < all.size() && i < kKnownBuiltins.size(); ++i) {
            const auto& k = kKnownBuiltins[i];
            std::string sig;
            for (const auto& p : all[i]->params) sig += p.type == ParamType::Int ? 'i' : p.type == ParamType::String ? 's' : 'a';
            if (all[i]->id != toInt(k.id) || all[i]->name != k.name || sig != k.params) {
                ++gromada_test::failures;
                std::fprintf(stderr, "HEADER.INI: %s differs from the table (%d %.*s %.*s)\n", all[i]->signature().c_str(),
                             toInt(k.id), int(k.name.size()), k.name.data(), int(k.params.size()), k.params.data());
            }
        }
    }

    // Level 5 from a fresh state: the expected calls of its first tick and consistent arguments.
    {
        const Script s = Script::compile(loader, "maps/levels/05.INI", opt);
        ExecConfig cfg;
        cfg.traceUserFunctions = true;
        Run r{s, cfg};
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
            // PlayMusic (user function of music.ini) forwards its tracks to the PlayAudio builtin
            // and the level remembers them in music1..music4.
            CHECK_EQ(music[0]->args, audio[0]->args);
            CHECK_EQ(audio[0]->id, toInt(BuiltinId::PlayAudio));
            CHECK_EQ(audio[0]->args.size(), std::size_t{4});
            for (int i = 0; i < 4 && i < int(audio[0]->args.size()); ++i)
                CHECK_EQ(audio[0]->args[std::size_t(i)], r.vm->get(std::format("music{}", i + 1)));
            // Order within the tick: camera first, then the player's vehicle, then the music.
            const auto index = [&](const BuiltinCallRecord* c) { return c - h.calls().data(); };
            CHECK(index(pos[0]) < index(create[0]));
            CHECK(index(create[0]) < index(flag[0]));
            CHECK(index(flag[0]) < index(audio[0]));
        }
        // Over time the level keeps spawning objects (every 10 s of game time).
        const std::size_t createdAtStart = create.size();
        r.ticks(400); // 20 s at 50 ms per tick
        CHECK(h.callsTo("Create").size() > createdAtStart);
        // The initialization blocks ran exactly once.
        CHECK_EQ(h.callsTo("SetPosition").size(), std::size_t{1});
        CHECK_EQ(h.callsTo("PlayAudio").size(), std::size_t{1});
    }
    return gromada_test::finish();
}
