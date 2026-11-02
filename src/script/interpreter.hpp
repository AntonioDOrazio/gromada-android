#pragma once
// The script VM: executes the bytecode of a compiled Script exactly like the original's
// interpreter (gromada.exe 0x41a790, docs/re/runtime.md §2.4-2.5).
//
// State (the original's LOGIC object):
//  - 2048 cells of {u32 value, u8 flags}: the script's globals occupy the first cells, the
//    evaluation stack starts right after them (sp = bp = number of globals at every run);
//  - the code, which the IFF opcode patches at run time;
//  - an emulated address space for strings (memory.hpp): string cells hold addresses.
//
// Every frame (tick) runs the code from offset 0 (`CALL main; JMP +30000`). Nothing survives
// between frames but the cells and the code patches. A new map = a new Script and a new
// Interpreter; the cells are carried over with inheritGlobals(previous.snapshot()), which
// reproduces the original exactly: int cells without initializer keep their value (by cell
// index, not by name), initializers are applied again, strings restart from "".
//
// `iff` (opcode IFF): when its condition is true, the original overwrites the code at the
// "start of the current statement" (the instruction after the last EOS, JZ, IFF, CALL or RET
// executed) with a JMP to the iff's false target, then runs the body. Normally that is the
// iff's own condition, so the iff never runs again; when the iff was reached by a JMP (end of an
// if branch, `break`) the JMP itself is rewritten and the iff can fire a second time when
// reached another way. This interpreter does the same patching (ExecConfig::iffMode = Patch).
#include <algorithm>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "script/builtin_ids.hpp"
#include "script/builtins.hpp"
#include "script/bytecode.hpp"
#include "script/compiler.hpp"
#include "script/memory.hpp"

namespace gromada::script {

// One cell of the original (value + flags, see cellflag).
struct Cell {
    std::uint32_t value = 0;
    std::uint8_t flags = 0;
    friend bool operator==(const Cell&, const Cell&) = default;
};

inline constexpr std::size_t kCellCount = 2048;

enum class IffMode : std::uint8_t {
    // Default, the original: the IFF opcode patches the start of the current statement (see the
    // comment at the top), including the rare second firing of an iff reached by a jump.
    Patch,
    // Patch the iff's own condition (computed at compile time): exactly once per site. Differs
    // from Patch only for iff statements reached by a JMP (IffSite::reachableByJump).
    OncePerSite,
    // Never patch: `iff` behaves like `if` (experiments).
    PlainIf,
};

// x % 0, INT_MIN / -1 and INT_MIN % -1 raise a CPU exception in the original (the game dies).
enum class DivideErrorPolicy : std::uint8_t {
    Throw,  // default: ScriptError(Runtime)
    Ignore, // x%0 = 0, INT_MIN/-1 = INT_MIN, INT_MIN%-1 = 0, plus ScriptHost::warning
};

// Accesses the original would perform on arbitrary memory: cells outside the 2048, string
// pointers that do not point into any emulated block.
enum class OutOfRangePolicy : std::uint8_t {
    Warn,  // default: reads give 0 / "", writes are ignored, ScriptHost::warning is called
    Error, // ScriptError(Runtime)
};

struct ExecConfig {
    IffMode iffMode = IffMode::Patch;
    DivideErrorPolicy divideError = DivideErrorPolicy::Throw;
    OutOfRangePolicy outOfRange = OutOfRangePolicy::Warn;
    // Safety limit of executed instructions per run (tick/call): the original would hang.
    std::uint64_t maxSteps = 10'000'000;
    // Report user function calls to ScriptHost::userFunctionCalled (debugging, tests).
    bool traceUserFunctions = false;
};

// State carried from one script to the next.
struct GlobalSnapshot {
    // The whole cell array and the number of globals of the script that produced it: what the
    // original keeps across a map load.
    std::vector<Cell> cells;
    std::uint32_t globalCount = 0;
    // Values by variable name (readable form; used by CarryOverPolicy::Mode::ByName).
    struct Entry {
        ValueType type = ValueType::Int;
        std::vector<std::int32_t> ints;
        std::vector<std::string> strings;
    };
    std::map<std::string, Entry> vars;
};

struct CarryOverPolicy {
    enum class Mode : std::uint8_t {
        // Default, the original: every cell keeps its value by index; the previous script's
        // string cells are freed (value 0); the new script's declarations then set flags,
        // initializers and empty strings.
        ByCell,
        // Same-named variables (experiments): controlled by the fields below.
        ByName,
    };
    Mode mode = Mode::ByCell;
    bool ints = true;
    bool strings = false;
    bool overwriteInitialized = false;
};

class Interpreter {
public:
    // Sets up the cells like the original's compiler at a map load from a fresh game (every
    // cell 0): flags, compile-time initializers, an empty 128-byte buffer per string cell.
    // Nothing is executed and no builtin is called.
    Interpreter(Script script, ScriptHost& host, ExecConfig config = {})
        : script_{std::move(script)}, host_{host}, cfg_{config} {
        globalCount_ = std::uint32_t(script_.cellCount());
        if (globalCount_ >= kCellCount)
            throw ScriptError(ScriptError::Phase::Runtime, {}, "LOGIC::Too many variables for the cell array");
        cells_.assign(kCellCount, Cell{});
        mem_.setCode(script_.code());
        for (std::uint32_t i = 0; i < globalCount_; ++i) {
            const CellDecl& d = script_.cells()[i];
            if (d.flags & cellflag::String) stringBuffers_.push_back(mem_.alloc(128));
        }
        applyDeclarations();
        iffSiteAt_.assign(script_.code().size() + 1, -1);
        for (std::size_t s = 0; s < script_.iffSites().size(); ++s) iffSiteAt_[script_.iffSites()[s].opcodeOffset] = std::int32_t(s);
        iffFires_.assign(script_.iffSites().size(), 0);
    }

