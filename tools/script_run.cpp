// script_run: compiles a Gromada script (.INI) and runs main() for N ticks with a logging host,
// printing every builtin call. Desktop-only developer tool.
//
//   script_run [options] maps/levels/05.INI
//
// Options:
//   --root DIR         game folder used to resolve #include "maps\..." (default: $GROMADA_DATA,
//                      else the folder above the script's "maps" folder, else the current folder)
//   --ticks N          number of main() calls (default 1)
//   --tick-ms MS       GetTime() advance per tick in the simulated world (default 50)
//   --set NAME=VALUE   set a global before the first tick; NAME[i]=VALUE for an element;
//                      VALUE is an integer or "text" (repeatable)
//   --iff MODE         once (default) | edge | if
//   --missing-args M   keep (default) | reset
//   --no-short-circuit evaluate both sides of && and ||
//   --stub             no simulated world: every builtin returns 0
//   --quiet A,B,...    do not print calls to these builtins (e.g. GetTime,GetKey)
//   --results          print the value returned by each builtin call
//   --trace            also print calls to user functions (prefixed with '>')
//   --globals          print every global variable after the run
//   --dump             print the parsed program (fully parenthesized) and exit
//   --builtins         print the builtin registry (id, signature) and exit
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <print>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "script/printer.hpp"
#include "script/recording_host.hpp"
#include "script/script.hpp"

namespace fs = std::filesystem;
using namespace gromada::script;

