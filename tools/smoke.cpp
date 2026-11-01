#include "libgromada/libgromada_all.hpp"
#include <print>
int main(int argc, char** argv) {
    GameResources res{std::filesystem::path{argv[1]} / "fw.res"};
    std::println("vids={} sounds={} weapons={} baseTiles={}", res.vids().size(), res.sounds().size(), res.weapons().size(), res.baseTilesVids().size());
    for (int i = 1; i <= 30; ++i) {
        auto p = res.mapsPath() / "levels" / std::format("{:02}.map", i);
        if (!std::filesystem::exists(p)) continue;
        try {
            auto map = loadMap(res.vids(), p);
            std::println("{:02}.map {}x{} objects={} ver={}", i, map.header.width, map.header.height, map.objects.size(), (int)map.header.mapVersion);
        } catch (const std::exception& e) { std::println("{:02}.map FAIL {}", i, e.what()); }
    }
}