    const Script& script() const { return script_; }
    const ExecConfig& config() const { return cfg_; }
    ExecConfig& config() { return cfg_; }

    // Runs one frame: the code from offset 0 (CALL main). Returns main's return value.
    Value tick() {
        if (script_.mainEntry() == 0) throw ScriptError(ScriptError::Phase::Runtime, {}, "the script has no main() function");
        ++ticks_;
        begin(0);
        execute();
        return valueOf(cells_[globalCount_]);
    }
    std::uint64_t tickCount() const { return ticks_; }

    // Calls a user function like a script call would: each argument is stored into its
    // parameter cell (missing ones keep their value), then the function runs on an empty stack.
    Value call(std::string_view name, std::span<const Value> args = {}) {
        const FunctionDef* f = script_.findFunction(name);
        if (!f) throw ScriptError(ScriptError::Phase::Runtime, {}, "no function named '" + std::string{name} + "'");
        for (std::size_t i = 0; i < args.size(); ++i) storeValue(std::uint32_t(f->paramBase) + std::uint32_t(i), args[i]);
        begin(std::uint32_t(mem_.code().size()));
        // As CALL from a virtual instruction placed at the end of the code: RET leaves the loop.
        cells_[bp_].value = std::uint32_t(mem_.code().size());
        ++bp_;
        sp_ = bp_;
        pc_ = f->entry;
        stmtStart_ = pc_;
        traceCall(f->entry);
        execute();
        return valueOf(cells_[globalCount_]);
    }

    // ---- variables ----
    bool hasGlobal(std::string_view name) const { return script_.findGlobalVar(name) != nullptr; }
    Value get(std::string_view name, std::int32_t index = 0) const { return valueOf(cells_[cellIndex(name, index)]); }
    void set(std::string_view name, const Value& value, std::int32_t index = 0) { storeValue(cellIndex(name, index), value); }

