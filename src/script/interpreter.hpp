#pragma once
// Tree-walking interpreter of a compiled Script.
//
// The semantics that are not yet confirmed by reverse engineering of the original runtime
// (issue #3) are collected in ExecConfig and documented in docs/script-language.md.
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "script/builtins.hpp"
#include "script/compiler.hpp"

namespace gromada::script {

// How `iff (cond) stmt` behaves. Each iff statement in the source is a "site" with its own state.
enum class IffMode : std::uint8_t {
    // Default: the body runs the first time cond is true, never again (until resetIffs()).
    // Matches every use in the game (`iff(1) {...; return;}` chains run one block per tick).
    OncePerSite,
    // The body runs whenever cond goes from false to true (cond evaluated on every pass;
    // the state before the first evaluation is "false").
    RisingEdge,
    // Plain `if` (for experiments).
    PlainIf,
};

// What a user function's parameter gets when the call passes fewer arguments
// (the game calls NextMap(demomap) although NextMap has two parameters).
enum class MissingArguments : std::uint8_t {
    KeepPrevious, // default: parameters are static storage; unpassed ones keep their last value
    Reset,        // unpassed parameters are reset to 0 / ""
};

enum class OutOfRangePolicy : std::uint8_t {
    Warn,  // default: reads give 0/"", writes are ignored, ScriptHost::warning is called
    Error, // throw ScriptError(Runtime)
};

struct ExecConfig {
    IffMode iffMode = IffMode::OncePerSite;
    // OncePerSite only: evaluate the condition (for its side effects) even after the site fired.
    bool iffEvaluateWhenFired = false;
    // && and || stop evaluating as soon as the result is known (C). False: both sides are always
    // evaluated (matters only for builtins with side effects).
    bool shortCircuit = true;
    MissingArguments missingArguments = MissingArguments::KeepPrevious;
    // Value of `return;`, of a function that ends without return, and of main().
    std::int32_t defaultReturn = 0;
    // Characters read from strings (`s[i]`) are 0..255 (false) or -128..127 like MSVC's char (true).
    bool signedChars = false;
    OutOfRangePolicy outOfRange = OutOfRangePolicy::Warn;
    // Safety limits per top-level entry (tick/call): statements+loop iterations, and call depth.
    std::uint64_t maxSteps = 10'000'000;
    std::uint32_t maxCallDepth = 200;
    // Report user function calls to ScriptHost::userFunctionCalled (debugging, tests).
    bool traceUserFunctions = false;
};

// Values of global variables, by name, to carry state from one script to the next
// (the game passes n_map, n_player, map... between the menu and the level scripts).
struct GlobalSnapshot {
    struct Entry {
        ValueType type = ValueType::Int;
        std::vector<std::int32_t> ints;
        std::vector<std::string> strings;
    };
    std::map<std::string, Entry> vars;
};

// Which variables inheritGlobals() copies. The defaults follow the analysis of the game scripts
// documented in docs/script-language.md; they are an open question for issue #3.
struct CarryOverPolicy {
    bool ints = true;
    bool strings = false;
    // Variables declared with an initializer (`int firstmenu=1;`) keep their initial value
    // unless this is true.
    bool overwriteInitialized = false;
};

class Interpreter {
public:
    // Allocates the globals and runs their initializers (which may call builtins on `host`).
    Interpreter(Script script, ScriptHost& host, ExecConfig config = {})
        : script_{std::move(script)}, host_{host}, cfg_{config} {
        const auto& globals = script_.globals();
        vars_.resize(globals.size());
        for (std::size_t i = 0; i < globals.size(); ++i) {
            const auto& g = globals[i];
            auto& v = vars_[i];
            v.type = g.type;
            if (g.type == ValueType::Int) v.ints.assign(std::size_t(g.size), 0);
            else v.strings.assign(std::size_t(g.size), std::string{});
        }
        iffState_.assign(script_.iffSiteCount(), 0);
        beginEntry();
        for (std::size_t i = 0; i < globals.size(); ++i) {
            const auto& g = globals[i];
            if (!g.hasInitializer()) continue;
            if (g.decl->init) store(static_cast<std::int32_t>(i), 0, eval(*g.decl->init), g.loc);
            for (std::size_t k = 0; k < g.decl->arrayInit.size(); ++k)
                store(static_cast<std::int32_t>(i), static_cast<std::int32_t>(k), eval(*g.decl->arrayInit[k]), g.loc);
        }
    }

