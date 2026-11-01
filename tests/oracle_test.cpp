// Differential oracle (issue #11): the pure reference functions of src/game/reference_math.hpp
// must reproduce the vectors produced by running the original gromada.exe in an emulator
// (tools/oracle/generate.py, docs/oracle.md). The vectors are committed: no game data needed.
#include "game/reference_math.hpp"
#include "test.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace ref = gromada::game::ref;

namespace {

// ---- minimal JSON reader (objects, arrays, integers, strings) ---------------------------------
struct Json {
    enum Kind { Null, Num, Str, Arr, Obj } kind = Null;
    std::int64_t num = 0;
    std::string str;
    std::vector<Json> arr;
    std::map<std::string, Json> obj;
    const Json* find(const std::string& k) const {
        auto it = obj.find(k);
        return it == obj.end() ? nullptr : &it->second;
    }
};

struct Parser {
    std::string_view s;
    std::size_t p = 0;
    bool ok = true;
    void ws() { while (p < s.size() && (s[p] == ' ' || s[p] == '\n' || s[p] == '\r' || s[p] == '\t')) ++p; }
    bool eat(char c) { ws(); if (p < s.size() && s[p] == c) { ++p; return true; } return false; }
    std::string string() {
        std::string out;
        if (!eat('"')) { ok = false; return out; }
        while (p < s.size() && s[p] != '"') {
            if (s[p] == '\\' && p + 1 < s.size()) { ++p; out += s[p] == 'n' ? '\n' : s[p]; }
            else out += s[p];
            ++p;
        }
        ++p;
        return out;
    }
    Json value() {
        Json j;
        ws();
        if (p >= s.size()) { ok = false; return j; }
        char c = s[p];
        if (c == '{') {
            ++p; j.kind = Json::Obj;
            if (eat('}')) return j;
            do { std::string k = string(); if (!eat(':')) { ok = false; return j; } j.obj[k] = value(); } while (ok && eat(','));
            if (!eat('}')) ok = false;
        } else if (c == '[') {
            ++p; j.kind = Json::Arr;
            if (eat(']')) return j;
            do { j.arr.push_back(value()); } while (ok && eat(','));
            if (!eat(']')) ok = false;
        } else if (c == '"') {
            j.kind = Json::Str; j.str = string();
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            j.kind = Json::Num;
            bool neg = c == '-';
            if (neg) ++p;
            std::int64_t v = 0;
            while (p < s.size() && s[p] >= '0' && s[p] <= '9') v = v * 10 + (s[p++] - '0');
            j.num = neg ? -v : v;
        } else if (s.substr(p, 4) == "null") {
            p += 4;
        } else {
            ok = false;
        }
        return j;
    }
};

// ---- vector families ---------------------------------------------------------------------
struct Family {
    std::string name;
    Json root;
    std::vector<std::string> columns;
    std::vector<std::string> objectColumns;
    bool loaded = false;
    int col(const std::string& c) const {
        for (std::size_t i = 0; i < columns.size(); ++i) if (columns[i] == c) return int(i);
        std::fprintf(stderr, "%s: no column %s\n", name.c_str(), c.c_str());
        std::exit(2);
    }
    int ocol(const std::string& c) const {
        for (std::size_t i = 0; i < objectColumns.size(); ++i) if (objectColumns[i] == c) return int(i);
        std::fprintf(stderr, "%s: no object column %s\n", name.c_str(), c.c_str());
        std::exit(2);
    }
    const std::vector<Json>& cases() const { return root.find("cases")->arr; }
};

Family load(const std::string& name) {
    Family f;
    f.name = name;
    std::ifstream in(std::string(GROMADA_ORACLE_DIR) + "/" + name + ".json", std::ios::binary);
    if (!in) { std::fprintf(stderr, "missing vectors %s.json\n", name.c_str()); ++gromada_test::failures; return f; }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    Parser p{text};
    f.root = p.value();
    if (!p.ok || f.root.kind != Json::Obj || !f.root.find("cases")) {
        std::fprintf(stderr, "bad JSON in %s.json\n", name.c_str()); ++gromada_test::failures; return f;
    }
    for (auto& c : f.root.find("columns")->arr) f.columns.push_back(c.str);
    if (auto* oc = f.root.find("objectColumns")) for (auto& c : oc->arr) f.objectColumns.push_back(c.str);
    f.loaded = true;
    return f;
}

// Row accessor: r("name") reads a column of the current case.
struct Row {
    const Family& f;
    const std::vector<Json>& v;
    std::int64_t operator()(const std::string& c) const { return v[f.col(c)].num; }
    std::int64_t at(std::size_t i) const { return v[i].num; }
    std::size_t size() const { return v.size(); }
};

struct Tally {
    std::string name;
    int cases = 0, bad = 0;
    void check(bool good, int index, const char* what, std::int64_t expected, std::int64_t got) {
        if (good) return;
        if (bad < 8) std::fprintf(stderr, "  %s case %d: %s expected %lld got %lld\n", name.c_str(), index, what,
                                  (long long)expected, (long long)got);
        ++bad;
    }
    ~Tally() {
        std::fprintf(stderr, "%-18s %6d cases %s\n", name.c_str(), cases, bad ? "MISMATCH" : "ok");
        if (bad) { std::fprintf(stderr, "  %d mismatching values\n", bad); ++gromada_test::failures; }
    }
};
#define EXPECT(t, idx, what, expected, got) \
    do { const std::int64_t _e = (expected), _g = (got); (t).check(_e == _g, (idx), (what), _e, _g); } while (0)

template <class Fn>
void forCases(const char* name, Fn&& fn) {
    Family f = load(name);
    if (!f.loaded) return;
    Tally t{name};
    int i = 0;
    for (auto& c : f.cases()) {
        if (c.arr.size() < f.columns.size()) { t.check(false, i, "row length", f.columns.size(), c.arr.size()); continue; }
        fn(t, i, Row{f, c.arr}, f);
        ++t.cases;
        ++i;
    }
    if (t.cases < 1) t.check(false, 0, "no cases", 1, 0);
}

// ---- families ---------------------------------------------------------------------------
void testTables() {
    Family f = load("tables");
    if (!f.loaded) return;
    Tally t{"tables"};
    const Json& a = *f.root.find("arrays");
    for (int i = 0; i < 256; ++i) {
        EXPECT(t, i, "sin", a.find("sin")->arr[i].num, ref::kSin[i]);
        EXPECT(t, i, "negCos", a.find("negCos")->arr[i].num, ref::kNegCos[i]);
        EXPECT(t, i, "startupSinScaled", a.find("startupSinScaled")->arr[i].num, ref::startupScaled(ref::kSin[i]));
        EXPECT(t, i, "startupNegCosScaled", a.find("startupNegCosScaled")->arr[i].num, ref::startupScaled(ref::kNegCos[i]));
        EXPECT(t, i, "startupCot", a.find("startupCot")->arr[i].num, ref::cotEntry(i));
        ++t.cases;
    }
    for (int i = 0; i < 7; ++i) EXPECT(t, i, "cordic", a.find("cordicAngles")->arr[i].num, ref::kCordicAngles[i]);

    forCases("rotation_tables", [](Tally& t, int i, Row r, const Family&) {
        const int dirs = int(r("dirs"));
        for (int d = 0; d < 256; ++d) {
            EXPECT(t, i, "left", r.at(1 + d), ref::rotationStep(dirs, d, false));
            EXPECT(t, i, "right", r.at(257 + d), ref::rotationStep(dirs, d, true));
        }
    });
    forCases("map_scale_tables", [](Tally& t, int i, Row r, const Family&) {
        const int k = int(r("i"));
        const auto sx = std::int32_t(r("scaleX")), sy = std::int32_t(r("scaleY"));
        EXPECT(t, i, "sinScaled", r("sinScaled"), ref::perspectiveScaled(ref::kSin[k], sx, sy));
        EXPECT(t, i, "negCosScaled", r("negCosScaled"), ref::perspectiveScaled(ref::kNegCos[k], sx, sy));
        EXPECT(t, i, "cot", r("cot"), ref::cotEntry(k));
    });
    forCases("vid_derived", [](Tally& t, int i, Row r, const Family&) {
        auto v = ref::deriveVid(std::uint8_t(r("dirs")), std::int16_t(r("sizeX")), std::int16_t(r("sizeY")),
                                std::int16_t(r("sizeZ")), std::int8_t(r("rotationPeriod")));
        EXPECT(t, i, "crash", r("crash"), !v.has_value());
        if (!v) return;
        EXPECT(t, i, "dirStep", r("dirStep"), v->dirStep);
        EXPECT(t, i, "halfX", r("halfX"), v->halfX);
        EXPECT(t, i, "halfY", r("halfY"), v->halfY);
        EXPECT(t, i, "absSizeZ", r("absSizeZ"), v->absSizeZ);
        EXPECT(t, i, "rotationPeriod", r("rotationPeriodOut"), v->rotationPeriod);
        EXPECT(t, i, "maxUnit", r("maxUnit"), -1);
    });
}

void testRand() {
    forCases("rand", [](Tally& t, int i, Row r, const Family&) {
        ref::MsvcRand rng;
        rng.seed(std::uint32_t(r("seed")));
        for (int k = 0; k < 48; ++k) EXPECT(t, i, "rand", r.at(1 + k), rng.next());
        EXPECT(t, i, "state", r("state"), rng.state);
    });
    forCases("random_builtin", [](Tally& t, int i, Row r, const Family&) {
        ref::MsvcRand rng;
        rng.seed(std::uint32_t(r("seed")));
        auto v = ref::scriptRandom(rng, std::int32_t(r("max")));
        EXPECT(t, i, "crash", r("crash"), !v.has_value());
        if (v) EXPECT(t, i, "result", r("result"), *v);
        EXPECT(t, i, "state", r("state"), rng.state);
    });
}

void testDirectionDistance() {
    forCases("direction", [](Tally& t, int i, Row r, const Family&) {
        auto d = ref::getDirection(std::int32_t(r("x1")), std::int32_t(r("y1")), std::int32_t(r("x2")), std::int32_t(r("y2")));
        EXPECT(t, i, "direction", r("direction"), d.direction);
        EXPECT(t, i, "distance", r("distance"), d.distance);
    });
    forCases("distance_sprite", [](Tally& t, int i, Row r, const Family&) {
        EXPECT(t, i, "distance", r("result"),
               ref::gameDistance(std::int16_t(r("x")), std::int16_t(r("y")), std::int16_t(r("otherX")), std::int16_t(r("otherY"))));
    });
    forCases("distance_point", [](Tally& t, int i, Row r, const Family&) {
        EXPECT(t, i, "distance", r("result"),
               ref::gameDistanceToPoint(std::int16_t(r("x")), std::int16_t(r("y")), std::int32_t(r("px")), std::int32_t(r("py"))));
    });
    forCases("sizeto", [](Tally& t, int i, Row r, const Family&) {
        EXPECT(t, i, "SizeTo", r("result"),
               ref::sizeTo(r("nullUnit") != 0, std::int16_t(r("unitX")), std::int16_t(r("unitY")), std::int32_t(r("x")),
                           std::int32_t(r("y"))));
    });
}

void testTime() {
    forCases("clock", [](Tally& t, int i, Row r, const Family&) {
        EXPECT(t, i, "curclock", r("curclock'"),
               ref::clockAdvance(std::uint32_t(r("curclock")), std::uint32_t(r("lastReal")), std::uint32_t(r("timeGetTime"))));
    });
    forCases("tick_schedule", [](Tally& t, int i, Row r, const Family&) {
        ref::MsvcRand rng;
        rng.seed(std::uint32_t(r("seed")));
        auto p = ref::tickPeriod(std::uint16_t(r("frameDuration")), std::uint16_t(r("flags")), rng);
        const int ticked = !p ? 2 : ref::tickDue(std::uint32_t(r("curclock")), std::uint32_t(r("lastTact")), *p) ? 1 : 0;
        EXPECT(t, i, "ticked", r("ticked"), ticked);
        EXPECT(t, i, "seed", r("seed'"), rng.state);
    });
    forCases("sprite_timer", [](Tally& t, int i, Row r, const Family&) {
        const std::uint32_t dt = std::uint32_t(r("curclock")) - std::uint32_t(r("lastTact"));
        EXPECT(t, i, "timer", r("timer'"), ref::timerCountdown(std::uint16_t(r("timer")), dt));
        EXPECT(t, i, "frame", r("frame'"), std::int16_t(std::uint16_t(r("frame") + 1)));
    });
}

void testVm() {
    forCases("vm_arith", [](Tally& t, int i, Row r, const Family&) {
        const int op = int(r("op"));
        const auto a = std::int32_t(r("a")), b = std::int32_t(r("b"));
        auto v = (op == ref::VmNeg || op == ref::VmNot || op == ref::VmLnot) ? ref::vmUnary(op, a) : ref::vmBinary(op, a, b);
        EXPECT(t, i, "crash", r("crash"), !v.has_value());
        if (v) EXPECT(t, i, "result", r("result"), *v);
    });
}

ref::AnimVid animVid(const Row& r, int lenCol, int dirs) {
    ref::AnimVid v;
    for (int k = 0; k < 16; ++k) v.lengths[k] = std::uint8_t(r.at(lenCol + k));
    v.dirs = std::uint8_t(dirs);
    v.dirStep = std::uint8_t(256 / dirs);
    return v;
}

void testAnimation() {
    forCases("change_action", [](Tally& t, int i, Row r, const Family& f) {
        const ref::AnimVid v = animVid(r, f.col("len0"), int(r("dirs")));
        const ref::AnimVid cv = animVid(r, f.col("childLen0"), int(r("childDirs")));
        ref::AnimSprite s{std::uint8_t(r("action")), std::uint8_t(r("direction")), 0x1111, 0x2222, 0x3333, std::uint16_t(r("state"))};
        ref::AnimSprite c{std::uint8_t(r("childAction")), std::uint8_t(r("childDirection")), 0x1111, 0x2222, 0x3333,
                          std::uint16_t(r("childState"))};
        const bool hasChild = r("hasChild") != 0;
        ref::changeAction(s, v, std::int32_t(r("act")), std::int32_t(r("dir")), hasChild ? &c : nullptr, hasChild ? &cv : nullptr);
        EXPECT(t, i, "action", r("action'"), s.action);
        EXPECT(t, i, "direction", r("direction'"), s.direction);
        EXPECT(t, i, "frame", r("frame'"), s.frame);
        EXPECT(t, i, "firstFrame", r("firstFrame'"), s.firstFrame);
        EXPECT(t, i, "lastFrame", r("lastFrame'"), s.lastFrame);
        EXPECT(t, i, "state", r("state'"), s.state);
        if (hasChild) {
            EXPECT(t, i, "child.action", r("child.action'"), c.action);
            EXPECT(t, i, "child.direction", r("child.direction'"), c.direction);
            EXPECT(t, i, "child.frame", r("child.frame'"), c.frame);
            EXPECT(t, i, "child.firstFrame", r("child.firstFrame'"), c.firstFrame);
            EXPECT(t, i, "child.lastFrame", r("child.lastFrame'"), c.lastFrame);
            EXPECT(t, i, "child.state", r("child.state'"), c.state);
        }
    });
    forCases("turn_to", [](Tally& t, int i, Row r, const Family&) {
        auto res = ref::turnTo(std::uint8_t(r("direction")), std::uint8_t(r("action")), std::int32_t(r("target")),
                               std::uint8_t(r("dirs")), std::uint8_t(r("dirStep")), std::uint16_t(r("flags")));
        EXPECT(t, i, "ret", r("ret"), res.done ? 1 : 0);
        EXPECT(t, i, "direction", r("direction'"), res.direction);
        EXPECT(t, i, "caAct", r("caAct"), res.changeAct);
        if (res.changeAct >= 0) EXPECT(t, i, "caDir", r("caDir"), res.changeDir);
    });
}

void testSetDestination() {
    forCases("set_destination", [](Tally& t, int i, Row r, const Family&) {
        ref::DestVid v{std::int16_t(r("speedX")), std::int16_t(r("speedY")), std::uint8_t(r("dirs")), std::uint16_t(r("flags")),
                       std::uint8_t(r("category")), std::uint8_t(r("cls"))};
        ref::DestSprite s;
        s.x = std::int16_t(r("x")); s.y = std::int16_t(r("y")); s.z = std::int16_t(r("z"));
        s.direction = std::uint8_t(r("direction")); s.state = std::uint16_t(r("state")); s.speed = std::int16_t(r("speed"));
        s.vz = 0x1234; s.fracX = s.fracY = s.fracZ = 0x55; s.goX = 0x2222; s.goY = 0x3333; s.goZ = 0x4444;
        auto res = ref::setDestination(s, v, std::int32_t(r("argX")), std::int32_t(r("argY")), std::int32_t(r("argZ")));
        EXPECT(t, i, "state", r("state'"), s.state);
        EXPECT(t, i, "direction", r("direction'"), s.direction);
        EXPECT(t, i, "goX", r("goX"), s.goX);
        EXPECT(t, i, "goY", r("goY"), s.goY);
        EXPECT(t, i, "goZ", r("goZ"), s.goZ);
        EXPECT(t, i, "vz", r("vz"), s.vz);
        EXPECT(t, i, "fracsZeroed", r("fracsZeroed"), s.fracX == 0 && s.fracY == 0 && s.fracZ == 0);
        EXPECT(t, i, "gDist", r("gDist"), res.gDist);
        EXPECT(t, i, "caAct", r("caAct"), res.changeAct);
    });
}

void testPhysics() {
    forCases("physics", [](Tally& t, int i, Row r, const Family&) {
        ref::PhysicsVid v{std::uint16_t(r("flags")), std::int16_t(r("speedX")), std::uint16_t(r("acceleration")),
                          std::int8_t(r("rotationPeriod"))};
        ref::PhysicsSprite s;
        s.x = std::int16_t(r("x")); s.y = std::int16_t(r("y")); s.z = std::int16_t(r("z"));
        s.direction = std::uint8_t(r("direction")); s.state = std::uint16_t(r("state"));
        s.lastTact = std::uint32_t(r("lastTact")); s.speed = std::int16_t(r("speed")); s.vz = std::int16_t(r("vz"));
        s.fracX = std::int8_t(r("fracX")); s.fracY = std::int8_t(r("fracY")); s.fracZ = std::int8_t(r("fracZ"));
        s.stepAccum = std::uint8_t(r("stepAccum"));
        s.goX = std::int16_t(r("goX")); s.goY = std::int16_t(r("goY")); s.goZ = std::int16_t(r("goZ"));
        // grid parameters of the generator (tools/oracle/gen_physics.py)
        ref::PhysicsWorld w{std::uint32_t(r("curclock")), std::int32_t(r("mapW")), std::int32_t(r("mapH")),
                            std::int16_t(r("windStrength")), std::uint32_t(r("windDir")), 5, 4, 100, 120};
        constexpr int rowShift = 7;
        std::vector<std::array<std::int32_t, 3>> acts;
        int checkMoves = 0, obstacles = 0;
        std::int32_t cm[3] = {0, 0, 0};
        auto res = ref::physicsStep(
            s, v, w, [&](int act, std::int32_t a, std::int32_t b) { acts.push_back({act, a, b}); },
            [&](std::int32_t x, std::int32_t y, std::int32_t z) { ++checkMoves; cm[0] = x; cm[1] = y; cm[2] = z; return r("blocked") != 0; },
            [&](std::int32_t, std::int32_t, std::int32_t) { ++obstacles; return std::uint32_t(r("obstacle")); });
        EXPECT(t, i, "x", r("x'"), s.x);
        EXPECT(t, i, "y", r("y'"), s.y);
        EXPECT(t, i, "z", r("z'"), s.z);
        EXPECT(t, i, "state", r("state'"), s.state);
        EXPECT(t, i, "speed", r("speed'"), s.speed);
        EXPECT(t, i, "vz", r("vz'"), s.vz);
        EXPECT(t, i, "fracX", r("fracX'"), s.fracX);
        EXPECT(t, i, "fracY", r("fracY'"), s.fracY);
        EXPECT(t, i, "fracZ", r("fracZ'"), s.fracZ);
        EXPECT(t, i, "stepAccum", r("stepAccum'"), s.stepAccum);
        EXPECT(t, i, "stepSet", r("stepSet"), res.step.has_value() && !res.passive);
        if (res.step) EXPECT(t, i, "step", r("step"), *res.step);
        EXPECT(t, i, "nActions", r("nActions"), std::int64_t(acts.size()));
        for (std::size_t k = 0; k < acts.size() && k < 2; ++k) {
            const char* p = k ? "act2" : "act1";
            EXPECT(t, i, "act", r(p), acts[k][0]);
            EXPECT(t, i, "act.v1", r(std::string(p) + "v1"), acts[k][1]);
            EXPECT(t, i, "act.v2", r(std::string(p) + "v2"), acts[k][2]);
        }
        EXPECT(t, i, "checkMoveCalls", r("checkMoveCalls"), checkMoves);
        if (checkMoves) {
            EXPECT(t, i, "cmX", r("cmX"), cm[0]);
            EXPECT(t, i, "cmY", r("cmY"), cm[1]);
            EXPECT(t, i, "cmZ", r("cmZ"), cm[2]);
        }
        EXPECT(t, i, "obstacleCalls", r("obstacleCalls"), obstacles);
        EXPECT(t, i, "cellRemoved", r("cellRemoved"), res.cellRemoved ? (res.cellRemoved->y << rowShift) + res.cellRemoved->x : -1);
        EXPECT(t, i, "cellAdded", r("cellAdded"), res.cellAdded ? (res.cellAdded->y << rowShift) + res.cellAdded->x : -1);
    });
}

// Grid contents of a variable-length case: object k of the case is filed in (cellX, cellY);
// `self` (if any) comes first in its cell. Returns candidate indices in iteration order.
std::vector<int> gridOrder(const ref::GridRange& range, int nObjects, const std::vector<std::pair<int, int>>& cells,
                           int selfCellX, int selfCellY) {
    std::vector<int> order;
    for (int cy = range.y0; cy <= range.y1; ++cy)
        for (int cx = range.x0; cx <= range.x1; ++cx) {
            if (cx == selfCellX && cy == selfCellY) order.push_back(-1);
            for (int k = 0; k < nObjects; ++k)
                if (cells[k].first == cx && cells[k].second == cy) order.push_back(k);
        }
    return order;
}

void testCheckMove() {
    forCases("check_move", [](Tally& t, int i, Row r, const Family& f) {
        const int sizeX = int(r("sizeX")), sizeY = int(r("sizeY")), sizeZ = int(r("sizeZ"));
        auto d = ref::deriveVid(32, std::int16_t(sizeX), std::int16_t(sizeY), std::int16_t(sizeZ), 0);
        ref::CollisionVid self{std::uint8_t(r("category")), std::uint16_t(r("flags")), std::uint8_t(r("collisionMask")),
                               std::int16_t(sizeZ), d->halfX, d->halfY, d->absSizeZ};
        const int n = int(r("nObjects"));
        const std::size_t base = f.columns.size(), stride = f.objectColumns.size();
        std::vector<ref::CollisionObject> objs(n);
        std::vector<std::pair<int, int>> cells(n);
        for (int k = 0; k < n; ++k) {
            auto o = [&](const char* c) { return r.at(base + k * stride + f.ocol(c)); };
            auto od = ref::deriveVid(32, std::int16_t(o("sizeX")), std::int16_t(o("sizeY")), std::int16_t(o("sizeZ")), 0);
            objs[k].x = std::int16_t(o("x")); objs[k].y = std::int16_t(o("y")); objs[k].z = std::int16_t(o("z"));
            objs[k].action = std::uint8_t(o("action"));
            objs[k].vid = {std::uint8_t(o("category")), std::uint16_t(o("flags")), std::uint8_t(o("collisionMask")),
                           std::int16_t(o("sizeZ")), od->halfX, od->halfY, od->absSizeZ};
            cells[k] = {int(o("cellX")), int(o("cellY"))};
        }
        const auto x = std::int32_t(r("argX")), y = std::int32_t(r("argY")), z = std::int32_t(r("argZ"));
        const ref::GridSpec grid{5, 5, 64, 64};
        const bool inGrid = r("selfInGrid") != 0;
        auto order = gridOrder(ref::checkMoveQuery(grid, self, x, y), n, cells, inGrid ? int(r("selfX")) >> 5 : -9,
                               inGrid ? int(r("selfY")) >> 5 : -9);
        std::vector<ref::CollisionObject> cand;
        for (int k : order) {
            if (k >= 0) { cand.push_back(objs[k]); continue; }
            ref::CollisionObject me;  // the moving sprite met in its own cell
            me.x = std::int16_t(r("selfX")); me.y = std::int16_t(r("selfY")); me.z = std::int16_t(z);
            me.vid = self; me.isSelf = true;
            cand.push_back(me);
        }
        std::uint32_t crushed = 0;
        int got = ref::checkMove(self, x, y, z, cand, [&](int idx) { crushed |= 1u << order[idx]; });
        if (got >= 0) got = order[got] < 0 ? -3 /* self returned from the grid: never expected */ : order[got];
        EXPECT(t, i, "blocker", r("blocker"), got);
        EXPECT(t, i, "crushedMask", r("crushedMask"), crushed);
    });
}

void testDamage() {
    forCases("damage", [](Tally& t, int i, Row r, const Family&) {
        auto res = ref::applyDamage(std::uint8_t(r("maxHP")), std::uint8_t(r("hp")), std::int32_t(r("damage")));
        EXPECT(t, i, "hp", r("hp'"), res.hp);
        EXPECT(t, i, "caAct", r("caAct"), res.changeAct);
        EXPECT(t, i, "woundChildRemoved", r("woundChildRemoved"), res.woundChildRemoved);
    });
    forCases("area_damage", [](Tally& t, int i, Row r, const Family& f) {
        auto d = ref::deriveVid(32, std::int16_t(r("sizeX")), std::int16_t(r("sizeY")), 0, 0);
        ref::Exploder e{std::int16_t(r("x")), std::int16_t(r("y")), std::int16_t(r("z")), std::uint8_t(r("deathDamage")),
                        std::int16_t(std::uint16_t(r("deathDamageRadius"))), d->halfX, d->halfY};
        const int n = int(r("nObjects"));
        const std::size_t base = f.columns.size(), stride = f.objectColumns.size();
        std::vector<ref::DamageTarget> objs(n);
        std::vector<std::pair<int, int>> cells(n);
        for (int k = 0; k < n; ++k) {
            auto o = [&](const char* c) { return r.at(base + k * stride + f.ocol(c)); };
            auto od = ref::deriveVid(32, std::int16_t(o("sizeX")), std::int16_t(o("sizeY")), std::int16_t(o("sizeZ")), 0);
            objs[k] = {std::int16_t(o("x")), std::int16_t(o("y")), std::int16_t(o("z")), std::uint8_t(o("maxHP")),
                       od->halfX, od->halfY, od->absSizeZ, false};
            cells[k] = {int(o("cellX")), int(o("cellY"))};
        }
        const ref::GridSpec grid{5, 5, 64, 64};
        auto order = gridOrder(ref::areaDamageQuery(grid, e), n, cells, int(r("x")) >> 5, int(r("y")) >> 5);
        std::vector<ref::DamageTarget> cand;
        for (int k : order) {
            if (k >= 0) cand.push_back(objs[k]);
            else { ref::DamageTarget me{e.x, e.y, e.z, std::uint8_t(r("maxHP")), e.halfX, e.halfY, 0, true}; cand.push_back(me); }
        }
        std::uint32_t hit = 0;
        bool hitTwice = false;
        const bool exploded = ref::areaDamage(e, cand, [&](int idx, int dmg) {
            const std::uint32_t bit = 1u << order[idx];
            hitTwice |= (hit & bit) != 0;
            hit |= bit;
            if (dmg != e.deathDamage) hitTwice = true;
        });
        EXPECT(t, i, "hp", r("hp'"), exploded ? 0 : 9);
        EXPECT(t, i, "hitMask", r("hitMask"), hit);
        EXPECT(t, i, "hitOnce", 0, hitTwice);
    });
}

} // namespace

int main() {
    testTables();
    testRand();
    testDirectionDistance();
    testTime();
    testVm();
    testAnimation();
    testSetDestination();
    testPhysics();
    testCheckMove();
    testDamage();
    return gromada_test::finish();
}