    // Number of globals (cells declared by the script); the stack starts after them.
    std::uint32_t globalCount() const { return globalCount_; }
    std::span<const Cell> cells() const { return cells_; }
    const Cell& cell(std::size_t i) const { return cells_.at(i); }
    // A cell as a Value (string cells are read through their address).
    Value cellValue(std::size_t i) const { return valueOf(cells_.at(i)); }
    // The NUL-terminated string at an emulated address (nullopt if the address is invalid).
    std::optional<std::string> stringAt(std::uint32_t address) const { return mem_.readString(address); }

    // Values of every int cell of the script, in cell order (array elements included): what the
    // original stores in a demo right after compiling the map's script (docs/re/runtime.md §8).
    std::vector<std::uint32_t> intGlobalValues() const {
        std::vector<std::uint32_t> r;
        for (std::uint32_t i = 0; i < globalCount_; ++i)
            if (script_.cells()[i].flags & cellflag::Int) r.push_back(cells_[i].value);
        return r;
    }
    // Loads the int cells from a demo. Returns false (and changes nothing) if the count differs.
    bool setIntGlobalValues(std::span<const std::uint32_t> values) {
        std::size_t n = 0;
        for (std::uint32_t i = 0; i < globalCount_; ++i) n += (script_.cells()[i].flags & cellflag::Int) ? 1 : 0;
        if (n != values.size()) return false;
        std::size_t k = 0;
        for (std::uint32_t i = 0; i < globalCount_; ++i)
            if (script_.cells()[i].flags & cellflag::Int) cells_[i].value = values[k++];
        return true;
    }

    GlobalSnapshot snapshot() const {
        GlobalSnapshot s;
        s.cells = cells_;
        s.globalCount = globalCount_;
        for (const auto& g : script_.globals()) {
            auto& e = s.vars[g.name];
            e.type = g.type;
            for (std::int32_t k = 0; k < g.size; ++k) {
                const Value v = valueOf(cells_[std::size_t(g.firstCell + k)]);
                if (g.type == ValueType::Int) e.ints.push_back(v.toInt());
                else e.strings.push_back(v.toString());
            }
        }
        return s;
    }

    // Takes over the state of the previous script (call before the first tick). See
    // CarryOverPolicy; the default is the original's behaviour.
    void inheritGlobals(const GlobalSnapshot& previous, const CarryOverPolicy& policy = {}) {
        if (policy.mode == CarryOverPolicy::Mode::ByCell) {
            if (previous.cells.size() != kCellCount) return;
            std::vector<Cell> next = previous.cells;
            // Unloading the previous script frees its strings (0x417c30): their cells become 0.
            for (std::uint32_t i = 0; i < previous.globalCount && i < kCellCount; ++i)
                if (next[i].flags & cellflag::String) next[i].value = 0;
            cells_ = std::move(next);
            applyDeclarations();
            return;
        }
        for (const auto& g : script_.globals()) {
            const auto it = previous.vars.find(g.name);
            if (it == previous.vars.end() || it->second.type != g.type) continue;
            if (g.hasInitializer() && !policy.overwriteInitialized) continue;
            for (std::int32_t k = 0; k < g.size; ++k) {
                const std::uint32_t c = std::uint32_t(g.firstCell + k);
                if (g.type == ValueType::Int && policy.ints && std::size_t(k) < it->second.ints.size())
                    storeValue(c, Value{it->second.ints[std::size_t(k)]});
                else if (g.type == ValueType::String && policy.strings && std::size_t(k) < it->second.strings.size())
                    storeValue(c, Value{it->second.strings[std::size_t(k)]});
            }
        }
    }

    // ---- code and iff ----
    // The code as patched by the iff statements executed so far.
    std::span<const std::uint8_t> code() const { return mem_.code(); }
    // How many times the iff site (index into Script::iffSites()) fired.
    std::uint32_t iffFireCount(std::uint32_t site) const { return iffFires_.at(site); }
    bool iffFired(std::uint32_t site) const { return iffFires_.at(site) != 0; }
    // Restores the compiled code (as a map load does).
    void resetIffs() {
        auto& code = mem_.code();
        code = script_.code();
        std::ranges::fill(iffFires_, 0u);
    }

private:
    [[noreturn]] void fail(const std::string& msg) const {
        throw ScriptError(ScriptError::Phase::Runtime, script_.locationOf(insn_), msg);
    }
    void warn(const std::string& msg) { host_.warning(script_.locationOf(insn_), msg); }
    void anomaly(const std::string& msg) {
        if (cfg_.outOfRange == OutOfRangePolicy::Error) fail(msg);
        warn(msg);
    }