    const Script& script() const { return script_; }
    const ExecConfig& config() const { return cfg_; }
    ExecConfig& config() { return cfg_; }

    // Runs main() once. The host decides the cadence (see docs: main() is believed to run once
    // per game logic tick). Returns main's return value.
    Value tick() {
        const FunctionDef* m = script_.findFunction("main");
        if (!m) throw ScriptError(ScriptError::Phase::Runtime, {}, "the script has no main() function");
        ++ticks_;
        beginEntry();
        return invoke(*m, {}, m->loc);
    }
    std::uint64_t tickCount() const { return ticks_; }

    // Calls a user function by name.
    Value call(std::string_view name, std::span<const Value> args = {}) {
        const FunctionDef* f = script_.findFunction(name);
        if (!f) throw ScriptError(ScriptError::Phase::Runtime, {}, "no function named '" + std::string{name} + "'");
        beginEntry();
        return invoke(*f, args, f->loc);
    }

    // ---- globals ----
    bool hasGlobal(std::string_view name) const { return script_.findGlobal(name).has_value(); }
    Value get(std::string_view name, std::int32_t index = 0) const {
        const auto slot = requireGlobal(name);
        const auto& v = vars_[std::size_t(slot)];
        const auto n = v.type == ValueType::Int ? v.ints.size() : v.strings.size();
        if (index < 0 || std::size_t(index) >= n) throw std::out_of_range("index out of range for '" + std::string{name} + "'");
        return v.type == ValueType::Int ? Value{v.ints[std::size_t(index)]} : Value{v.strings[std::size_t(index)]};
    }
    void set(std::string_view name, const Value& value, std::int32_t index = 0) {
        const auto slot = requireGlobal(name);
        const auto& v = vars_[std::size_t(slot)];
        const auto n = v.type == ValueType::Int ? v.ints.size() : v.strings.size();
        if (index < 0 || std::size_t(index) >= n) throw std::out_of_range("index out of range for '" + std::string{name} + "'");
        store(slot, index, value, {});
    }

    GlobalSnapshot snapshot() const {
        GlobalSnapshot s;
        const auto& globals = script_.globals();
        for (std::size_t i = 0; i < globals.size(); ++i) {
            auto& e = s.vars[globals[i].name];
            e.type = vars_[i].type;
            e.ints = vars_[i].ints;
            e.strings = vars_[i].strings;
        }
        return s;
    }

    // Copies values of same-named, same-typed globals from a previous script (overlapping
    // elements for arrays of different size). Call before the first tick().
    void inheritGlobals(const GlobalSnapshot& previous, const CarryOverPolicy& policy = {}) {
        const auto& globals = script_.globals();
        for (std::size_t i = 0; i < globals.size(); ++i) {
            const auto it = previous.vars.find(globals[i].name);
            if (it == previous.vars.end() || it->second.type != vars_[i].type) continue;
            if (globals[i].hasInitializer() && !policy.overwriteInitialized) continue;
            auto& v = vars_[i];
            if (v.type == ValueType::Int && policy.ints) {
                const auto n = std::min(v.ints.size(), it->second.ints.size());
                std::copy_n(it->second.ints.begin(), n, v.ints.begin());
            } else if (v.type == ValueType::String && policy.strings) {
                const auto n = std::min(v.strings.size(), it->second.strings.size());
                std::copy_n(it->second.strings.begin(), n, v.strings.begin());
            }
        }
    }

    // ---- iff ----
    bool iffFired(std::uint32_t site) const { return iffState_.at(site) != 0; }
    void resetIffs() { std::ranges::fill(iffState_, std::uint8_t{0}); }

private:
    struct Storage {
        ValueType type = ValueType::Int;
        std::vector<std::int32_t> ints;
        std::vector<std::string> strings;
        std::size_t size() const { return type == ValueType::Int ? ints.size() : strings.size(); }
    };

    enum class Flow : std::uint8_t { Normal, Break, Continue, Return };

    std::int32_t requireGlobal(std::string_view name) const {
        const auto slot = script_.findGlobal(name);
        if (!slot) throw std::out_of_range("no global named '" + std::string{name} + "'");
        return *slot;
    }

