#pragma once
// Emulated address space for the strings of the script VM.
//
// In the original a string cell holds a `char*`: into the code (literals), into a variable's
// 128-byte heap buffer, into the static concatenation buffer (0x44498c) or the static _itoa
// buffer (0x44e590), or into a builtin's static result buffer. Several values in one statement
// can therefore alias the same bytes (`a+b == c+d` compares the concatenation buffer with
// itself). To reproduce this, string values are 32-bit addresses into blocks of this class and
// every copy goes through the same byte-level operations as the original (strcpy/strcat done
// with forward `rep movsd`/`rep movsb`, byte writes for `s[i]=v`).
//
// Addresses are deterministic but of course not the original's heap addresses. Reading or
// writing outside every block (a dangling or garbage pointer) does nothing and is reported.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gromada::script {

class StringMemory {
public:
    friend struct SaveStateAccess; // save states (script/vm_state.hpp)
    // Fixed blocks (the two static buffers keep the original's addresses).
    static constexpr std::uint32_t kConcatBase = 0x0044498C; // 1024 bytes, up to 0x444d8c
    static constexpr std::uint32_t kConcatSize = 1024;
    static constexpr std::uint32_t kItoaBase = 0x0044E590;
    static constexpr std::uint32_t kItoaSize = 16;
    static constexpr std::uint32_t kHeapBase = 0x00800000;   // variable buffers
    static constexpr std::uint32_t kHeapEnd = 0x01000000;
    static constexpr std::uint32_t kCodeBase = 0x01000000;   // the bytecode (string literals)
    static constexpr std::uint32_t kHostBase = 0x02000000;   // builtin results
    static constexpr std::uint32_t kHostBufferSize = 0x10000;

    StringMemory() {
        blocks_[kConcatBase] = Block(kConcatSize, 0);
        blocks_[kItoaBase] = Block(kItoaSize, 0);
    }

    // ---- blocks ----
    std::vector<std::uint8_t>& code() { return blocks_[kCodeBase]; }
    const std::vector<std::uint8_t>& code() const { return blocks_.at(kCodeBase); }
    void setCode(std::vector<std::uint8_t> code) { blocks_[kCodeBase] = std::move(code); }

    // malloc: first fit in the heap area; the block is zero-filled.
    std::uint32_t alloc(std::uint32_t size) {
        size = std::max<std::uint32_t>(size, 1);
        std::uint32_t at = kHeapBase;
        for (auto it = blocks_.lower_bound(kHeapBase); it != blocks_.end() && it->first < kHeapEnd; ++it) {
            if (it->first >= at + size + kGap) break;
            at = std::max(at, it->first + std::uint32_t(it->second.size()) + kGap);
        }
        at = (at + 15u) & ~15u;
        if (at + size > kHeapEnd) return 0;
        blocks_[at] = Block(size, 0);
        return at;
    }
    // free: only heap blocks, only by their base address (like the CRT: anything else is a bug
    // of the script that the original would not survive; here it is ignored).
    bool free(std::uint32_t address) {
        if (address < kHeapBase || address >= kHeapEnd) return false;
        return blocks_.erase(address) > 0;
    }
    bool isHeapBlock(std::uint32_t address) const {
        return address >= kHeapBase && address < kHeapEnd && blocks_.contains(address);
    }

    // Static result buffer of a builtin: `content` replaces its previous content.
    std::uint32_t hostBuffer(std::uint32_t slot, std::string_view content) {
        const std::uint32_t base = kHostBase + slot * kHostBufferSize;
        Block& b = blocks_[base];
        const std::size_t n = std::min<std::size_t>(content.size(), kHostBufferSize - 1);
        b.assign(content.begin(), content.begin() + std::ptrdiff_t(n));
        b.push_back(0);
        return base;
    }

    // ---- byte access ----
    bool valid(std::uint32_t address) const { return find(address).has_value(); }
    std::optional<std::uint8_t> readByte(std::uint32_t address) const {
        const auto loc = find(address);
        if (!loc) return std::nullopt;
        return (*loc->block)[loc->offset];
    }
    bool writeByte(std::uint32_t address, std::uint8_t v) {
        const auto loc = find(address);
        if (!loc) return false;
        (*loc->block)[loc->offset] = v;
        return true;
    }

    // The NUL-terminated string at `address`; nullopt if the address is not in any block. A
    // string that runs to the end of its block without a terminator is cut there.
    std::optional<std::string> readString(std::uint32_t address) const {
        const auto loc = find(address);
        if (!loc) return std::nullopt;
        const auto& b = *loc->block;
        std::size_t e = loc->offset;
        while (e < b.size() && b[e] != 0) ++e;
        return std::string(reinterpret_cast<const char*>(b.data() + loc->offset), e - loc->offset);
    }
    // strlen; nullopt for an invalid address.
    std::optional<std::uint32_t> length(std::uint32_t address) const {
        const auto loc = find(address);
        if (!loc) return std::nullopt;
        const auto& b = *loc->block;
        std::size_t e = loc->offset;
        while (e < b.size() && b[e] != 0) ++e;
        return std::uint32_t(e - loc->offset);
    }

    // Copies `n` bytes forward like `rep movsd` (n/4 dwords) then `rep movsb` (n%4 bytes): each
    // dword is read then written, so overlapping copies behave like the original's inline
    // strcpy/strcat. Bytes outside every block are skipped (reads give 0). Returns false if any
    // byte was outside.
    bool copyForward(std::uint32_t dst, std::uint32_t src, std::uint32_t n) {
        bool ok = true;
        std::uint32_t i = 0;
        for (; i + 4 <= n; i += 4) {
            std::uint8_t t[4];
            for (std::uint32_t k = 0; k < 4; ++k) {
                const auto b = readByte(src + i + k);
                ok &= b.has_value();
                t[k] = b.value_or(0);
            }
            for (std::uint32_t k = 0; k < 4; ++k) ok &= writeByte(dst + i + k, t[k]);
        }
        for (; i < n; ++i) {
            const auto b = readByte(src + i);
            ok &= b.has_value();
            ok &= writeByte(dst + i, b.value_or(0));
        }
        return ok;
    }

private:
    using Block = std::vector<std::uint8_t>;
    static constexpr std::uint32_t kGap = 16; // keeps neighbouring heap blocks apart

    struct Location {
        Block* block;
        std::size_t offset;
    };
    std::optional<Location> find(std::uint32_t address) const {
        auto it = blocks_.upper_bound(address);
        if (it == blocks_.begin()) return std::nullopt;
        --it;
        const std::uint64_t off = std::uint64_t(address) - it->first;
        if (off >= it->second.size()) return std::nullopt;
        return Location{const_cast<Block*>(&it->second), std::size_t(off)};
    }

    std::map<std::uint32_t, Block> blocks_;
};

} // namespace gromada::script
