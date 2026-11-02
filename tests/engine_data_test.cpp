// Data-backed tests of the simulation engine (needs GROMADA_DATA, skipped in CI):
// every level loads and runs 60 s of game time at 16.7 and 8.3 ms steps with the player's
// Kassandra placed like the level scripts do; enemies move and fight, the flagman obeys
// move/fire orders, runs are deterministic, level 19 fits the 4 ms frame budget, and the
// direction tables match gromada.exe.
#include "game/engine.hpp"
#include "test.hpp"

#include <chrono>
#include <map>
#include <cstdio>
#include <regex>

using namespace gromada;
using namespace gromada::game;

namespace {

std::pair<int, int> spawnFor(const std::filesystem::path& ini, int fallbackX, int fallbackY) {
    std::ifstream f(ini, std::ios::binary);
    if (!f) return {fallbackX, fallbackY};
    const std::string text{std::istreambuf_iterator<char>(f), {}};
    std::smatch m, my;
    if (std::regex_search(text, m, std::regex(R"(Create\(kassandra,\s*(\d+)\s*,\s*(\d+))")))
        return {std::stoi(m[1]), std::stoi(m[2])};
    if (std::regex_search(text, m, std::regex(R"(tp_x\[\d+\]\s*=\s*\{\s*(\d+))")) &&
        std::regex_search(text, my, std::regex(R"(tp_y\[\d+\]\s*=\s*\{\s*(\d+))")))
        return {std::stoi(m[1]), std::stoi(my[1])};
    return {fallbackX, fallbackY};
}

struct RunResult {
    bool flagmanResponded = false;
    bool flagmanAliveAtOrder = false;
    int enemyMoved = 0;
    int damageEvents = 0;
    int shots = 0;           // fight animations started by units
    std::uint64_t hash = 0;
    double simP99 = 0, simMax = 0, drawAvg = 0, drawMax = 0;
    std::size_t sprites = 0;
};

RunResult runLevel(const GameResources& res, const std::filesystem::path& data, int level, double step, std::uint32_t seed,
                   bool measureDraw) {
    char name[32];
    std::snprintf(name, sizeof name, "%02d", level);
    const auto mapFile = data / "maps" / "levels" / (std::string(name) + ".map");
    auto ini = mapFile;
    ini.replace_extension(".INI");
    if (!std::filesystem::exists(ini)) ini.replace_extension(".ini");

    GameEngine e{res};
    e.loadMap(mapFile, seed);
    World& w = e.world();
    RunResult r;
    w.damageHook = [&](const Sprite&, int, int) { ++r.damageEvents; };
    auto call = [&](int id, std::initializer_list<script::Value> args) {
        std::vector<script::Value> v(args);
        return e.callBuiltin(id, v).value_or(script::Value{0});
    };
    const auto [sx, sy] = spawnFor(ini, e.camera().x + 320, e.camera().y + 240);
    call(127, {81, 1, 60});
    const int flagman = call(68, {81, sx, sy, 0}).toInt();
    call(119, {0, flagman});
    w.players()[0].type = 1;
    call(67, {flagman, 35, 88, 0});
    const int ammo[8] = {20, 80, 60, 10, 10, 0, 0, 0};
    for (int k = 0; k < 8; ++k) call(67, {flagman, 63, ammo[k], k});
    call(126, {sx, sy});
    call(128, {1});

    std::map<UnitId, std::pair<int, int>> start;
    for (Sprite* u : w.gridUnits())
        if (w.vidOf(*u).army != 0) start[u->id] = {u->x, u->y};

    std::vector<RGBA8> pixels(640 * 480);
    FramebufferRef fb{pixels.data(), 480, 640};
    std::vector<double> sim;
    double drawSum = 0;
    int draws = 0;
    int orderX = 0, orderY = 0;
    double t = 0;
    const double total = 60000;
    bool ordered = false;
    while (t < total) {
        const int dt = static_cast<int>(std::floor(t + step) - std::floor(t));
        t += step;
        OriginalInput in;
        in.mouseScreenX = 320;
        in.mouseScreenY = 240;
        Sprite* f = w.byId(flagman);
        if (!ordered && t >= 1000) {
            ordered = true;
            if (f) {
                r.flagmanAliveAtOrder = true;
                orderX = f->x + 160;
                orderY = f->y + 40;
                in.mouseWorldX = static_cast<std::int16_t>(orderX);
                in.mouseWorldY = static_cast<std::int16_t>(orderY);
                in.bits = OriginalInput::LClick | OriginalInput::LDown;
            }
        } else if (t >= 4000 && t < 5000 && f) {
            in.mouseWorldX = static_cast<std::int16_t>(f->x + 100);
            in.mouseWorldY = static_cast<std::int16_t>(f->y + 100);
            in.bits = OriginalInput::RDown;
        }
        const auto t0 = std::chrono::steady_clock::now();
        e.beginFrame(dt);
        e.setInput(in);
        e.tactWorld();
        e.tactPlayers();
        e.tactGroupAi();
        e.updateCamera();
        e.takeSounds();
        sim.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        if (measureDraw || static_cast<int>(t / step) % 600 == 0) {
            const auto d0 = std::chrono::steady_clock::now();
            e.draw(fb);
            const double d = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - d0).count();
            drawSum += d;
            r.drawMax = std::max(r.drawMax, d);
            ++draws;
        }
        if (Sprite* fl = w.byId(flagman); fl && ordered && t < 4000 && !r.flagmanResponded) {
            if (fl->wpX == orderX || std::abs(fl->x - (orderX - 160)) > 4 || std::abs(fl->y - (orderY - 40)) > 4)
                r.flagmanResponded = true;
        }
    }
    r.shots = static_cast<int>(w.fireCount);
    for (Sprite* u : w.gridUnits()) {
        auto it = start.find(u->id);
        if (it != start.end() && (it->second.first != u->x || it->second.second != u->y)) ++r.enemyMoved;
    }
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&](std::uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    for (int l = 0; l < World::MaxLayers; ++l)
        for (const Sprite* s = w.layerHead(l); s; s = s->next) {
            mix(s->nvid); mix(static_cast<std::uint16_t>(s->x)); mix(static_cast<std::uint16_t>(s->y));
            mix(static_cast<std::uint16_t>(s->z)); mix(s->action); mix(static_cast<std::uint16_t>(s->frame)); mix(s->hp);
        }
    mix(w.rng().state);
    r.hash = h;
    r.sprites = w.liveCount();
    std::ranges::sort(sim);
    r.simP99 = sim[sim.size() * 99 / 100];
    r.simMax = sim.back();
    r.drawAvg = draws ? drawSum / draws : 0;
    return r;
}