    [[noreturn]] static void fail(const SourceLocation& loc, std::string msg) {
        throw ScriptError(ScriptError::Phase::Runtime, loc, std::move(msg));
    }

    void beginEntry() {
        steps_ = 0;
        depth_ = 0;
    }

    void step(const SourceLocation& loc) {
        if (++steps_ > cfg_.maxSteps)
            fail(loc, std::format("step limit of {} exceeded (endless loop?)", cfg_.maxSteps));
    }

    void outOfRange(const SourceLocation& loc, const std::string& msg) {
        if (cfg_.outOfRange == OutOfRangePolicy::Error) fail(loc, msg);
        host_.warning(loc, msg);
    }

    // ---- variable access ----
    // Element `index` of a variable. A bare array name means element 0 (the game assigns and
    // reads `nweapon`, `invisible`, `net_kill`, declared as arrays, without an index).
    Value load(std::int32_t slot, std::int32_t index, const SourceLocation& loc) {
        const auto& v = vars_[std::size_t(slot)];
        if (index < 0 || std::size_t(index) >= v.size()) {
            outOfRange(loc, std::format("index {} out of range for '{}[{}]'", index, script_.globals()[std::size_t(slot)].name, v.size()));
            return v.type == ValueType::Int ? Value{0} : Value{std::string{}};
        }
        return v.type == ValueType::Int ? Value{v.ints[std::size_t(index)]} : Value{v.strings[std::size_t(index)]};
    }

    // Stores with conversion to the variable's type; returns the stored value.
    Value store(std::int32_t slot, std::int32_t index, const Value& value, const SourceLocation& loc) {
        auto& v = vars_[std::size_t(slot)];
        if (index < 0 || std::size_t(index) >= v.size()) {
            outOfRange(loc, std::format("index {} out of range for '{}[{}]' (write ignored)", index,
                                        script_.globals()[std::size_t(slot)].name, v.size()));
            return value.convertedTo(v.type);
        }
        if (v.type == ValueType::Int) return Value{v.ints[std::size_t(index)] = value.toInt()};
        return Value{v.strings[std::size_t(index)] = value.toString()};
    }

    bool isCharAccess(std::int32_t slot) const {
        const auto& g = script_.globals()[std::size_t(slot)];
        return g.type == ValueType::String && !g.isArray;
    }

    std::int32_t charValue(unsigned char c) const {
        return cfg_.signedChars ? std::int32_t(static_cast<signed char>(c)) : std::int32_t(c);
    }

    // s[i] on a string scalar: the byte at i; the terminating NUL (i == length) reads as 0.
    Value loadChar(std::int32_t slot, std::int32_t index, const SourceLocation& loc) {
        const std::string& s = vars_[std::size_t(slot)].strings[0];
        if (index >= 0 && std::size_t(index) < s.size()) return Value{charValue(static_cast<unsigned char>(s[std::size_t(index)]))};
        if (index != std::int32_t(s.size()))
            outOfRange(loc, std::format("character index {} beyond the end of '{}' (length {})", index,
                                        script_.globals()[std::size_t(slot)].name, s.size()));
        return Value{0};
    }

    // s[i] = c with C-string semantics: c == 0 truncates at i; i == length appends; writing
    // past the terminator is ignored (the bytes in between would be undefined in C).
    Value storeChar(std::int32_t slot, std::int32_t index, const Value& value, const SourceLocation& loc) {
        std::string& s = vars_[std::size_t(slot)].strings[0];
        const auto byte = static_cast<unsigned char>(
            value.isString() ? (value.stringValue().empty() ? 0 : value.stringValue()[0]) : value.intValue() & 0xFF);
        if (index < 0 || std::size_t(index) > s.size()) {
            outOfRange(loc, std::format("character index {} beyond the end of '{}' (length {}, write ignored)", index,
                                        script_.globals()[std::size_t(slot)].name, s.size()));
        } else if (byte == 0) {
            s.resize(std::size_t(index));
        } else if (std::size_t(index) == s.size()) {
            s.push_back(static_cast<char>(byte));
        } else {
            s[std::size_t(index)] = static_cast<char>(byte);
        }
        return Value{charValue(byte)};
    }

    struct LvalueRef {
        std::int32_t slot;
        std::int32_t index;
        bool charAccess;
    };

