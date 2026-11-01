#pragma once
// Builtin (system) functions and the host interface the engine implements.
//
// The game declares its builtins in maps/HEADER.INI as `extern Name(params) ID;`. The numeric
// ID is the stable identity of a builtin: the host dispatches on it. A name may be declared more
// than once with different arities (`StopSFX()` and `StopSFX(int nsfx)` have different IDs), so
// name lookup takes the number of arguments into account.
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "script/ast.hpp"
#include "script/diagnostics.hpp"
#include "script/value.hpp"

namespace gromada::script {

struct BuiltinInfo {
    std::int32_t id = 0;
    std::string name;
    std::vector<ParamDecl> params;
    SourceLocation loc; // where it was declared (unknown for programmatic registrations)

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

class BuiltinRegistry {
public:
    BuiltinRegistry() = default;
    // Copies rebuild the indices (they point into each registry's own storage).
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

    // Adds a builtin. Re-adding an identical declaration is a no-op. Throws ScriptError(Link) if
    // the ID is already bound to a different name, or name+arity is already bound to another ID.
    const BuiltinInfo& add(BuiltinInfo info) {
        if (const auto it = byId_.find(info.id); it != byId_.end()) {
            const BuiltinInfo& old = *it->second;
            if (old.name == info.name && old.arity() == info.arity()) return old;
            throw ScriptError(ScriptError::Phase::Link, info.loc,
                              std::format("builtin id {} declared as '{}' but already bound to '{}'", info.id,
                                          info.name, old.signature()));
        }
        for (const BuiltinInfo* other : overloads(info.name))
            if (other->arity() == info.arity())
                throw ScriptError(ScriptError::Phase::Link, info.loc,
                                  std::format("builtin '{}' with {} parameter(s) declared with id {} and id {}",
                                              info.name, info.arity(), other->id, info.id));
        storage_.push_back(std::make_unique<BuiltinInfo>(std::move(info)));
        const BuiltinInfo* p = storage_.back().get();
        byId_[p->id] = p;
        byName_[p->name].push_back(p);
        return *p;
    }

    void addAll(const BuiltinRegistry& other) {
        for (const auto& b : other.storage_) add(*b);
    }

    const BuiltinInfo* findById(std::int32_t id) const {
        const auto it = byId_.find(id);
        return it == byId_.end() ? nullptr : it->second;
    }

    std::span<const BuiltinInfo* const> overloads(std::string_view name) const {
        const auto it = byName_.find(name);
        if (it == byName_.end()) return {};
        return it->second;
    }

    // Resolution of a call with `argc` arguments: the overload with exactly that arity, otherwise
    // the only overload (arity mismatches are then handled by the caller's policy), otherwise null.
    const BuiltinInfo* resolve(std::string_view name, std::size_t argc) const {
        const auto list = overloads(name);
        for (const BuiltinInfo* b : list)
            if (b->arity() == argc) return b;
        return list.size() == 1 ? list.front() : nullptr;
    }

    // Every builtin, by increasing id.
    std::vector<const BuiltinInfo*> all() const {
        std::vector<const BuiltinInfo*> r;
        for (const auto& [id, p] : byId_) r.push_back(p);
        return r;
    }
    std::size_t size() const { return storage_.size(); }
    bool empty() const { return storage_.empty(); }

private:
    std::vector<std::unique_ptr<BuiltinInfo>> storage_; // stable addresses, also across moves
    std::map<std::int32_t, const BuiltinInfo*> byId_;
    std::map<std::string, std::vector<const BuiltinInfo*>, std::less<>> byName_;
};

// Helpers for hosts: argument i converted to the wanted type, or a default when missing.
inline std::int32_t argInt(std::span<const Value> args, std::size_t i, std::int32_t def = 0) {
    return i < args.size() ? args[i].toInt() : def;
}
inline std::string argString(std::span<const Value> args, std::size_t i, std::string def = {}) {
    return i < args.size() ? args[i].toString() : def;
}

// Interface implemented by the engine (and by test/log hosts).
class ScriptHost {
public:
    virtual ~ScriptHost() = default;

    // Called for every builtin call. `args` are evaluated left to right and passed as they are
    // (an int may be passed where the declaration says string and vice versa: convert with
    // argInt/argString). The return value becomes the value of the call expression.
    virtual Value callBuiltin(const BuiltinInfo& builtin, std::span<const Value> args) = 0;

    // Non-fatal runtime anomalies (out-of-range index...).
    virtual void warning(const SourceLocation& loc, std::string_view message) { (void)loc; (void)message; }

    // Called when a user function (defined in the script) is entered, only if
    // ExecConfig::traceUserFunctions is set. `args` are the values passed by the caller.
    virtual void userFunctionCalled(const FunctionDef& fn, std::span<const Value> args) { (void)fn; (void)args; }
};

} // namespace gromada::script
