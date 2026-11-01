#pragma once
// Parser for Gromada demo recordings (maps/demo*.dem). Header-only, depends only on the
// standard library. Format reverse engineered from gromada.exe (see docs/re/runtime.md, "Demo"):
//
//   resource container: u32 sectionCount (=1), then one section of type 7
//   section header: u8 type, u32 nextSectionOffset, u32 elementCount, u16 dataOffset
//   section data:
//     u32 nameLength, char name[nameLength]   map loaded by the demo (e.g. maps\levels\10.map)
//     u32 seed                                srand() seed of the map (MSVC LCG)
//     u32 clock                               game clock (GetTime) at map start
//     u32 intGlobals[N]                       every int cell of the map script, in declaration
//                                             order (array elements included)
//     per game tick:  u32 clock, u8 input[12]
//     per FRead():    u32 length, char text[length]   (inside the tick that called it)
//     u32 0xFFFFFFFF                          end of recording
//   after the section: name of the map to load when the demo ends (no terminator)
//
// N depends on the script; when it is not given, the parser finds the only N for which the
// first tick follows the clock of the header and the whole stream ends exactly at the marker.
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gromada::demo {

struct DemoInput {             // the 12-byte input snapshot of one tick (obj+0x32dd in the exe)
    std::int16_t worldMouseX;  // screen mouse + camera position
    std::int16_t worldMouseY;
    std::int16_t screenMouseX;
    std::int16_t screenMouseY;
    std::uint16_t inputBits;   // GetInputState(): INPUT_LCLICK 1, RCLICK 2, LEFT 4, RIGHT 8,
                               // LDOWN 16, RDOWN 32, UP 64, DOWN 128, 0x100 = Shift held
    std::uint8_t key;          // GetKey(): WM_CHAR code, or 1..4 = arrows, 5 = Delete
    std::uint8_t unused;
};

struct DemoTick {
    std::uint32_t clock;       // GetTime() during this tick
    DemoInput input;
};

struct DemoFRead {
    std::size_t tick;          // index of the tick during which the script called FRead()
    std::string text;
};

struct DemoFile {
    std::string mapName;
    std::uint32_t seed = 0;
    std::uint32_t startClock = 0;
    std::vector<std::uint32_t> intGlobals;
    std::vector<DemoTick> ticks;
    std::vector<DemoFRead> freads;
    std::string nextMap;       // trailer after the section
};

namespace detail {
inline bool readU32(std::span<const std::uint8_t> d, std::size_t o, std::uint32_t& v) {
    if (o + 4 > d.size()) return false;
    v = std::uint32_t(d[o]) | std::uint32_t(d[o + 1]) << 8 | std::uint32_t(d[o + 2]) << 16 |
        std::uint32_t(d[o + 3]) << 24;
    return true;
}
inline std::int16_t s16(std::span<const std::uint8_t> d, std::size_t o) {
    return static_cast<std::int16_t>(d[o] | d[o + 1] << 8);
}

// Parses the record stream starting at `o`; returns the offset after the end marker.
inline std::optional<std::size_t> parseStream(std::span<const std::uint8_t> d, std::size_t o,
                                              std::size_t end, DemoFile* out) {
    for (;;) {
        std::uint32_t v;
        if (o + 4 > end || !readU32(d, o, v)) return std::nullopt;
        if (v == 0xFFFFFFFFu) return o + 4;
        if (v < 0x10000u) {    // FRead record: lines are at most 127 bytes, clocks are large
            if (o + 4 + v > end) return std::nullopt;
            if (out) out->freads.push_back({out->ticks.empty() ? 0 : out->ticks.size() - 1,
                                            std::string(reinterpret_cast<const char*>(&d[o + 4]), v)});
            o += 4 + v;
            continue;
        }
        if (o + 16 > end) return std::nullopt;
        if (out) {
            DemoTick t{v, {s16(d, o + 4), s16(d, o + 6), s16(d, o + 8), s16(d, o + 10),
                           static_cast<std::uint16_t>(d[o + 12] | d[o + 13] << 8), d[o + 14], d[o + 15]}};
            out->ticks.push_back(t);
        }
        o += 16;
    }
}
} // namespace detail

// intGlobalCount: number of int cells of the map script, or nullopt to detect it.
inline std::optional<DemoFile> parseDemo(std::span<const std::uint8_t> d,
                                         std::optional<std::size_t> intGlobalCount = std::nullopt) {
    using namespace detail;
    std::uint32_t sections, next, count;
    if (!readU32(d, 0, sections) || sections != 1 || d.size() < 15 || d[4] != 7) return std::nullopt;
    readU32(d, 5, next);
    readU32(d, 9, count);
    const std::size_t dataOffset = d[13] | d[14] << 8;
    const std::size_t end = 4 + 5 + std::size_t(next);
    if (end > d.size()) return std::nullopt;
    std::size_t o = 15 + dataOffset;

    DemoFile f;
    std::uint32_t n;
    if (!readU32(d, o, n) || o + 4 + n + 8 > end) return std::nullopt;
    f.mapName.assign(reinterpret_cast<const char*>(&d[o + 4]), n);
    o += 4 + n;
    readU32(d, o, f.seed);
    readU32(d, o + 4, f.startClock);
    o += 8;

    auto tryCount = [&](std::size_t k) -> bool {
        std::uint32_t first;
        const std::size_t p = o + 4 * k;
        if (!readU32(d, p, first) || first == 0xFFFFFFFFu) return false;
        // the first tick runs right after the load: clock advanced by at most 71 ms
        if (first < f.startClock || first > f.startClock + 71) return false;
        return parseStream(d, p, end, nullptr) == end;
    };
    std::size_t k = 0;
    if (intGlobalCount) {
        k = *intGlobalCount;
        if (!tryCount(k)) return std::nullopt;
    } else {
        bool found = false;
        for (std::size_t i = 0; i <= 256 && o + 4 * i < end; ++i)  // scripts have < 256 cells
            if (tryCount(i)) { k = i; found = true; break; }
        if (!found) return std::nullopt;
    }
    f.intGlobals.resize(k);
    for (std::size_t i = 0; i < k; ++i) readU32(d, o + 4 * i, f.intGlobals[i]);
    parseStream(d, o + 4 * k, end, &f);
    f.nextMap.assign(reinterpret_cast<const char*>(d.data() + end), d.size() - end);
    return f;
}

} // namespace gromada::demo
