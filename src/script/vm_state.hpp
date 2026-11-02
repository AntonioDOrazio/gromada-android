#pragma once
// Save states of the script VM (issue #14, docs/savestates.md): everything an Interpreter
// carries from one frame to the next -- the 2048 cells, the emulated string memory (the code
// as patched by `iff`, heap buffers, the static concatenation/_itoa buffers, the builtins'
// result buffers), the buffer of each string cell, iff counters, interned strings and the
// registers. The compiled Script itself is not stored: the state records its identity
// (hash of the compiled code and its sizes) and is only accepted by an interpreter of the
// same script.
//
// decode() validates everything into a staged VmState without touching the interpreter;
// apply() then cannot fail. Header-only; depends on core/state_io.hpp and the interpreter.

#include "core/state_io.hpp"
#include "script/interpreter.hpp"

namespace gromada::script {

struct VmIdentity {
    std::uint64_t codeHash = 0;
    std::uint32_t codeSize = 0, cellCount = 0, iffSites = 0;
    friend bool operator==(const VmIdentity&, const VmIdentity&) = default;
};

inline VmIdentity vmIdentityOf(const Script& s) {
    return {state::fnv1a(std::span<const std::uint8_t>{s.code()}), static_cast<std::uint32_t>(s.code().size()),
            static_cast<std::uint32_t>(s.cellCount()), static_cast<std::uint32_t>(s.iffSites().size())};
}

inline void writeIdentity(state::Writer& w, const VmIdentity& id) {
    w.u64(id.codeHash);
    w.u32(id.codeSize);
    w.u32(id.cellCount);
    w.u32(id.iffSites);
}
inline VmIdentity readIdentity(state::Reader& r) {
    VmIdentity id;
    id.codeHash = r.u64();
    id.codeSize = r.u32();
    id.cellCount = r.u32();
    id.iffSites = r.u32();
    return id;
}

// Decoded, validated state of an interpreter (see SaveStateAccess::decode).
struct VmState {
    std::vector<Cell> cells;
    std::map<std::uint32_t, std::vector<std::uint8_t>> blocks;
    std::vector<std::uint32_t> stringBuffers;
    std::vector<std::uint32_t> iffFires;
    std::map<std::string, std::uint32_t> interned;
    std::uint32_t pc = 0, insn = 0, sp = 0, bp = 0, stmtStart = 0;
    std::int32_t index = 0;
    bool indexActive = false;
    std::uint64_t steps = 0, ticks = 0;
};

inline void writeCells(state::Writer& w, std::span<const Cell> cells) {
    w.count(cells.size());
    for (const Cell& c : cells) {
        w.u32(c.value);
        w.u8(c.flags);
    }
}
inline std::vector<Cell> readCells(state::Reader& r, std::size_t expected) {
    const std::size_t n = r.count(5, kCellCount);
    if (n != expected) throw state::Error("save state: wrong number of script cells");
    std::vector<Cell> cells(n);
    for (Cell& c : cells) {
        c.value = r.u32();
        c.flags = r.u8();
    }
    return cells;
}

struct SaveStateAccess {
    static constexpr std::uint32_t Tag = state::fourcc("SVM1");

    static void save(state::Writer& w, const Interpreter& vm) {
        const std::size_t chunk = w.beginChunk(Tag);
        writeIdentity(w, vmIdentityOf(vm.script_));
        writeCells(w, vm.cells_);
        const auto& blocks = vm.mem_.blocks_;
        w.count(blocks.size());
        for (const auto& [address, bytes] : blocks) {
            w.u32(address);
            w.blob(bytes);
        }
        w.array(std::span<const std::uint32_t>{vm.stringBuffers_});
        w.array(std::span<const std::uint32_t>{vm.iffFires_});
        w.count(vm.interned_.size());
        for (const auto& [s, address] : vm.interned_) {
            w.str(s);
            w.u32(address);
        }
        w.u32(vm.pc_);
        w.u32(vm.insn_);
        w.u32(vm.sp_);
        w.u32(vm.bp_);
        w.u32(vm.stmtStart_);
        w.i32(vm.index_);
        w.boolean(vm.indexActive_);
        w.u64(vm.steps_);
        w.u64(vm.ticks_);
        w.endChunk(chunk);
    }

