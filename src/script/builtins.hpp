#pragma once
// Builtin (system) functions and the host interface the engine implements.
//
// The game declares its builtins in maps/HEADER.INI as `extern Name(params) ID;`. Like the
// original compiler, only the name and the ID matter: the ID (truncated to a byte) becomes the
// opcode emitted for a call, and the parameter text is documentation. When a name is declared
// twice the FIRST declaration wins (`StopSFX` 134 and 136: every call uses 134).
#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "script/diagnostics.hpp"
#include "script/program.hpp"
#include "script/value.hpp"

namespace gromada::script {

struct BuiltinInfo {
    std::int32_t id = 0;          // opcode (0..255)
    std::string name;
    std::vector<ParamDecl> params; // parsed from the declaration's text (best effort)
    SourceLocation loc;           // where it was declared (unknown for programmatic registrations)

    std::size_t arity() const { return params.size(); }
    std::string signature() const {
        std::string s = name + "(";
        for (std::size_t i = 0; i < params.size(); ++i) {
            if (i) s += ", ";
            s += paramTypeName(params[i].type);
            if (!params[i].name.empty()) s += " " + params[i].name;
        }
        return s + ") " + std::to_string(id);
    }
};

// Every extern declaration of a script, in declaration order. Lookups return the first
// declaration with that name or ID, like the original's linear search.
class BuiltinRegistry {
public:
    BuiltinRegistry() = default;
    BuiltinRegistry(const BuiltinRegistry& other) { addAll(other); }
    BuiltinRegistry& operator=(const BuiltinRegistry& other) {
        if (this != &other) {
            BuiltinRegistry copy{other};
            *this = std::move(copy);
        }
        return *this;
    }
    BuiltinRegistry(BuiltinRegistry&&) noexcept = default;
    BuiltinRegistry& operator=(BuiltinRegistry&&) noexcept = default;

    // Adds a declaration (duplicates are kept; lookups find the first one).
    const BuiltinInfo& add(BuiltinInfo info) {
        storage_.push_back(std::make_unique<BuiltinInfo>(std::move(info)));
        return *storage_.back();
    }
    void addAll(const BuiltinRegistry& other) {
        for (const auto& b : other.storage_) add(*b);
    }

    const BuiltinInfo* findById(std::int32_t id) const {
        for (const auto& b : storage_)
            if (b->id == id) return b.get();
        return nullptr;
    }
    const BuiltinInfo* findByName(std::string_view name) const {
        for (const auto& b : storage_)
            if (b->name == name) return b.get();
        return nullptr;
    }
    // Every declaration with this name, in declaration order (only the first is callable).
    std::vector<const BuiltinInfo*> overloads(std::string_view name) const {
        std::vector<const BuiltinInfo*> r;
        for (const auto& b : storage_)
            if (b->name == name) r.push_back(b.get());
        return r;
    }
    // The declaration a call by name binds to: the first one, whatever the argument count.
    const BuiltinInfo* resolve(std::string_view name, std::size_t argc = 0) const {
        (void)argc;
        return findByName(name);
    }
    // Every declaration, by increasing id (stable for equal ids).
    std::vector<const BuiltinInfo*> all() const {
        std::vector<const BuiltinInfo*> r = declarations();
        std::ranges::stable_sort(r, {}, [](const BuiltinInfo* b) { return b->id; });
        return r;
    }
    // Every declaration in declaration order.
    std::vector<const BuiltinInfo*> declarations() const {
        std::vector<const BuiltinInfo*> r;
        for (const auto& b : storage_) r.push_back(b.get());
        return r;
    }
    std::size_t size() const { return storage_.size(); }
    bool empty() const { return storage_.empty(); }

private:
    std::vector<std::unique_ptr<BuiltinInfo>> storage_; // stable addresses, also across moves
};

// Helpers for hosts: argument i converted to the wanted type, or a default when missing.
// Conversions are the original's: string -> int with atoi, int -> string in decimal.
inline std::int32_t argInt(std::span<const Value> args, std::size_t i, std::int32_t def = 0) {
    return i < args.size() ? args[i].toInt() : def;
}
inline std::string argString(std::span<const Value> args, std::size_t i, std::string def = {}) {
    return i < args.size() ? args[i].toString() : def;
}

// Whether the original's handler of builtin `id` pushes a return value (docs/re/runtime.md §3,
// column "Ritorno"). Builtins that push nothing leave the stack alone: their result cannot be
// used in an expression. GetNetInfo (129) pushes for types 1..6 and 10..25 only.
inline bool knownBuiltinPushes(std::int32_t id, std::span<const Value> args) {
    switch (id) {
        case 65: case 67: case 68: case 69: case 74: case 75: case 85: case 86: case 88: case 90:
        case 93: case 94: case 95: case 96: case 102: case 103: case 105: case 109: case 110:
        case 112: case 113: case 115: case 116: case 117: case 121: case 123: case 124: case 131:
        case 133: case 137: case 138: case 139:
            return true;
        case 129: {
            const std::int32_t t = argInt(args, 0);
            return (t >= 1 && t <= 6) || (t >= 10 && t <= 25);
        }
        default: return false;
    }
}

// Interface implemented by the engine (and by test/log hosts).
class ScriptHost {
public:
    virtual ~ScriptHost() = default;

    // Called for every builtin call (opcodes 64..139). `args` are the values the script pushed,
    // first argument first, as they are (an int may arrive where the declaration says string
    // and vice versa: convert with argInt/argString). The return value is pushed on the
    // script's stack if builtinPushesResult() says so, and ignored otherwise.
    virtual Value callBuiltin(const BuiltinInfo& builtin, std::span<const Value> args) = 0;

    // Whether the builtin pushes its result (default: the original's handlers, see
    // knownBuiltinPushes). Called after callBuiltin, with the same arguments.
    virtual bool builtinPushesResult(const BuiltinInfo& builtin, std::span<const Value> args) {
        return knownBuiltinPushes(builtin.id, args);
    }

    // Non-fatal runtime anomalies (the original logs them to error.log): stack errors at the
    // end of a statement, unknown opcodes, accesses outside the emulated memory.
    virtual void warning(const SourceLocation& loc, std::string_view message) { (void)loc; (void)message; }

    // Called when a user function (defined in the script) is entered, only if
    // ExecConfig::traceUserFunctions is set. `args` are the values of its parameter cells.
    virtual void userFunctionCalled(const FunctionDef& fn, std::span<const Value> args) { (void)fn; (void)args; }
};

} // namespace gromada::script
