#include "libgromada/libgromada_all.hpp"
#include "test.hpp"

int main() {
    SKIP_WITHOUT_DATA();
    const auto dir = *gromada_test::dataDir();
    GameResources res{dir / "fw.res"};
    CHECK_EQ(res.vids().size(), std::size_t{368});
    CHECK_EQ(res.sounds().size(), std::size_t{47});
    CHECK_EQ(res.weapons().size(), std::size_t{31});
    // Type byte of each sound (issue #17): 0 = looping (engines...), others one-shot / 100
    // non-positional. Effect numbers are 1-based.
    for (int n = 1; n <= 47; ++n) {
        const bool looping = n == 15 || n == 18 || n == 30 || n == 32 || (n >= 41 && n <= 47);
        CHECK_EQ(res.sounds()[n - 1].kind == 0, looping);
    }
    for (int i = 1; i <= 30; ++i) {
        const auto map = loadMap(res.vids(), res.mapsPath() / "levels" / std::format("{:02}.map", i));
        CHECK(map.header.width > 0 && !map.objects.empty());
    }
    return gromada_test::finish();
}
