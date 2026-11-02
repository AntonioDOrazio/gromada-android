// The simulation engine (src/game, issue #8) checked DIRECTLY against the vectors produced by
// running the original gromada.exe in an emulator (tests/data/oracle, tools/oracle, issue #12).
// Unlike tests/oracle_test.cpp (pure reference functions), every family here runs the engine's
// own code: World methods on synthetic vids and sprites, GameEngine builtins, the script
// interpreter. Collaborators that the oracle replaced with recorders are intercepted through
// World::actionHook / changeActionHook or passed in (World::physics). Intentional divergences
// (docs/engine.md §5) are counted and reported, never silently ignored. No game data needed.
#include "game/engine.hpp"
#include "script/recording_host.hpp"
#include "script/script.hpp"
#include "oracle_vectors.hpp"

#include <functional>
#include <memory>
#include <set>

using namespace gromada;
using namespace gromada::game;
using namespace oracle_vectors;

namespace {

// ---- synthetic worlds ---------------------------------------------------------------------
std::vector<Vid> blankVids(int n) {
    std::vector<Vid> v(static_cast<std::size_t>(n));
    for (auto& x : v) {
        x.directionsCount = 1;
        x.type = static_cast<ObjectClass>(cls::Static);
    }
    return v;
}

void setSizes(VidInfo& v, int sx, int sy, int sz) {
    v.sizeX = static_cast<std::int16_t>(sx);
    v.sizeY = static_cast<std::int16_t>(sy);
    v.sizeZ = static_cast<std::int16_t>(sz);
    v.halfX = static_cast<std::int16_t>(v.sizeX >> 1);   // as the loader (vid_derived family)
    v.halfY = static_cast<std::int16_t>(v.sizeY >> 1);
    v.absSizeZ = static_cast<std::int16_t>(std::abs(static_cast<int>(v.sizeZ)));
}
void setDirs(VidInfo& v, int dirs, int dirStep = -1) {
    v.dirs = static_cast<std::uint8_t>(dirs);
    v.dirStep = static_cast<std::uint8_t>(dirStep >= 0 ? dirStep : 256 / (dirs ? dirs : 1));
}

// Creates a sprite of `nvid` through CreateUnit without its side effects (no Randomized offset,
// no birth animation or children, no linked object); the vid keeps its fields. `grid`: filed in
// the grid cell of (x, y).
Sprite* spawn(World& w, int nvid, int x, int y, int z, bool grid) {
    VidInfo& v = w.vid(nvid);
    const VidInfo saved = v;
    v.flags = grid ? vf::PresentOnGrid : 0;
    v.animLen = {};
    v.linkedObjectVid = 0;
    v.childNvid = {};
    v.maxUnit = -1;
    Sprite* s = w.createUnit(nvid, x, y, z, 0, 0, nullptr);
    const auto count = v.count;
    v = saved;
    v.count = count;
    if (!s) { std::fprintf(stderr, "spawn(%d, %d, %d) failed\n", nvid, x, y); std::exit(2); }
    return s;
}

// Records virtual Action calls (intercepted when `filter` says so) and ChangeAction calls.
struct Recorder {
    struct Call { int act, v1, v2; Sprite* s; };
    std::vector<Call> actions, changes;
    void attach(World& w, std::function<bool(int act)> intercept = [](int) { return true; }, bool changeActions = true) {
        actions.clear();
        changes.clear();
        w.actionHook = [this, intercept](Sprite& s, int a, int v1, int v2, int& ret) {
            if (!intercept(a)) return false;
            actions.push_back({a, v1, v2, &s});
            ret = 0;
            return true;
        };
        if (changeActions)
            w.changeActionHook = [this](Sprite& s, int a, int dir) { changes.push_back({a, dir, 0, &s}); return true; };
        else
            w.changeActionHook = nullptr;
    }
};

// ---- families: math, RNG, time ------------------------------------------------------------
void testRandAndTables() {
    forCases("rand", [](Tally& t, int i, Row r, const Family&) {
        MsvcRand rng;
        rng.seed(std::uint32_t(r("seed")));
        for (int k = 0; k < 48; ++k) EXPECT(t, i, "rand", r.at(1 + std::size_t(k)), rng.next());
        EXPECT(t, i, "state", r("state"), rng.state);
    });

    Family f = load("tables");
    if (f.loaded) {
        Tally t{"tables"};
        const Json& a = *f.root.find("arrays");
        fx::PerspectiveTables startup;
        startup.build(24, 16);   // World's constructor: the tables before the first map
        for (int i = 0; i < 256; ++i) {
            EXPECT(t, i, "sin", a.find("sin")->arr[std::size_t(i)].num, fx::Sin[std::size_t(i)]);
            EXPECT(t, i, "negCos", a.find("negCos")->arr[std::size_t(i)].num, fx::NegCos[std::size_t(i)]);
            EXPECT(t, i, "startupSinScaled", a.find("startupSinScaled")->arr[std::size_t(i)].num, startup.sinS[std::size_t(i)]);
            EXPECT(t, i, "startupNegCosScaled", a.find("startupNegCosScaled")->arr[std::size_t(i)].num, startup.negCosS[std::size_t(i)]);
            // startupCot (0x4316ac) is never read by the simulation: the engine has no such table.
            ++t.cases;
        }
        for (int i = 0; i < 7; ++i) EXPECT(t, i, "cordic", a.find("cordicAngles")->arr[std::size_t(i)].num, fx::CordicAngles[std::size_t(i)]);
    }
    forCases("rotation_tables", [](Tally& t, int i, Row r, const Family&) {
        const int dirs = int(r("dirs"));
        for (int d = 0; d < 256; ++d) {
            EXPECT(t, i, "left", r.at(1 + std::size_t(d)), fx::rotateStep(dirs, std::uint8_t(d), false));
            EXPECT(t, i, "right", r.at(257 + std::size_t(d)), fx::rotateStep(dirs, std::uint8_t(d), true));
        }
    });
    forCases("map_scale_tables", [](Tally& t, int i, Row r, const Family&) {
        fx::PerspectiveTables p;
        p.build(int(r("scaleX")), int(r("scaleY")));
        const auto k = std::size_t(r("i"));
        EXPECT(t, i, "sinScaled", r("sinScaled"), p.sinS[k]);
        EXPECT(t, i, "negCosScaled", r("negCosScaled"), p.negCosS[k]);
    });
    forCases("vid_derived", [](Tally& t, int i, Row r, const Family&) {
        std::vector<Vid> vids(1);
        vids[0].directionsCount = std::uint8_t(r("dirs"));
        vids[0].sizeX = std::uint16_t(r("sizeX"));
        vids[0].sizeY = std::uint16_t(r("sizeY"));
        vids[0].sizeZ = std::uint16_t(r("sizeZ"));
        vids[0].rotationPeriod = std::uint8_t(r("rotationPeriod"));
        World w{vids, {}};
        const VidInfo& v = w.vid(0);
        if (r("crash")) {   // dirs == 0 divides by zero in the original (D10: the engine uses 1)
            ++t.skipped;
            EXPECT(t, i, "dirs0", 1, v.dirs);
            return;
        }
        EXPECT(t, i, "dirStep", r("dirStep"), v.dirStep);
        EXPECT(t, i, "halfX", r("halfX"), v.halfX);
        EXPECT(t, i, "halfY", r("halfY"), v.halfY);
        EXPECT(t, i, "absSizeZ", r("absSizeZ"), v.absSizeZ);
        EXPECT(t, i, "rotationPeriod", r("rotationPeriodOut"), v.rotationPeriod);
        EXPECT(t, i, "maxUnit", r("maxUnit"), v.maxUnit);
    });
}

void testDirectionDistance() {
    std::vector<Vid> vids = blankVids(4);
    World w{vids, {}};
    w.loadEmpty(256 * 24, 256 * 16, 1);
    forCases("direction", [&](Tally& t, int i, Row r, const Family&) {
        const auto d = w.getDirection(int(r("x1")), int(r("y1")), int(r("x2")), int(r("y2")));
        EXPECT(t, i, "direction", r("direction"), d.raw);
        EXPECT(t, i, "distance", r("distance"), d.distance);
        EXPECT(t, i, "g_dist", r("distance"), w.lastDistance());
    });
    Sprite a, b;
    forCases("distance_sprite", [&](Tally& t, int i, Row r, const Family&) {
        a.x = std::int16_t(r("x")); a.y = std::int16_t(r("y"));
        b.x = std::int16_t(r("otherX")); b.y = std::int16_t(r("otherY"));
        EXPECT(t, i, "distance", r("result"), World::distance(a, b));
    });
    forCases("distance_point", [&](Tally& t, int i, Row r, const Family&) {
        a.x = std::int16_t(r("x")); a.y = std::int16_t(r("y"));
        EXPECT(t, i, "distance", r("result"), World::distanceTo(a, int(r("px")), int(r("py"))));
    });
    // Builtins through GameEngine::callBuiltin.
    GameEngine eng{vids, {}};
    eng.loadEmptyMap(100, 100, 1);
    Sprite* u = spawn(eng.world(), 1, 10, 10, 0, false);
    forCases("sizeto", [&](Tally& t, int i, Row r, const Family&) {
        u->x = std::int16_t(r("unitX"));
        u->y = std::int16_t(r("unitY"));
        const std::array<script::Value, 3> args{script::Value{r("nullUnit") ? 0 : u->id}, script::Value{std::int32_t(r("x"))},
                                                script::Value{std::int32_t(r("y"))}};
        const auto v = eng.callBuiltin(65, args);
        EXPECT(t, i, "SizeTo", r("result"), v ? v->toInt() : -999999);
    });
    forCases("random_builtin", [&](Tally& t, int i, Row r, const Family&) {
        eng.world().rng().seed(std::uint32_t(r("seed")));
        const std::array<script::Value, 1> args{script::Value{std::int32_t(r("max"))}};
        const auto v = eng.callBuiltin(90, args);
        EXPECT(t, i, "state", r("state"), eng.world().rng().state);
        if (r("crash")) {   // Random(-1): #DE in the original, 0 in the engine (D10)
            ++t.skipped;
            EXPECT(t, i, "crash value", 0, v ? v->toInt() : -1);
            return;
        }
        EXPECT(t, i, "result", r("result"), v ? v->toInt() : -999999);
    });
}

void testTime() {
    std::vector<Vid> vids = blankVids(4);
    World w{vids, {}};
    forCases("clock", [&](Tally& t, int i, Row r, const Family&) {
        w.setClock(std::uint32_t(r("curclock")));
        const std::uint32_t dt = std::uint32_t(r("timeGetTime")) - std::uint32_t(r("lastReal"));   // SVGA::BeginFrame
        if (dt > 0x7fffffffu) {   // D13: the engine's elapsed time is a signed frame time; < 0 = invalid, no advance
            ++t.skipped;
            w.advanceClock(static_cast<int>(dt));
            EXPECT(t, i, "negative elapsed", r("curclock"), w.clock());
            return;
        }
        w.advanceClock(static_cast<int>(dt));
        EXPECT(t, i, "curclock", r("curclock'"), w.clock());
    });
    forCases("tick_schedule", [&](Tally& t, int i, Row r, const Family&) {
        w.loadEmpty(1000, 1000, 1);
        VidInfo& v = w.vid(1);
        v.category = 0;   // terrain: SpriteTact has no physics, consumes no rand()
        Sprite* s = spawn(w, 1, 10, 10, 0, false);
        v.frameDuration = std::uint16_t(r("frameDuration"));
        v.flags = std::uint16_t(r("flags"));
        s->frame = 0;
        s->lastFrame = 1000;
        s->lastTact = std::uint32_t(r("lastTact"));
        w.setClock(std::uint32_t(r("curclock")));
        w.rng().seed(std::uint32_t(r("seed")));
        w.tact();
        const int ticked = s->frame == 1 ? 1 : (v.flags & vf::SkipUpdate) ? 2 : 0;
        EXPECT(t, i, "ticked", r("ticked"), ticked);
        EXPECT(t, i, "seed", r("seed'"), w.rng().state);
    });
    forCases("sprite_timer", [&](Tally& t, int i, Row r, const Family&) {
        w.loadEmpty(1000, 1000, 1);
        w.vid(1).category = 0;
        Sprite* s = spawn(w, 1, 10, 10, 0, false);
        s->timer = std::uint16_t(r("timer"));
        s->lastTact = std::uint32_t(r("lastTact"));
        s->frame = std::int16_t(r("frame"));
        s->firstFrame = -32768;
        s->lastFrame = 32767;
        w.setClock(std::uint32_t(r("curclock")));
        w.spriteTact(*s);
        EXPECT(t, i, "timer", r("timer'"), s->timer);
        EXPECT(t, i, "frame", r("frame'"), s->frame);
    });
}

// ---- script VM through the interpreter (src/script) ------------------------------------------
void testVm() {
    std::string src = "int a, b, r;\n";
    const std::pair<int, const char*> ops[] = {
        {3, "a+b"}, {4, "a-b"}, {5, "a*b"}, {6, "a/b"}, {7, "a%b"}, {8, "-a"}, {9, "~a"}, {10, "a^b"}, {11, "a|b"},
        {12, "a&b"}, {13, "a==b"}, {14, "a||b"}, {15, "a>b"}, {16, "a<b"}, {17, "a>=b"}, {18, "a<=b"}, {19, "!a"},
        {20, "a!=b"}, {21, "a&&b"}, {37, "a>>b"}, {38, "a<<b"}};
    for (auto& [op, e] : ops) src += "f" + std::to_string(op) + "(){ r=" + e + "; }\n";
    src += "main(){}\n";
    script::RecordingHost host;
    script::Interpreter vm{script::Script::compileText(src), host};
    forCases("vm_arith", [&](Tally& t, int i, Row r, const Family&) {
        vm.set("a", script::Value{std::int32_t(r("a"))});
        vm.set("b", script::Value{std::int32_t(r("b"))});
        vm.set("r", script::Value{0x5a5a5a5a});
        bool crash = false;
        try {
            vm.call("f" + std::to_string(r("op")));
        } catch (const script::ScriptError&) {
            crash = true;
        }
        EXPECT(t, i, "crash", r("crash"), crash);
        if (!crash) EXPECT(t, i, "result", r("result"), vm.get("r").toInt());
    });
}

// ---- animation and movement ------------------------------------------------------------------
void setAnim(VidInfo& v, const Row& r, int col, int dirs) {
    for (int k = 0; k < 16; ++k) v.animLen[std::size_t(k)] = std::uint8_t(r.at(std::size_t(col + k)));
    setDirs(v, dirs);
}

void testAnimation() {
    std::vector<Vid> vids = blankVids(4);
    World w{vids, {}};
    w.loadEmpty(1000, 1000, 1);
    Sprite* s = spawn(w, 1, 10, 10, 0, false);
    Sprite* c = spawn(w, 2, 10, 10, 0, false);
    forCases("change_action", [&](Tally& t, int i, Row r, const Family& f) {
        setAnim(w.vid(1), r, f.col("len0"), int(r("dirs")));
        setAnim(w.vid(2), r, f.col("childLen0"), int(r("childDirs")));
        const bool hasChild = r("hasChild") != 0;
        s->child = hasChild ? c : nullptr;
        auto init = [](Sprite& x, std::int64_t a, std::int64_t d, std::int64_t st) {
            x.action = std::uint8_t(a); x.direction = std::uint8_t(d); x.state = std::uint16_t(st);
            x.frame = 0x1111; x.firstFrame = 0x2222; x.lastFrame = 0x3333;
        };
        init(*s, r("action"), r("direction"), r("state"));
        init(*c, r("childAction"), r("childDirection"), r("childState"));
        w.changeAction(*s, int(r("act")), int(r("dir")));
        EXPECT(t, i, "action", r("action'"), s->action);
        EXPECT(t, i, "direction", r("direction'"), s->direction);
        EXPECT(t, i, "frame", r("frame'"), s->frame);
        EXPECT(t, i, "firstFrame", r("firstFrame'"), s->firstFrame);
        EXPECT(t, i, "lastFrame", r("lastFrame'"), s->lastFrame);
        EXPECT(t, i, "state", r("state'"), s->state);
        if (hasChild) {
            EXPECT(t, i, "child.action", r("child.action'"), c->action);
            EXPECT(t, i, "child.direction", r("child.direction'"), c->direction);
            EXPECT(t, i, "child.frame", r("child.frame'"), c->frame);
            EXPECT(t, i, "child.firstFrame", r("child.firstFrame'"), c->firstFrame);
            EXPECT(t, i, "child.lastFrame", r("child.lastFrame'"), c->lastFrame);
            EXPECT(t, i, "child.state", r("child.state'"), c->state);
        }
    });
    s->child = nullptr;

    Recorder rec;
    rec.attach(w);
    forCases("turn_to", [&](Tally& t, int i, Row r, const Family&) {
        VidInfo& v = w.vid(1);
        setDirs(v, int(r("dirs")), int(r("dirStep")));
        v.flags = std::uint16_t(r("flags"));
        s->direction = std::uint8_t(r("direction"));
        s->action = std::uint8_t(r("action"));
        rec.changes.clear();
        const int ret = w.turnTo(*s, int(r("target")));
        EXPECT(t, i, "ret", r("ret"), ret);
        EXPECT(t, i, "direction", r("direction'"), s->direction);
        EXPECT(t, i, "nChanges", r("caAct") >= 0 ? 1 : 0, std::int64_t(rec.changes.size()));
        if (!rec.changes.empty()) {
            EXPECT(t, i, "caAct", r("caAct"), rec.changes[0].act);
            EXPECT(t, i, "caDir", r("caDir"), rec.changes[0].v1);
        }
    });

    forCases("set_destination", [&](Tally& t, int i, Row r, const Family&) {
        VidInfo& v = w.vid(1);
        v.speedX = std::int16_t(r("speedX"));
        v.speedY = std::int16_t(r("speedY"));
        setDirs(v, int(r("dirs")));
        v.flags = std::uint16_t(r("flags"));
        v.category = std::uint8_t(r("category"));
        v.cls = std::uint8_t(r("cls"));
        s->x = std::int16_t(r("x")); s->y = std::int16_t(r("y")); s->z = std::int16_t(r("z"));
        s->direction = std::uint8_t(r("direction"));
        s->state = std::uint16_t(r("state"));
        s->speed = std::int16_t(r("speed"));
        s->fracX = s->fracY = s->fracZ = 0x55;
        s->vz = 0x1234;
        s->goX = 0x2222; s->goY = 0x3333; s->goZ = 0x4444;
        w.getDirection(0, 0, 777, 0);   // a stale g_dist
        rec.changes.clear();
        w.setDestination(*s, int(r("argX")), int(r("argY")), int(r("argZ")));
        EXPECT(t, i, "state", r("state'"), s->state);
        EXPECT(t, i, "direction", r("direction'"), s->direction);
        EXPECT(t, i, "goX", r("goX"), s->goX);
        EXPECT(t, i, "goY", r("goY"), s->goY);
        EXPECT(t, i, "goZ", r("goZ"), s->goZ);
        EXPECT(t, i, "vz", r("vz"), s->vz);
        EXPECT(t, i, "fracsZeroed", r("fracsZeroed"), s->fracX == 0 && s->fracY == 0 && s->fracZ == 0);
        EXPECT(t, i, "gDist", r("gDist"), w.lastDistance());   // 0 when speedX == 0, like the original
        EXPECT(t, i, "nChanges", r("caAct") >= 0 ? 1 : 0, std::int64_t(rec.changes.size()));
        if (!rec.changes.empty()) {
            EXPECT(t, i, "caAct", r("caAct"), rec.changes[0].act);
            EXPECT(t, i, "caDir", r("caDir"), rec.changes[0].v1);
        }
    });
    w.actionHook = nullptr;
    w.changeActionHook = nullptr;
}

// Physics step: grid of gen_physics.py (cells 32x16 px, 100x120 cells, row shift 7).
void testPhysics() {
    std::vector<Vid> vids = blankVids(4);
    World w{vids, {}};
    VidInfo& gridVid = w.vid(3);   // only sets the grid cell size
    gridVid.flags = vf::PresentOnGrid;
    setSizes(gridVid, 64, 32, 0);
    Recorder rec;
    forCases("physics", [&](Tally& t, int i, Row r, const Family&) {
        w.loadEmpty(3200, 1920, 1);
        if (w.gridShiftX() != 5 || w.gridShiftY() != 4) { t.check(false, i, "grid", 5, w.gridShiftX()); return; }
        VidInfo& v = w.vid(1);
        v.category = cat::Vehicle;
        setDirs(v, 32);
        const auto flags = std::uint16_t(r("flags"));
        v.flags = flags;
        v.speedX = std::int16_t(r("speedX"));
        v.acceleration = std::uint16_t(r("acceleration"));
        v.rotationPeriod = std::int8_t(r("rotationPeriod"));
        Sprite* s = spawn(w, 1, 0, 0, 0, (flags & vf::PresentOnGrid) != 0);
        if (flags & vf::PresentOnGrid) w.gridMoveTo(*s, int(r("x")), int(r("y")));
        s->x = std::int16_t(r("x")); s->y = std::int16_t(r("y")); s->z = std::int16_t(r("z"));
        s->direction = std::uint8_t(r("direction"));
        s->state = std::uint16_t(r("state"));
        s->lastTact = std::uint32_t(r("lastTact"));
        s->speed = std::int16_t(r("speed")); s->vz = std::int16_t(r("vz"));
        s->fracX = std::int8_t(r("fracX")); s->fracY = std::int8_t(r("fracY")); s->fracZ = std::int8_t(r("fracZ"));
        s->stepAccum = std::uint8_t(r("stepAccum"));
        s->goX = std::int16_t(r("goX")); s->goY = std::int16_t(r("goY")); s->goZ = std::int16_t(r("goZ"));
        w.setClock(std::uint32_t(r("curclock")));
        w.setMapSize(int(r("mapW")), int(r("mapH")));
        w.setWind(int(r("windStrength")), int(r("windDir")));
        rec.attach(w);
        auto cellOf = [&](int x, int y) { return std::pair{x >> 5, y >> 4}; };
        auto inCell = [&](std::pair<int, int> c) {
            const auto* cell = w.gridCell(c.first, c.second);
            return cell && std::find(cell->begin(), cell->end(), s) != cell->end();
        };
        const auto oldCell = cellOf(s->x, s->y);
        const bool wasInOld = inCell(oldCell);
        int checkMoves = 0, obstacles = 0;
        int cm[3] = {0, 0, 0};
        int step = 0;
        bool ran = false;
        if (!(s->state & st::Passive)) {   // SpriteTact skips the physics of passive sprites (0x409cae)
            ran = true;
            step = w.physics(
                *s,
                [&](Sprite&, int x, int y, int z) { ++checkMoves; cm[0] = x; cm[1] = y; cm[2] = z; return r("blocked") != 0; },
                [&](Sprite&, int, int, int) { ++obstacles; return std::uint32_t(r("obstacle")); });
        }
        w.actionHook = nullptr;
        w.changeActionHook = nullptr;
        EXPECT(t, i, "x", r("x'"), s->x);
        EXPECT(t, i, "y", r("y'"), s->y);
        EXPECT(t, i, "z", r("z'"), s->z);
        EXPECT(t, i, "state", r("state'"), s->state);
        EXPECT(t, i, "speed", r("speed'"), s->speed);
        EXPECT(t, i, "vz", r("vz'"), s->vz);
        EXPECT(t, i, "fracX", r("fracX'"), s->fracX);
        EXPECT(t, i, "fracY", r("fracY'"), s->fracY);
        EXPECT(t, i, "fracZ", r("fracZ'"), s->fracZ);
        EXPECT(t, i, "stepAccum", r("stepAccum'"), s->stepAccum);
        if (r("stepSet")) EXPECT(t, i, "step", r("step"), step);
        else if (ran)   // speed 9999: the original leaves the local unwritten, but the arrival bits it
                        // sets make act_nextcommand independent of it (docs/engine.md §7)
            EXPECT(t, i, "beam arrival", 3, s->state & 3);
        EXPECT(t, i, "nActions", r("nActions"), std::int64_t(rec.actions.size()));
        for (std::size_t k = 0; k < rec.actions.size() && k < 2; ++k) {
            const std::string p = k ? "act2" : "act1";
            EXPECT(t, i, "act", r(p), rec.actions[k].act);
            EXPECT(t, i, "act.v1", r(p + "v1"), rec.actions[k].v1);
            EXPECT(t, i, "act.v2", r(p + "v2"), rec.actions[k].v2);
        }
        EXPECT(t, i, "changeActions", 0, std::int64_t(rec.changes.size()));
        EXPECT(t, i, "checkMoveCalls", r("checkMoveCalls"), checkMoves);
        if (checkMoves) {
            EXPECT(t, i, "cmX", r("cmX"), cm[0]);
            EXPECT(t, i, "cmY", r("cmY"), cm[1]);
            EXPECT(t, i, "cmZ", r("cmZ"), cm[2]);
        }
        EXPECT(t, i, "obstacleCalls", r("obstacleCalls"), obstacles);
        const auto newCell = cellOf(s->x, s->y);
        const int idxOld = (oldCell.second << 7) + oldCell.first, idxNew = (newCell.second << 7) + newCell.first;
        const int removed = wasInOld && !inCell(oldCell) ? idxOld : -1;
        const int added = newCell != oldCell && inCell(newCell) ? idxNew : -1;
        EXPECT(t, i, "cellRemoved", r("cellRemoved"), removed);
        EXPECT(t, i, "cellAdded", r("cellAdded"), added);
    });
}

void testCommands() {
    std::vector<Vid> vids = blankVids(4);
    World w{vids, {}};
    w.loadEmpty(1000, 1000, 1);
    Sprite* s = spawn(w, 1, 10, 10, 0, false);
    Recorder rec;
    rec.attach(w);
    forCases("commands", [&](Tally& t, int i, Row r, const Family&) {
        const int n = int(r("nCommands"));
        w.vid(1).rotationPeriod = std::int8_t(r("rotationPeriod"));
        s->frame = std::int16_t(r("frame"));
        s->lastFrame = std::int16_t(r("lastFrame"));
        s->action = std::uint8_t(r("action"));
        s->state = std::uint16_t(r("state"));
        s->commands.reset();
        if (n) {
            s->commands = std::make_unique<CommandStack>();
            for (int k = 0; k < n; ++k)
                s->commands->e.push_back({std::uint8_t(r.obj(k, "act")), std::int32_t(r.obj(k, "var1")), std::int32_t(r.obj(k, "var2"))});
            s->commands->count = int(r("count"));
        }
        rec.actions.clear();
        rec.changes.clear();
        if (World::commandsDue(*s)) w.nextCommand(*s, int(r("step")));
        int kind = 0, act = -1, v1 = 0, v2 = 0;
        if (!rec.actions.empty()) { kind = 1; act = rec.actions[0].act; v1 = rec.actions[0].v1; v2 = rec.actions[0].v2; }
        else if (!rec.changes.empty()) {
            kind = 2; act = rec.changes[0].act & 0xff;
            EXPECT(t, i, "caDir", -1, rec.changes[0].v1);
        }
        EXPECT(t, i, "calls", kind ? 1 : 0, std::int64_t(rec.actions.size() + rec.changes.size()));
        EXPECT(t, i, "kind", r("kind"), kind);
        EXPECT(t, i, "act", r("act"), act);
        EXPECT(t, i, "v1", r("v1"), v1);
        EXPECT(t, i, "v2", r("v2"), v2);
        EXPECT(t, i, "count", r("count'"), s->commands ? s->commands->count : -1);
    });
    w.actionHook = nullptr;
    w.changeActionHook = nullptr;
}

// ---- collisions and damage (grids of gen_checkmove.py / gen_damage.py: 32 px cells, 64x64) -----
struct GridWorld {
    std::vector<Vid> vids = blankVids(16);
    World w{vids, {}};
    GridWorld() {
        VidInfo& g = w.vid(15);
        g.flags = vf::PresentOnGrid;
        setSizes(g, 64, 64, 0);
    }
    void reset() {
        w.loadEmpty(2048, 2048, 1);
        if (w.gridShiftX() != 5 || w.gridShiftY() != 5) { std::fprintf(stderr, "unexpected grid\n"); std::exit(2); }
    }
    // A sprite filed in cell (cx, cy) whatever its position (the generators can misfile).
    Sprite* place(int nvid, int x, int y, int z, int cx, int cy) {
        Sprite* s = spawn(w, nvid, cx << 5, cy << 5, z, true);
        s->x = std::int16_t(x); s->y = std::int16_t(y); s->z = std::int16_t(z);
        return s;
    }
};

void testCheckMove() {
    GridWorld g;
    Recorder rec;
    forCases("check_move", [&](Tally& t, int i, Row r, const Family&) {
        g.reset();
        World& w = g.w;
        VidInfo& sv = w.vid(1);
        sv.category = std::uint8_t(r("category"));
        sv.collisionMask = std::uint8_t(r("collisionMask"));
        setSizes(sv, int(r("sizeX")), int(r("sizeY")), int(r("sizeZ")));
        const int sx = int(r("selfX")), sy = int(r("selfY")), az = int(r("argZ"));
        Sprite* self = r("selfInGrid") ? g.place(1, sx, sy, az, sx >> 5, sy >> 5) : spawn(w, 1, sx, sy, az, false);
        sv.flags = std::uint16_t(r("flags"));
        const int n = int(r("nObjects"));
        std::vector<Sprite*> objs;
        for (int k = 0; k < n; ++k) {
            VidInfo& ov = w.vid(2 + k);
            ov.category = std::uint8_t(r.obj(k, "category"));
            ov.collisionMask = std::uint8_t(r.obj(k, "collisionMask"));
            setSizes(ov, int(r.obj(k, "sizeX")), int(r.obj(k, "sizeY")), int(r.obj(k, "sizeZ")));
            Sprite* o = g.place(2 + k, int(r.obj(k, "x")), int(r.obj(k, "y")), int(r.obj(k, "z")), int(r.obj(k, "cellX")),
                                int(r.obj(k, "cellY")));
            ov.flags = std::uint16_t(r.obj(k, "flags"));
            o->action = std::uint8_t(r.obj(k, "action"));
            objs.push_back(o);
        }
        rec.attach(w);
        Sprite* b = w.checkMove(*self, int(r("argX")), int(r("argY")), az);
        w.actionHook = nullptr;
        w.changeActionHook = nullptr;
        int blocker = -1;
        if (b == self) blocker = -2;
        else if (b) blocker = int(std::find(objs.begin(), objs.end(), b) - objs.begin());
        std::uint32_t crushed = 0;
        for (auto& c : rec.actions) {
            EXPECT(t, i, "crush act", 43, c.act);
            EXPECT(t, i, "crush v1", 2, c.v1);
            EXPECT(t, i, "crush v2", 0, c.v2);
            crushed |= 1u << (std::find(objs.begin(), objs.end(), c.s) - objs.begin());
        }
        EXPECT(t, i, "blocker", r("blocker"), blocker);
        EXPECT(t, i, "crushedMask", r("crushedMask"), crushed);
    });
}

void testDamage() {
    std::vector<Vid> vids = blankVids(80);
    World w{vids, {}};
    w.loadEmpty(1000, 1000, 1);
    Recorder rec;
    forCases("damage", [&](Tally& t, int i, Row r, const Family&) {
        VidInfo& v = w.vid(1);
        v.maxHP = std::uint8_t(r("maxHP"));
        v.childNvid = {};
        v.childNvid[act::wound] = 77;
        Sprite* s = spawn(w, 1, 10, 10, 0, false);
        Sprite* wound = spawn(w, 77, 10, 10, 0, false);   // the wound effect in the sub-object chain
        s->child = wound;
        s->hp = std::uint8_t(r("hp"));
        rec.attach(w, [](int a) { return a != act::damage; });   // destroy of the removed child: recorded
        w.action(*s, act::damage, int(r("damage")), 0);
        w.actionHook = nullptr;
        w.changeActionHook = nullptr;
        EXPECT(t, i, "hp", r("hp'"), s->hp);
        EXPECT(t, i, "nChanges", r("caAct") >= 0 ? 1 : 0, std::int64_t(rec.changes.size()));
        if (!rec.changes.empty()) {
            EXPECT(t, i, "caAct", r("caAct"), rec.changes[0].act);
            EXPECT(t, i, "caDir", -1, rec.changes[0].v1);
        }
        EXPECT(t, i, "woundChildRemoved", r("woundChildRemoved"), s->child == nullptr);
        s->child = nullptr;
        w.action(*s, act::destroy, 0, 0);
        if (wound->alive) w.action(*wound, act::destroy, 0, 0);
        w.tact();   // recycles the freed sprites
    });

    GridWorld g;
    forCases("area_damage", [&](Tally& t, int i, Row r, const Family&) {
        g.reset();
        World& gw = g.w;
        VidInfo& ev = gw.vid(1);
        ev.deathDamage = std::uint8_t(r("deathDamage"));
        ev.deathDamageRadius = std::int16_t(std::uint16_t(r("deathDamageRadius")));
        ev.maxHP = std::uint8_t(r("maxHP"));
        setSizes(ev, int(r("sizeX")), int(r("sizeY")), 0);
        const int x = int(r("x")), y = int(r("y"));
        Sprite* me = g.place(1, x, y, int(r("z")), x >> 5, y >> 5);
        me->hp = 9;
        const int n = int(r("nObjects"));
        std::vector<Sprite*> objs;
        for (int k = 0; k < n; ++k) {
            VidInfo& ov = gw.vid(2 + k);
            ov.maxHP = std::uint8_t(r.obj(k, "maxHP"));
            setSizes(ov, int(r.obj(k, "sizeX")), int(r.obj(k, "sizeY")), int(r.obj(k, "sizeZ")));
            objs.push_back(g.place(2 + k, int(r.obj(k, "x")), int(r.obj(k, "y")), int(r.obj(k, "z")), int(r.obj(k, "cellX")),
                                   int(r.obj(k, "cellY"))));
        }
        rec.attach(gw, [](int a) { return a == act::damage; });
        gw.action(*me, act::death, 0, 0);
        gw.actionHook = nullptr;
        gw.changeActionHook = nullptr;
        std::uint32_t hit = 0;
        bool twice = false;
        for (auto& c : rec.actions) {
            const auto k = std::find(objs.begin(), objs.end(), c.s) - objs.begin();
            twice |= (hit >> k) & 1;
            hit |= 1u << k;
            EXPECT(t, i, "damage", r("deathDamage"), c.v1);
            EXPECT(t, i, "source", me->id, c.v2);
        }
        EXPECT(t, i, "hp", r("hp'"), me->hp);
        EXPECT(t, i, "hitMask", r("hitMask"), hit);
        EXPECT(t, i, "hitOnce", 0, twice);
    });
}

} // namespace

int main() {
    testRandAndTables();
    testDirectionDistance();
    testTime();
    testVm();
    testAnimation();
    testPhysics();
    testCommands();
    testCheckMove();
    testDamage();
    return gromada_test::finish();
}
