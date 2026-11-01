// Demo (.dem) parser: synthetic round trip + check on the original demos (needs GROMADA_DATA).
#include "demo/demo_file.hpp"
#include "test.hpp"

#include <fstream>
#include <iterator>

using namespace gromada::demo;

namespace {
void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(std::uint8_t(x >> (8 * i)));
}

std::vector<std::uint8_t> synthetic(std::size_t globals) {
    std::vector<std::uint8_t> s;  // section payload
    const std::string name = "maps\\levels\\05.map";
    put32(s, std::uint32_t(name.size()));
    s.insert(s.end(), name.begin(), name.end());
    put32(s, 0x1234);       // seed
    put32(s, 1000000);      // clock
    for (std::size_t i = 0; i < globals; ++i) put32(s, std::uint32_t(i * 7));
    put32(s, 1000071);      // tick 0
    for (int b : {10, 0, 20, 0, 1, 0, 2, 0, 0x11, 0, int('c'), 0}) s.push_back(std::uint8_t(b));
    put32(s, 3);            // FRead during tick 0
    for (char c : std::string("abc")) s.push_back(std::uint8_t(c));
    put32(s, 1000100);      // tick 1
    for (int i = 0; i < 12; ++i) s.push_back(0);
    put32(s, 0xFFFFFFFFu);
    std::vector<std::uint8_t> f;
    put32(f, 1);
    f.push_back(7);
    put32(f, std::uint32_t(s.size() + 6));  // nextSectionOffset: header tail (4+2) + data
    put32(f, 1);
    f.push_back(0); f.push_back(0);
    f.insert(f.end(), s.begin(), s.end());
    const std::string trailer = "maps\\mainmenu.map";
    f.insert(f.end(), trailer.begin(), trailer.end());
    return f;
}
} // namespace

int main() {
    {
        auto blob = synthetic(5);
        auto d = parseDemo(blob);
        CHECK(d.has_value());
        if (d) {
            CHECK_EQ(d->mapName, std::string("maps\\levels\\05.map"));
            CHECK_EQ(d->seed, 0x1234u);
            CHECK_EQ(d->intGlobals.size(), std::size_t(5));
            CHECK_EQ(d->ticks.size(), std::size_t(2));
            CHECK_EQ(d->ticks[0].input.inputBits, std::uint16_t(0x11));
            CHECK_EQ(d->ticks[0].input.key, std::uint8_t('c'));
            CHECK_EQ(d->freads.size(), std::size_t(1));
            CHECK_EQ(d->freads[0].tick, std::size_t(0));
            CHECK_EQ(d->freads[0].text, std::string("abc"));
            CHECK_EQ(d->nextMap, std::string("maps\\mainmenu.map"));
        }
        CHECK(parseDemo(blob, 5).has_value());
        CHECK(!parseDemo(blob, 4).has_value());
    }

    SKIP_WITHOUT_DATA();
    struct Expect { const char* file; const char* map; std::size_t globals, ticks; };
    for (const Expect& e : {Expect{"demo1.dem", "maps\\levels\\10.map", 157, 6379},
                            Expect{"demo2.dem", "maps\\levels\\06.map", 146, 6792},
                            Expect{"demo3.dem", "maps\\levels\\14.map", 146, 5568}}) {
        std::ifstream in(*gromada_test::dataDir() / "maps" / e.file, std::ios::binary);
        std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in), {}};
        auto d = parseDemo(bytes);
        CHECK(d.has_value());
        if (!d) continue;
        CHECK_EQ(d->mapName, std::string(e.map));
        CHECK_EQ(d->intGlobals.size(), e.globals);  // = int cells of the level script
        CHECK_EQ(d->ticks.size(), e.ticks);
        CHECK_EQ(d->freads.size(), std::size_t(19)); // LoadPlayer() in the first Tact()
        CHECK_EQ(d->nextMap, std::string("maps\\mainmenu.map"));
        CHECK_EQ(d->ticks.front().clock, d->startClock + 71);
        int backwards = 0;
        for (std::size_t i = 1; i < d->ticks.size(); ++i) {
            const auto dt = std::int64_t(d->ticks[i].clock) - std::int64_t(d->ticks[i - 1].clock);
            if (dt < 0) ++backwards;          // Pause(0) restores the clock saved by Pause(1)
            else CHECK(dt <= 71);             // per-frame clock step is clamped to 71 ms
        }
        CHECK_EQ(backwards, 1);
    }
    return gromada_test::finish();
}
