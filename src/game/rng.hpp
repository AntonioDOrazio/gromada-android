#pragma once
// The MSVC CRT rand() shared by the engine and the scripts (docs/re/engine.md §10,
// gromada.exe 0x41c7b5 / srand 0x41c7ab). Header-only.

#include <cstdint>

namespace gromada::game {

struct MsvcRand {
    std::uint32_t state = 1; // CRT initial value

    void seed(std::uint32_t s) noexcept { state = s; }
    // state = state * 214013 + 2531011; return (state >> 16) & 0x7fff
    int next() noexcept {
        state = state * 214013u + 2531011u;
        return static_cast<int>((state >> 16) & 0x7fffu);
    }
};

} // namespace gromada::game