    LvalueRef lvalue(const Expr& target) {
        if (target.kind == ExprKind::Variable) return {target.slot, 0, false};
        const std::int32_t index = eval(*target.a).toInt();
        return {target.slot, index, isCharAccess(target.slot)};
    }
    Value loadRef(const LvalueRef& r, const SourceLocation& loc) {
        return r.charAccess ? loadChar(r.slot, r.index, loc) : load(r.slot, r.index, loc);
    }
    Value storeRef(const LvalueRef& r, const Value& v, const SourceLocation& loc) {
        return r.charAccess ? storeChar(r.slot, r.index, v, loc) : store(r.slot, r.index, v, loc);
    }

    // ---- expressions ----
    static int compareStrings(const std::string& a, const std::string& b) {
        // strcmp: unsigned byte comparison
        const std::size_t n = std::min(a.size(), b.size());
        for (std::size_t i = 0; i < n; ++i) {
            const auto x = static_cast<unsigned char>(a[i]), y = static_cast<unsigned char>(b[i]);
            if (x != y) return x < y ? -1 : 1;
        }
        return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
    }

    Value binary(BinaryOp op, const Value& l, const Value& r) {
        if (op == BinaryOp::Add && (l.isString() || r.isString())) return Value{l.toString() + r.toString()};
        const bool relational = op >= BinaryOp::Less;
        if (relational && l.isString() && r.isString()) {
            const int c = compareStrings(l.stringValue(), r.stringValue());
            switch (op) {
                case BinaryOp::Less: return Value{c < 0};
                case BinaryOp::LessEq: return Value{c <= 0};
                case BinaryOp::Greater: return Value{c > 0};
                case BinaryOp::GreaterEq: return Value{c >= 0};
                case BinaryOp::Equal: return Value{c == 0};
                case BinaryOp::NotEqual: return Value{c != 0};
                default: break;
            }
        }
        return Value{Parser::foldBinary(op, l.toInt(), r.toInt())};
    }

    Value eval(const Expr& e) {
        switch (e.kind) {
            case ExprKind::IntLiteral: return Value{e.intValue};
            case ExprKind::StrLiteral: return Value{e.text};
            case ExprKind::Variable: return load(e.slot, 0, e.loc);
            case ExprKind::Index: {
                const std::int32_t index = eval(*e.a).toInt();
                return isCharAccess(e.slot) ? loadChar(e.slot, index, e.loc) : load(e.slot, index, e.loc);
            }
            case ExprKind::Call: return evalCall(e);
            case ExprKind::Unary: {
                const std::int32_t v = eval(*e.a).toInt();
                switch (e.uop) {
                    case UnaryOp::Negate: return Value{intops::neg(v)};
                    case UnaryOp::Plus: return Value{v};
                    case UnaryOp::Not: return Value{v == 0 ? 1 : 0};
                    case UnaryOp::BitNot: return Value{~v};
                }
                return Value{};
            }
            case ExprKind::Binary: {
                const Value l = eval(*e.a);
                const Value r = eval(*e.b);
                return binary(e.bop, l, r);
            }
            case ExprKind::Logical: {
                const bool l = eval(*e.a).truthy();
                if (cfg_.shortCircuit && (e.isAnd ? !l : l)) return Value{e.isAnd ? 0 : 1};
                const bool r = eval(*e.b).truthy();
                return Value{(e.isAnd ? (l && r) : (l || r)) ? 1 : 0};
            }
            case ExprKind::Assign: {
                const LvalueRef ref = lvalue(*e.a);
                Value v = eval(*e.b);
                if (e.compound) v = binary(e.bop, loadRef(ref, e.loc), v);
                return storeRef(ref, v, e.loc);
            }
            case ExprKind::IncDec: {
                const LvalueRef ref = lvalue(*e.a);
                const Value old = loadRef(ref, e.loc);
                const std::int32_t oldInt = old.toInt();
                const std::int32_t updated = e.increment ? intops::add(oldInt, 1) : intops::sub(oldInt, 1);
                const Value stored = storeRef(ref, Value{updated}, e.loc);
                return e.prefix ? stored : Value{oldInt};
            }
            case ExprKind::Conditional: return eval(*e.a).truthy() ? eval(*e.b) : eval(*e.c);
        }
        return Value{};
    }