    void applyDeclarations() {
        std::size_t s = 0;
        for (std::uint32_t i = 0; i < globalCount_; ++i) {
            const CellDecl& d = script_.cells()[i];
            Cell& c = cells_[i];
            c.flags = d.flags;
            if (d.flags & cellflag::String) {
                c.value = stringBuffers_[s++];
                mem_.writeByte(c.value, 0);
            } else if (d.initialized && d.hasInitValue) {
                c.value = d.initValue;
            }
        }
    }

    std::uint32_t cellIndex(std::string_view name, std::int32_t index) const {
        const GlobalVar* g = script_.findGlobalVar(name);
        if (!g) throw std::out_of_range("no global named '" + std::string{name} + "'");
        if (index < 0 || index >= g->size) throw std::out_of_range("index out of range for '" + std::string{name} + "'");
        return std::uint32_t(g->firstCell + index);
    }

    // ---- values ----
    std::string readString(std::uint32_t address) const {
        if (auto s = mem_.readString(address)) return *s;
        return {};
    }
    std::string readStringChecked(std::uint32_t address) {
        if (auto s = mem_.readString(address)) return *s;
        anomaly(std::format("string pointer 0x{:08X} does not point to any string", address));
        return {};
    }
    Value valueOf(const Cell& c) const {
        if (c.flags & cellflag::String) return Value{readString(c.value)};
        return Value{std::int32_t(c.value)};
    }
    Value valueOfChecked(const Cell& c) {
        if (c.flags & cellflag::String) return Value{readStringChecked(c.value)};
        return Value{std::int32_t(c.value)};
    }

    // strcpy into a string cell's buffer; like STORE, a string longer than 127 bytes gets a new
    // buffer of exactly its size (here allocated before freeing the old one).
    void storeString(Cell& dst, std::uint32_t src) {
        const auto n = mem_.length(src);
        if (!n) {
            anomaly(std::format("string pointer 0x{:08X} does not point to any string", src));
            return;
        }
        if (*n > 127) {
            const std::uint32_t fresh = mem_.alloc(*n + 1);
            if (!fresh) fail("LOGIC::string=string Not enough memory");
            mem_.copyForward(fresh, src, *n + 1);
            if (mem_.isHeapBlock(dst.value)) mem_.free(dst.value);
            for (auto& b : stringBuffers_)
                if (b == dst.value) b = fresh;
            dst.value = fresh;
            return;
        }
        if (!mem_.copyForward(dst.value, src, *n + 1))
            anomaly(std::format("string copy to 0x{:08X}, which is not a string buffer", dst.value));
    }
    void storeValue(std::uint32_t cellIndex, const Value& v) {
        Cell& c = cells_.at(cellIndex);
        if (script_.cells().size() > cellIndex && (script_.cells()[cellIndex].flags & cellflag::String)) {
            const std::uint32_t tmp = mem_.hostBuffer(kApiSlot, v.toString());
            storeString(c, tmp);
        } else {
            c.value = std::uint32_t(v.toInt());
        }
    }