namespace {

[[noreturn]] void usage(std::string_view error) {
    if (!error.empty()) std::println(stderr, "script_run: {}", error);
    std::println(stderr, "usage: script_run [--root DIR] [--ticks N] [--tick-ms MS] [--set NAME=VALUE]... "
                         "[--iff once|edge|if] [--missing-args keep|reset] [--no-short-circuit] [--stub] "
                         "[--quiet A,B] [--results] [--trace] [--globals] [--dump] [--builtins] SCRIPT");
    std::exit(2);
}

fs::path inferRoot(const fs::path& script) {
    if (const char* d = std::getenv("GROMADA_DATA"); d && *d) return d;
    std::error_code ec;
    for (fs::path p = fs::absolute(script, ec).parent_path(); !p.empty(); p = p.parent_path()) {
        std::string name = p.filename().string();
        for (auto& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (name == "maps") return p.parent_path();
        if (p == p.root_path()) break;
    }
    return fs::current_path();
}

struct Assignment {
    std::string name;
    std::int32_t index = 0;
    Value value;
};

Assignment parseSet(std::string_view spec) {
    const auto eq = spec.find('=');
    if (eq == std::string_view::npos) usage("--set expects NAME=VALUE");
    Assignment a;
    std::string_view lhs = spec.substr(0, eq), rhs = spec.substr(eq + 1);
    if (const auto br = lhs.find('['); br != std::string_view::npos) {
        if (!lhs.ends_with(']')) usage("--set: malformed index in '" + std::string{lhs} + "'");
        a.index = stringToInt(lhs.substr(br + 1, lhs.size() - br - 2));
        lhs = lhs.substr(0, br);
    }
    a.name = std::string{lhs};
    if (rhs.size() >= 2 && rhs.front() == '"' && rhs.back() == '"') a.value = Value{rhs.substr(1, rhs.size() - 2)};
    else a.value = Value{stringToInt(rhs)};
    return a;
}

std::set<std::string, std::less<>> splitList(std::string_view s) {
    std::set<std::string, std::less<>> out;
    while (!s.empty()) {
        const auto c = s.find(',');
        out.emplace(s.substr(0, c));
        if (c == std::string_view::npos) break;
        s.remove_prefix(c + 1);
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    std::optional<fs::path> root;
    std::string scriptPath;
    int ticks = 1, tickMs = 50;
    std::vector<Assignment> sets;
    ExecConfig cfg;
    bool stub = false, results = false, printGlobals = false, dump = false, listBuiltins = false;
    std::set<std::string, std::less<>> quiet;

    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto value = [&]() -> std::string_view {
            if (i + 1 >= argc) usage(std::string{a} + " needs a value");
            return argv[++i];
        };
        if (a == "--root") root = fs::path{value()};
        else if (a == "--ticks") ticks = stringToInt(value());
        else if (a == "--tick-ms") tickMs = stringToInt(value());
        else if (a == "--set") sets.push_back(parseSet(value()));
        else if (a == "--iff") {
            const auto m = value();
            if (m == "once") cfg.iffMode = IffMode::OncePerSite;
            else if (m == "edge") cfg.iffMode = IffMode::RisingEdge;
            else if (m == "if") cfg.iffMode = IffMode::PlainIf;
            else usage("--iff expects once, edge or if");
        } else if (a == "--missing-args") {
            const auto m = value();
            if (m == "keep") cfg.missingArguments = MissingArguments::KeepPrevious;
            else if (m == "reset") cfg.missingArguments = MissingArguments::Reset;
            else usage("--missing-args expects keep or reset");
        } else if (a == "--no-short-circuit") cfg.shortCircuit = false;
        else if (a == "--stub") stub = true;
        else if (a == "--quiet") quiet = splitList(value());
        else if (a == "--results") results = true;
        else if (a == "--trace") cfg.traceUserFunctions = true;
        else if (a == "--globals") printGlobals = true;
        else if (a == "--dump") dump = true;
        else if (a == "--builtins") listBuiltins = true;
        else if (a == "-h" || a == "--help") usage("");
        else if (a.starts_with("--")) usage("unknown option " + std::string{a});
        else if (scriptPath.empty()) scriptPath = std::string{a};
        else usage("only one script can be run");
    }
    if (scriptPath.empty()) usage("no script given");
    if (!root) root = inferRoot(scriptPath);

    try {
        FileSystemSourceLoader loader{*root};
        const BuiltinRegistry known = knownBuiltinRegistry();
        CompileOptions copt;
        copt.predeclared = &known; // lets include-only fragments (music.ini...) run too
        const Script script = Script::compile(loader, scriptPath, copt);
        for (const auto& w : script.warnings()) std::println(stderr, "{}", w);

        if (dump) {
            std::print("{}", AstPrinter::program(script.ast()));
            return 0;
        }
        if (listBuiltins) {
            for (const BuiltinInfo* b : script.builtins().all()) std::println("{:4} {}", b->id, b->signature());
            return 0;
        }

        RecordingHost host;
        SimulatedWorld world;
        if (!stub) host.setResponder([&world](const BuiltinInfo& b, std::span<const Value> args) { return world(b, args); });

        std::size_t printed = 0, warned = 0;
        auto flush = [&] {
            const auto& calls = host.calls();
            for (; printed < calls.size(); ++printed) {
                const auto& c = calls[printed];
                if (quiet.contains(c.name)) continue;
                std::println("[{}] {}{}", c.tick, c.userFunction ? "> " : "", c.str(results));
            }
            for (; warned < host.warnings().size(); ++warned) std::println(stderr, "warning: {}", host.warnings()[warned]);
        };

        host.setTick(0);
        Interpreter vm{script, host, cfg}; // initializers may call builtins (tick 0)
        for (const auto& s : sets) {
            if (!vm.hasGlobal(s.name)) usage("--set: no global named '" + s.name + "'");
            vm.set(s.name, s.value, s.index);
        }
        flush();
        if (script.findFunction("main")) {
            for (int t = 1; t <= ticks; ++t) {
                host.setTick(std::uint64_t(t));
                const Value r = vm.tick();
                flush();
                if (results) std::println("[{}] main() -> {}", t, r.repr());
                world.timeMs += tickMs;
            }
        } else {
            std::println(stderr, "script_run: {} has no main(); nothing to run", scriptPath);
        }

        if (printGlobals) {
            for (const auto& g : script.globals()) {
                std::string line = g.name + " =";
                for (std::int32_t i = 0; i < g.size; ++i) line += " " + vm.get(g.name, i).repr();
                std::println("{}", line);
            }
        }
        std::map<std::string, int> counts;
        std::size_t builtinCalls = 0;
        for (const auto& c : host.calls())
            if (!c.userFunction) ++counts[c.name], ++builtinCalls;
        std::string summary;
        for (const auto& [name, n] : counts) summary += std::format(" {}={}", name, n);
        std::println(stderr, "{} builtin call(s) in {} tick(s):{}", builtinCalls, ticks, summary);
    } catch (const ScriptError& e) {
        std::println(stderr, "{}", e.what());
        return 1;
    } catch (const std::exception& e) {
        std::println(stderr, "script_run: {}", e.what());
        return 1;
    }
    return 0;
}