    // Reads the identity at the start of a VM chunk without consuming the reader.
    static VmIdentity peekIdentity(state::Reader r) {
        state::Reader c = r.chunk(Tag);
        return readIdentity(c);
    }

    // Validates a VM chunk against `script` (the script the interpreter runs).
    static VmState decode(state::Reader& outer, const Script& script) {
        state::Reader r = outer.chunk(Tag);
        if (readIdentity(r) != vmIdentityOf(script)) throw state::Error("save state: the script differs from the saved one");
        VmState s;
        s.cells = readCells(r, kCellCount);

        const std::size_t nBlocks = r.count(8, 1u << 16);
        std::uint64_t total = 0;
        std::uint64_t prevEnd = 0;
        for (std::size_t i = 0; i < nBlocks; ++i) {
            const std::uint32_t address = r.u32();
            auto bytes = r.blob(1u << 24);
            total += bytes.size();
            if (total > (64u << 20)) throw state::Error("save state: script memory too large");
            if (address < prevEnd) throw state::Error("save state: overlapping script memory blocks");
            prevEnd = std::uint64_t(address) + bytes.size();
            if (prevEnd > 0xFFFFFFFFull) throw state::Error("save state: script memory block out of range");
            const bool heap = address >= StringMemory::kHeapBase && address < StringMemory::kHeapEnd;
            const bool host = address >= StringMemory::kHostBase;
            const bool fixed = address == StringMemory::kConcatBase || address == StringMemory::kItoaBase ||
                               address == StringMemory::kCodeBase;
            if (!heap && !host && !fixed) throw state::Error("save state: unknown script memory block");
            if (heap && prevEnd > StringMemory::kHeapEnd) throw state::Error("save state: heap block out of range");
            if (host && bytes.size() > StringMemory::kHostBufferSize) throw state::Error("save state: host buffer too large");
            s.blocks.emplace(address, std::move(bytes));
        }
        const auto need = [&](std::uint32_t address, std::size_t size, const char* what) {
            const auto it = s.blocks.find(address);
            if (it == s.blocks.end() || it->second.size() != size) throw state::Error(std::string{"save state: bad script block "} + what);
        };
        need(StringMemory::kConcatBase, StringMemory::kConcatSize, "concat");
        need(StringMemory::kItoaBase, StringMemory::kItoaSize, "itoa");
        need(StringMemory::kCodeBase, script.code().size(), "code");

        std::size_t stringCells = 0;
        for (const CellDecl& d : script.cells()) stringCells += (d.flags & cellflag::String) ? 1 : 0;
        s.stringBuffers = r.array<std::uint32_t>(kCellCount);
        if (s.stringBuffers.size() != stringCells) throw state::Error("save state: wrong number of string buffers");
        s.iffFires = r.array<std::uint32_t>(1u << 20);
        if (s.iffFires.size() != script.iffSites().size()) throw state::Error("save state: wrong number of iff sites");
        const std::size_t nInterned = r.count(8, 4096);
        for (std::size_t i = 0; i < nInterned; ++i) {
            std::string str = r.str(StringMemory::kHostBufferSize);
            const std::uint32_t address = r.u32();
            s.interned.emplace(std::move(str), address);
        }
        s.pc = r.u32();
        s.insn = r.u32();
        s.sp = r.ranged<std::uint32_t>(0, kCellCount, "vm sp");
        s.bp = r.ranged<std::uint32_t>(0, kCellCount, "vm bp");
        s.stmtStart = r.u32();
        s.index = r.i32();
        s.indexActive = r.boolean();
        s.steps = r.u64();
        s.ticks = r.u64();
        r.expectEnd("script VM");
        return s;
    }

    static void apply(Interpreter& vm, VmState&& s) {
        vm.cells_ = std::move(s.cells);
        vm.mem_.blocks_ = std::move(s.blocks);
        vm.stringBuffers_ = std::move(s.stringBuffers);
        vm.iffFires_ = std::move(s.iffFires);
        vm.interned_ = std::move(s.interned);
        vm.pc_ = s.pc;
        vm.insn_ = s.insn;
        vm.sp_ = s.sp;
        vm.bp_ = s.bp;
        vm.stmtStart_ = s.stmtStart;
        vm.index_ = s.index;
        vm.indexActive_ = s.indexActive;
        vm.steps_ = s.steps;
        vm.ticks_ = s.ticks;
    }
};

} // namespace gromada::script