    // ---- stack (0x41b200 pop_str, 0x41b270 push_str, 0x41b2d0 push_int, 0x41b330 pop_int) ----
    void push(const Cell& c) {
        if (sp_ + 1 >= kCellCount) fail("LOGIC::stack overflow");
        cells_[sp_++] = c;
    }
    void pushInt(std::int32_t v) { push({std::uint32_t(v), cellflag::Int}); }
    void pushStr(std::uint32_t address) { push({address, cellflag::String}); }
    const Cell& popCell() {
        if (sp_ == 0 || sp_ - 1 < bp_) fail("LOGIC::stack pop, but not push");
        return cells_[--sp_];
    }
    std::int32_t popInt() {
        const Cell& c = popCell();
        if (c.flags & cellflag::String) return stringToInt(readStringChecked(c.value));
        return std::int32_t(c.value);
    }
    std::uint32_t popStr() {
        const Cell& c = popCell();
        if (c.flags & cellflag::String) return c.value;
        const std::string text = intToString(std::int32_t(c.value)); // _itoa into the static buffer
        for (std::size_t i = 0; i <= text.size(); ++i)
            mem_.writeByte(StringMemory::kItoaBase + std::uint32_t(i), i < text.size() ? std::uint8_t(text[i]) : 0);
        return StringMemory::kItoaBase;
    }
    // A cell addressed by an instruction (variable + index), or nullptr outside the array.
    Cell* cellRef(std::int64_t i) {
        if (i < 0 || i >= std::int64_t(kCellCount)) {
            anomaly(std::format("cell {} is outside the cell array", i));
            return nullptr;
        }
        return &cells_[std::size_t(i)];
    }
    void endOfStatement(std::string_view what) {
        stmtStart_ = pc_;
        if (std::int64_t(sp_) - std::int64_t(bp_) > 1) warn(std::format("LOGIC:{}::stack error {}", what, pc_));
        sp_ = bp_;
    }
    void resetIndex() {
        index_ = 0;
        indexActive_ = false;
    }

    void begin(std::uint32_t pc) {
        sp_ = bp_ = globalCount_;
        pc_ = pc;
        insn_ = pc;
        resetIndex();
        steps_ = 0;
    }

    void traceCall(std::uint32_t entry) {
        if (!cfg_.traceUserFunctions) return;
        const FunctionDef* f = script_.functionAt(entry);
        if (!f) return;
        std::vector<Value> args;
        for (std::int32_t k = 0; k < f->paramCount; ++k) args.push_back(valueOf(cells_[std::size_t(f->paramBase + k)]));
        host_.userFunctionCalled(*f, args);
    }

    std::uint32_t hostString(std::int32_t id, const std::string& s) {
        // FRead, GetCommands and GetReg return one static buffer each, like the original.
        if (id == toInt(BuiltinId::FRead) || id == toInt(BuiltinId::GetCommands) || id == toInt(BuiltinId::GetReg))
            return mem_.hostBuffer(std::uint32_t(id), s);
        // Other strings (GetVidName, GetNetInfo names) point to distinct, stable storage.
        if (const auto it = interned_.find(s); it != interned_.end()) return it->second;
        if (interned_.size() >= 4096) interned_.clear();
        const std::uint32_t address = mem_.hostBuffer(kInternSlot + std::uint32_t(interned_.size()), s);
        interned_[s] = address;
        return address;
    }

    int compareStrings(std::uint32_t a, std::uint32_t b) {
        const std::string x = readStringChecked(a), y = readStringChecked(b);
        const std::size_t n = std::min(x.size(), y.size());
        for (std::size_t i = 0; i <= n; ++i) {
            const unsigned cx = i < x.size() ? static_cast<unsigned char>(x[i]) : 0u;
            const unsigned cy = i < y.size() ? static_cast<unsigned char>(y[i]) : 0u;
            if (cx != cy) return cx < cy ? -1 : 1;
        }
        return 0;
    }

    void divideError(const char* what) {
        if (cfg_.divideError == DivideErrorPolicy::Throw) fail(std::format("{}: integer division fault (the original crashes)", what));
        warn(std::format("{}: integer division fault (the original crashes)", what));
    }