    Value evalCall(const Expr& e) {
        std::vector<Value> args;
        args.reserve(e.args.size());
        for (const auto& a : e.args) args.push_back(eval(*a)); // left to right
        if (e.function) return invoke(*e.function, args, e.loc);
        return host_.callBuiltin(*e.builtin, args);
    }

    Value invoke(const FunctionDef& fn, std::span<const Value> args, const SourceLocation& callLoc) {
        if (++depth_ > cfg_.maxCallDepth) fail(callLoc, std::format("call depth limit {} exceeded (recursion?)", cfg_.maxCallDepth));
        if (cfg_.traceUserFunctions) host_.userFunctionCalled(fn, args);
        for (std::size_t i = 0; i < fn.paramSlots.size(); ++i) {
            if (i < args.size()) store(fn.paramSlots[i], 0, args[i], callLoc);
            else if (cfg_.missingArguments == MissingArguments::Reset) store(fn.paramSlots[i], 0, Value{}, callLoc);
        }
        returnValue_ = Value{cfg_.defaultReturn};
        exec(*fn.body);
        --depth_;
        Value r = std::move(returnValue_);
        returnValue_ = Value{cfg_.defaultReturn};
        return r;
    }

    // ---- statements ----
    Flow exec(const Stmt& s) {
        step(s.loc);
        switch (s.kind) {
            case StmtKind::Empty: return Flow::Normal;
            case StmtKind::Expression: eval(*s.expr); return Flow::Normal;
            case StmtKind::Block:
                for (const auto& c : s.children)
                    if (const Flow f = exec(*c); f != Flow::Normal) return f;
                return Flow::Normal;
            case StmtKind::If:
                if (eval(*s.expr).truthy()) return exec(*s.body);
                if (s.elseBody) return exec(*s.elseBody);
                return Flow::Normal;
            case StmtKind::Iff: return execIff(s);
            case StmtKind::For:
                if (s.init) eval(*s.init);
                for (;;) {
                    if (s.expr && !eval(*s.expr).truthy()) break;
                    const Flow f = exec(*s.body);
                    if (f == Flow::Break) break;
                    if (f == Flow::Return) return f;
                    if (s.step) eval(*s.step);
                    step(s.loc);
                }
                return Flow::Normal;
            case StmtKind::While:
                while (eval(*s.expr).truthy()) {
                    const Flow f = exec(*s.body);
                    if (f == Flow::Break) break;
                    if (f == Flow::Return) return f;
                    step(s.loc);
                }
                return Flow::Normal;
            case StmtKind::DoWhile:
                for (;;) {
                    const Flow f = exec(*s.body);
                    if (f == Flow::Break) break;
                    if (f == Flow::Return) return f;
                    if (!eval(*s.expr).truthy()) break;
                    step(s.loc);
                }
                return Flow::Normal;
            case StmtKind::Break: return Flow::Break;
            case StmtKind::Continue: return Flow::Continue;
            case StmtKind::Return:
                returnValue_ = s.expr ? eval(*s.expr) : Value{cfg_.defaultReturn};
                return Flow::Return;
        }
        return Flow::Normal;
    }

    Flow execIff(const Stmt& s) {
        std::uint8_t& state = iffState_[s.iffSite];
        switch (cfg_.iffMode) {
            case IffMode::OncePerSite:
                if (state) {
                    if (cfg_.iffEvaluateWhenFired) eval(*s.expr);
                    return Flow::Normal;
                }
                if (!eval(*s.expr).truthy()) return Flow::Normal;
                state = 1; // before the body: `iff(1) { ...; return; }` must not run again
                return exec(*s.body);
            case IffMode::RisingEdge: {
                const bool now = eval(*s.expr).truthy();
                const bool before = state != 0;
                state = now ? 1 : 0;
                if (now && !before) return exec(*s.body);
                return Flow::Normal;
            }
            case IffMode::PlainIf:
                if (eval(*s.expr).truthy()) return exec(*s.body);
                return Flow::Normal;
        }
        return Flow::Normal;
    }

    Script script_;
    ScriptHost& host_;
    ExecConfig cfg_;
    std::vector<Storage> vars_;
    std::vector<std::uint8_t> iffState_;
    Value returnValue_;
    std::uint64_t ticks_ = 0;
    std::uint64_t steps_ = 0;
    std::uint32_t depth_ = 0;
};

} // namespace gromada::script
