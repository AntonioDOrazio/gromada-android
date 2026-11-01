#pragma once
// Stub hosts: record every builtin call (tests, tools/script_run) instead of driving an engine.
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "script/builtin_ids.hpp"
#include "script/builtins.hpp"

namespace gromada::script {

struct BuiltinCallRecord {
    std::uint64_t tick = 0;
    std::int32_t id = 0;          // builtin id; -1 for a user function (traceUserFunctions)
    bool userFunction = false;
    std::string name;
    std::vector<Value> args;
    Value result;

    // "Name(arg, arg) -> result" with strings quoted and escaped.
    std::string str(bool withResult = false) const {
        std::string s = name + "(";
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (i) s += ", ";
            s += args[i].repr();
        }
        s += ")";
        if (withResult && !userFunction) s += " -> " + result.repr();
        return s;
    }
};

// Records every builtin call (and user function calls when ExecConfig::traceUserFunctions is
// set). The return value of a call comes from, in order: the responder, the
// fixed value set with setReturn(name), or int 0.
class RecordingHost : public ScriptHost {
public:
    using Responder = std::function<std::optional<Value>(const BuiltinInfo&, std::span<const Value>)>;

    void setResponder(Responder r) { responder_ = std::move(r); }
    void setReturn(std::string name, Value v) { fixed_[std::move(name)] = std::move(v); }
    void setTick(std::uint64_t tick) { tick_ = tick; }

    Value callBuiltin(const BuiltinInfo& b, std::span<const Value> args) override {
        Value result;
        if (auto r = responder_ ? responder_(b, args) : std::nullopt) result = std::move(*r);
        else if (const auto it = fixed_.find(b.name); it != fixed_.end()) result = it->second;
        calls_.push_back(BuiltinCallRecord{tick_, b.id, false, b.name, {args.begin(), args.end()}, result});
        return result;
    }

    void userFunctionCalled(const FunctionDef& fn, std::span<const Value> args) override {
        calls_.push_back(BuiltinCallRecord{tick_, -1, true, fn.name, {args.begin(), args.end()}, Value{}});
    }

    void warning(const SourceLocation& loc, std::string_view message) override {
        warnings_.push_back(loc.str() + ": " + std::string{message});
    }

    const std::vector<BuiltinCallRecord>& calls() const { return calls_; }
    const std::vector<std::string>& warnings() const { return warnings_; }
    void clear() { calls_.clear(); warnings_.clear(); }

    std::vector<const BuiltinCallRecord*> callsTo(std::string_view name) const {
        std::vector<const BuiltinCallRecord*> r;
        for (const auto& c : calls_)
            if (c.name == name) r.push_back(&c);
        return r;
    }

private:
    Responder responder_;
    std::map<std::string, Value, std::less<>> fixed_;
    std::vector<BuiltinCallRecord> calls_;
    std::vector<std::string> warnings_;
    std::uint64_t tick_ = 0;
};

// A minimal fake world for RecordingHost::setResponder, so that scripts take plausible paths:
// a clock, handles for created objects, flagmen, a deterministic Random. Everything else
// answers 0 (or "" where the game expects a string). Dispatches on builtin IDs.
class SimulatedWorld {
public:
    std::int32_t timeMs = 0;          // returned by GetTime(); advanced by the caller
    std::int32_t nextHandle = 1000;   // returned by Create/MenuCreate/PrintText/...
    std::uint32_t randomState = 12345;
    std::int32_t flagman[4] = {0, 0, 0, 0};

    std::optional<Value> operator()(const BuiltinInfo& b, std::span<const Value> args) {
        switch (static_cast<BuiltinId>(b.id)) {
            case BuiltinId::GetTime: return Value{timeMs};
            case BuiltinId::Random: {
                const std::int32_t max = argInt(args, 0);
                randomState = randomState * 1103515245u + 12345u;
                return Value{max > 0 ? std::int32_t((randomState >> 16) % std::uint32_t(max)) : 0};
            }
            case BuiltinId::Create:
            case BuiltinId::MenuCreate:
            case BuiltinId::PrintText:
            case BuiltinId::PrintFile:
            case BuiltinId::CreateLight: return Value{nextHandle++};
            case BuiltinId::SetFlagman: flagman[argInt(args, 0) & 3] = argInt(args, 1); return Value{0};
            case BuiltinId::Flagman: return Value{flagman[argInt(args, 0) & 3]};
            case BuiltinId::strlen: return Value{std::int32_t(argString(args, 0).size())};
            case BuiltinId::GetVidName: return Value{"vid" + std::to_string(argInt(args, 0))};
            case BuiltinId::GetCommands:
            case BuiltinId::GetReg: return Value{std::string{}};
            default: return Value{0};
        }
    }
};

} // namespace gromada::script