    void execute() {
        std::vector<std::uint8_t>& code = mem_.code();
        const std::uint32_t length = std::uint32_t(code.size());
        auto byteAt = [&](std::uint32_t at) -> std::uint8_t { return at < length ? code[at] : 0; };
        while (pc_ < length) {
            if (++steps_ > cfg_.maxSteps) fail(std::format("step limit of {} instructions exceeded (endless loop?)", cfg_.maxSteps));
            insn_ = pc_;
            const std::uint8_t o = code[pc_++];
            switch (o) {
                case op::PUSHI: pushInt(std::int32_t(readU32(code, pc_))); pc_ += 4; break;
                case op::PUSHS: {
                    pushStr(StringMemory::kCodeBase + pc_);
                    while (pc_ < length && code[pc_] != 0) ++pc_;
                    ++pc_;
                    break;
                }
                case op::ADD: {
                    const bool strings = sp_ >= 2 && (cells_[sp_ - 1].flags & cellflag::String) &&
                                         (cells_[sp_ - 2].flags & cellflag::String);
                    if (strings) {
                        // strcpy(buf, a); strcat(buf, b) with the static buffer 0x44498c.
                        const std::uint32_t b = popStr(), a = popStr();
                        const std::uint32_t buf = StringMemory::kConcatBase;
                        const auto la = mem_.length(a);
                        if (!la) anomaly(std::format("string pointer 0x{:08X} does not point to any string", a));
                        else mem_.copyForward(buf, a, *la + 1);
                        const auto lb = mem_.length(b);
                        if (!lb) {
                            anomaly(std::format("string pointer 0x{:08X} does not point to any string", b));
                        } else {
                            const std::uint32_t end = buf + mem_.length(buf).value_or(0);
                            if (!mem_.copyForward(end, b, *lb + 1)) {
                                mem_.writeByte(buf + StringMemory::kConcatSize - 1, 0);
                                anomaly("string concatenation longer than the original's 1024-byte buffer (truncated)");
                            }
                        }
                        pushStr(buf);
                    } else {
                        const std::int32_t b = popInt(), a = popInt();
                        pushInt(intops::add(a, b));
                    }
                    break;
                }
                case op::SUB: { const std::int32_t b = popInt(), a = popInt(); pushInt(intops::sub(a, b)); break; }
                case op::MUL: { const std::int32_t b = popInt(), a = popInt(); pushInt(intops::mul(a, b)); break; }
                case op::DIV: {
                    const std::int32_t b = popInt(), a = popInt();
                    if (b == 0) pushInt(intops::kDivByZero);
                    else if (intops::divideFaults(a, b)) { divideError("INT_MIN / -1"); pushInt(a); }
                    else pushInt(a / b);
                    break;
                }
                case op::MOD: {
                    const std::int32_t b = popInt(), a = popInt();
                    if (intops::divideFaults(a, b)) { divideError(b == 0 ? "x % 0" : "INT_MIN % -1"); pushInt(0); }
                    else pushInt(a % b);
                    break;
                }
                case op::NEG: pushInt(intops::neg(popInt())); break;
                case op::NOT: pushInt(~popInt()); break;
                case op::XOR: { const std::int32_t b = popInt(), a = popInt(); pushInt(a ^ b); break; }
                case op::OR: { const std::int32_t b = popInt(), a = popInt(); pushInt(a | b); break; }
                case op::AND: { const std::int32_t b = popInt(), a = popInt(); pushInt(a & b); break; }
                case op::EQ:
                case op::NE: {
                    // strcmp if the operand on top of the stack (the right one) is a string.
                    if (sp_ >= 1 && (cells_[sp_ - 1].flags & cellflag::String)) {
                        const std::uint32_t b = popStr(), a = popStr();
                        const int c = compareStrings(a, b);
                        pushInt(o == op::EQ ? (c == 0 ? 1 : 0) : c); // NE pushes -1/0/1 like strcmp
                    } else {
                        const std::int32_t b = popInt(), a = popInt();
                        pushInt(o == op::EQ ? (a == b) : (a != b));
                    }
                    break;
                }
                case op::LOR: { const std::int32_t b = popInt(), a = popInt(); pushInt(a || b); break; }
                case op::LAND: { const std::int32_t b = popInt(), a = popInt(); pushInt(a && b); break; }
                case op::GT: { const std::int32_t b = popInt(), a = popInt(); pushInt(a > b); break; }
                case op::LT: { const std::int32_t b = popInt(), a = popInt(); pushInt(a < b); break; }
                case op::GE: { const std::int32_t b = popInt(), a = popInt(); pushInt(a >= b); break; }
                case op::LE: { const std::int32_t b = popInt(), a = popInt(); pushInt(a <= b); break; }
                case op::LNOT: pushInt(popInt() == 0); break;
                case op::SHR: { const std::int32_t b = popInt(), a = popInt(); pushInt(intops::shr(a, b)); break; }
                case op::SHL: { const std::int32_t b = popInt(), a = popInt(); pushInt(intops::shl(a, b)); break; }
                case op::JZ: {
                    const std::int32_t v = popInt();
                    const std::uint32_t operand = pc_;
                    pc_ = v == 0 ? std::uint32_t(std::int64_t(operand) + readI16(code, operand)) : operand + 2;
                    endOfStatement("if");
                    break;
                }
                case op::JMP: pc_ = std::uint32_t(std::int64_t(pc_) + readI16(code, pc_)); break;
                case op::IFF: {
                    const std::int32_t v = popInt();
                    const std::uint32_t operand = pc_;
                    const std::int16_t rel = readI16(code, operand);
                    const std::int32_t site = operand - 1 < iffSiteAt_.size() ? iffSiteAt_[operand - 1] : -1;
                    if (v != 0) {
                        if (site >= 0) ++iffFires_[std::size_t(site)];
                        std::optional<std::uint32_t> patchAt;
                        if (cfg_.iffMode == IffMode::Patch) patchAt = stmtStart_;
                        else if (cfg_.iffMode == IffMode::OncePerSite && site >= 0)
                            patchAt = script_.iffSites()[std::size_t(site)].conditionStart;
                        if (patchAt && *patchAt < length) {
                            // JMP at the statement start, to the iff's false target.
                            const std::uint16_t r = std::uint16_t(std::uint16_t(rel) - std::uint16_t(*patchAt) + operand - 1);
                            code[*patchAt] = op::JMP;
                            if (*patchAt + 1 < length) code[*patchAt + 1] = std::uint8_t(r & 0xFF);
                            if (*patchAt + 2 < length) code[*patchAt + 2] = std::uint8_t(r >> 8);
                        }
                        pc_ = operand + 2;
                    } else {
                        pc_ = std::uint32_t(std::int64_t(operand) + rel);
                    }
                    endOfStatement("iff");
                    break;
                }
                case op::EOS: endOfStatement("';'"); break;
                case op::PUSHV: {
                    const std::uint8_t var = byteAt(pc_);
                    const Cell* c = cellRef(std::int64_t(var) + index_);
                    if (sp_ >= kCellCount) fail("LOGIC::stack overflow");
                    cells_[sp_] = c ? *c : Cell{};
                    ++sp_;
                    ++pc_;
                    resetIndex();
                    break;
                }
                case op::STORE: {
                    const std::uint8_t var = byteAt(pc_);
                    const std::uint8_t baseFlags = cells_[var].flags;
                    if (baseFlags & cellflag::String) {
                        if (indexActive_ && !(baseFlags & cellflag::Array)) {
                            // s[i] = v on a string scalar: writes the byte v at offset i of its buffer.
                            const std::int32_t b = popInt();
                            const std::uint32_t address = cells_[var].value + std::uint32_t(index_);
                            if (!mem_.writeByte(address, std::uint8_t(b)))
                                anomaly(std::format("byte write at 0x{:08X} outside every string buffer", address));
                        } else {
                            const std::uint32_t src = popStr();
                            if (Cell* dst = cellRef(std::int64_t(var) + index_)) storeString(*dst, src);
                        }
                    } else {
                        const std::int32_t v = popInt();
                        if (Cell* dst = cellRef(std::int64_t(var) + index_)) dst->value = std::uint32_t(v);
                    }
                    ++pc_;
                    resetIndex();
                    ++sp_; // the assigned value stays on the stack (with its own type)
                    break;
                }
                case op::CALL: {
                    const std::uint32_t operand = pc_;
                    if (bp_ + 1 >= kCellCount) fail("LOGIC::stack overflow");
                    cells_[bp_].value = operand + 4; // the flags of that cell are left as they are
                    pc_ = readU32(code, operand);
                    ++bp_;
                    stmtStart_ = pc_;
                    sp_ = bp_;
                    traceCall(pc_);
                    break;
                }
                case op::RET: {
                    if (bp_ == 0 || sp_ == 0) fail("LOGIC::stack pop, but not push");
                    --bp_;
                    --sp_;
                    pc_ = cells_[bp_].value;
                    stmtStart_ = pc_;
                    cells_[bp_] = cells_[sp_];
                    sp_ = bp_ + 1;
                    break;
                }
                case op::POSTINC:
                case op::POSTDEC:
                case op::PREINC:
                case op::PREDEC: {
                    const std::uint8_t var = byteAt(pc_);
                    Cell* c = cellRef(std::int64_t(var) + index_);
                    if (sp_ >= kCellCount) fail("LOGIC::stack overflow");
                    const bool post = o == op::POSTINC || o == op::POSTDEC;
                    const std::uint32_t delta = (o == op::POSTINC || o == op::PREINC) ? 1u : 0xFFFFFFFFu;
                    if (post) cells_[sp_] = c ? *c : Cell{};
                    if (c && (c->flags & cellflag::Int)) c->value += delta; // strings are left alone
                    if (!post) cells_[sp_] = c ? *c : Cell{};
                    ++sp_;
                    ++pc_;
                    resetIndex();
                    break;
                }
                case op::INDEX:
                    index_ = popInt();
                    indexActive_ = true;
                    break;
                default:
                    if (!isBuiltinOpcode(o)) {
                        warn(std::format("LOGIC::Unknown command {} in {}", o, insn_));
                        return; // ends the frame
                    }
                    callBuiltin(o);
            }
        }
    }

