#pragma once
// Minimal test helpers (no framework). Each test is its own executable.
//  - CHECK(cond) / CHECK_EQ(a, b): record a failure and continue.
//  - gromada_test::dataDir(): game data folder from $GROMADA_DATA, or nullopt.
//    Tests that need the original game files must call SKIP_WITHOUT_DATA() first:
//    CI has no game data (it is copyrighted and never committed).
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

namespace gromada_test {
inline int failures = 0;
inline std::optional<std::filesystem::path> dataDir() {
    if (const char* d = std::getenv("GROMADA_DATA"); d && *d) return std::filesystem::path{d};
    return std::nullopt;
}
inline int finish() {
    if (failures) std::fprintf(stderr, "FAILED: %d check(s)\n", failures);
    else std::fprintf(stderr, "OK\n");
    return failures ? 1 : 0;
}
} // namespace gromada_test

#define CHECK(cond) do { if (!(cond)) { ++gromada_test::failures; \
    std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { const auto& _a = (a); const auto& _b = (b); if (!(_a == _b)) { ++gromada_test::failures; \
    std::fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s\n", __FILE__, __LINE__, #a, #b); } } while (0)
#define SKIP_WITHOUT_DATA() do { if (!gromada_test::dataDir()) { \
    std::fprintf(stderr, "SKIP (set GROMADA_DATA to the game folder)\n"); return 77; } } while (0)
