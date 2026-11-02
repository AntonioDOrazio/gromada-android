#include "libgromada/libgromada_all.hpp"
#include "test.hpp"

int main() {
    SKIP_WITHOUT_DATA();
    const auto dir = *gromada_test::dataDir();
    GameResources res{dir / "fw.res"};
    CHECK_EQ(res.vids().size(), std::size_t{368});
    CHECK_EQ(res.sounds().size(), std::size_t{47});
    CHECK_EQ(res.weapons().size(), std::size_t{31});
    // Type byte of each sound (issue #17): 0 = looping, others one-shot / 100 non-positional.
    // Effect number = fw.res index (0-based, 0x408730). The looping ones are ambient and engine
    // sounds: teleporter hum 14, idle engines 40, moving engines 42..44, bubble generator 46...
    for (int e = 0; e < 47; ++e) {
        const bool looping = e == 14 || e == 17 || e == 29 || e == 31 || (e >= 40 && e <= 46);
        CHECK_EQ(res.sounds()[static_cast<std::size_t>(e)].kind == 0, looping);
    }
    for (int i = 1; i <= 30; ++i) {
        const auto map = loadMap(res.vids(), res.mapsPath() / "levels" / std::format("{:02}.map", i));
        CHECK(map.header.width > 0 && !map.objects.empty());
    }
    return gromada_test::finish();
}