    void callBuiltin(std::uint8_t o) {
        const BuiltinInfo* info = script_.builtins().findById(o);
        if (!info) {
            fallback_.id = o;
            const KnownBuiltin* k = findKnownBuiltin(o);
            fallback_.name = k ? std::string{k->name} : std::format("builtin{}", o);
            info = &fallback_;
        }
        const std::size_t n = knownBuiltinArity(o);
        if (std::int64_t(sp_) - std::int64_t(n) < std::int64_t(bp_)) fail("LOGIC::stack pop, but not push");
        std::vector<Value> args;
        args.reserve(n);
        for (std::size_t k = sp_ - n; k < sp_; ++k) args.push_back(valueOfChecked(cells_[k]));
        sp_ -= std::uint32_t(n);
        const Value r = host_.callBuiltin(*info, args);
        if (!host_.builtinPushesResult(*info, args)) return;
        if (r.isString()) pushStr(hostString(o, r.stringValue()));
        else pushInt(r.intValue());
    }

    static constexpr std::uint32_t kApiSlot = 255;     // host buffer used by set()/call() arguments
    static constexpr std::uint32_t kInternSlot = 256;  // first host buffer of interned results

    Script script_;
    ScriptHost& host_;
    ExecConfig cfg_;
    StringMemory mem_;
    std::vector<Cell> cells_;
    std::vector<std::uint32_t> stringBuffers_; // buffer of each string cell, in cell order
    std::uint32_t globalCount_ = 0;
    std::vector<std::int32_t> iffSiteAt_;      // code offset of an IFF opcode -> site index
    std::vector<std::uint32_t> iffFires_;
    std::map<std::string, std::uint32_t> interned_;
    BuiltinInfo fallback_;
    // VM registers.
    std::uint32_t pc_ = 0, insn_ = 0, sp_ = 0, bp_ = 0, stmtStart_ = 0;
    std::int32_t index_ = 0;
    bool indexActive_ = false;
    std::uint64_t steps_ = 0, ticks_ = 0;
};

} // namespace gromada::script