void checkTablesAgainstExe(const std::filesystem::path& data) {
    std::ifstream f(data / "gromada.exe", std::ios::binary);
    if (!f) return;
    const std::string exe{std::istreambuf_iterator<char>(f), {}};
    auto i32 = [&](std::size_t off) { std::int32_t v; std::memcpy(&v, exe.data() + off, 4); return v; };
    // .data is mapped 1:1 (raw 0x28000 == va 0x428000)
    int bad = 0;
    for (int i = 0; i < 256; ++i) {
        if (i32(0x28e14 + 4 * i) != fx::Sin[i]) ++bad;
        if (i32(0x29214 + 4 * i) != fx::NegCos[i]) ++bad;
    }
    for (int i = 0; i < 7; ++i)
        if (static_cast<unsigned char>(exe[0x29e18 + i]) != fx::CordicAngles[i]) ++bad;
    CHECK_EQ(bad, 0);
}

} // namespace

int main() {
    SKIP_WITHOUT_DATA();
    const auto data = *gromada_test::dataDir();
    checkTablesAgainstExe(data);
    GameResources res{data / "fw.res"};

    // Levels 07 and 20..29 (except 27) are tiny placeholder maps (< 200 objects) whose script
    // creates everything; the checks on enemy activity count the 19 real missions.
    int levelsWithMovement = 0, levelsWithFights = 0, levelsWithShots = 0, responded = 0, aliveAtOrder = 0, missions = 0;
    for (int level = 1; level <= 30; ++level) {
        for (double step : {1000.0 / 60.0, 1000.0 / 120.0}) {
            const auto r = runLevel(res, data, level, step, 0x1234u + static_cast<unsigned>(level), false);
            std::fprintf(stderr, "level %02d step %.1f: sprites=%zu enemies moved=%d damage=%d shots=%d flagman=%s p99=%.3fms\n",
                         level, step, r.sprites, r.enemyMoved, r.damageEvents, r.shots,
                         !r.flagmanAliveAtOrder ? "dead" : r.flagmanResponded ? "obeys" : "IGNORED", r.simP99);
            if (step > 10 && r.sprites > 1000) {
                ++missions;
                if (r.enemyMoved) ++levelsWithMovement;
                if (r.damageEvents) ++levelsWithFights;
                if (r.shots > 2) ++levelsWithShots;   // the flagman fires twice by itself
                if (r.flagmanAliveAtOrder) { ++aliveAtOrder; if (r.flagmanResponded) ++responded; }
            }
            if (r.flagmanAliveAtOrder) CHECK(r.flagmanResponded);
            CHECK(r.shots >= 1 || !r.flagmanAliveAtOrder);
        }
    }
    std::fprintf(stderr, "missions %d: with moving enemies %d, with damage %d, with shots %d, flagman obeyed %d/%d\n", missions,
                 levelsWithMovement, levelsWithFights, levelsWithShots, responded, aliveAtOrder);
    CHECK(missions >= 18);
    CHECK(levelsWithMovement * 4 >= missions * 3);
    CHECK(levelsWithFights * 4 >= missions * 3);
    CHECK(levelsWithShots * 3 >= missions * 2);
    CHECK(aliveAtOrder >= 15);

    // determinism: same seed and steps -> same world
    const auto a = runLevel(res, data, 19, 1000.0 / 60.0, 77, false);
    const auto b = runLevel(res, data, 19, 1000.0 / 60.0, 77, false);
    CHECK_EQ(a.hash, b.hash);
    const auto c = runLevel(res, data, 19, 1000.0 / 60.0, 78, false);
    CHECK(a.hash != c.hash);

    // performance: level 19 (the largest map) within 4 ms per frame on desktop
    const auto p = runLevel(res, data, 19, 1000.0 / 60.0, 5, true);
    std::fprintf(stderr, "level 19 perf: sim p99=%.3fms max=%.3fms, draw avg=%.3fms max=%.3fms\n", p.simP99, p.simMax,
                 p.drawAvg, p.drawMax);
    CHECK(p.simP99 + p.drawAvg < 4.0);
    return gromada_test::finish();
}
