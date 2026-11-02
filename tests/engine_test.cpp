// Synthetic tests of the simulation engine (src/game): no game data needed.
// Reference values come from docs/re/engine.md and tools/re/engine_reference.py.
#include "game/engine.hpp"
#include "test.hpp"

using namespace gromada;
using namespace gromada::game;

namespace {

struct VidSpec {
    std::uint8_t cls = cls::Vehicle;
    std::uint8_t category = cat::Vehicle;
    std::uint16_t flags = vf::PresentOnGrid;
    int size = 20, sizeZ = 10;
    int maxHP = 0;
    int speedX = 0, speedY = 0, accel = 9999;
    int dirs = 1, rot = 0;
    int zLayer = 5;
    int army = 1;
    int weapon = 0;
    int visibility = 0;
    int deathDamage = 0, deathRadius = 0;
    std::uint8_t mask = 1;
    std::array<std::uint8_t, 16> anim{1};
};

Vid makeVid(const VidSpec& s) {
    Vid v;
    v.category = static_cast<ObjectCategory>(s.category);
    v.type = static_cast<ObjectClass>(s.cls);
    v.flags = static_cast<ObjectFlags>(s.flags);
    v.collisionMask = s.mask;
    v.sizeX = v.sizeY = static_cast<std::uint16_t>(s.size);
    v.sizeZ = static_cast<std::uint16_t>(s.sizeZ);
    v.maxHP = static_cast<std::uint8_t>(s.maxHP);
    v.visibilityRadius = static_cast<std::uint16_t>(s.visibility);
    v.speedX = static_cast<std::uint16_t>(s.speedX);
    v.speedY = static_cast<std::uint16_t>(s.speedY);
    v.acceleration = static_cast<std::uint16_t>(s.accel);
    v.rotationPeriod = static_cast<std::uint8_t>(s.rot);
    v.directionsCount = static_cast<std::uint8_t>(s.dirs);
    v.z_layer = static_cast<std::uint8_t>(s.zLayer);
    v.army = static_cast<std::uint8_t>(s.army);
    v.someWeaponIndex = static_cast<std::uint8_t>(s.weapon);
    v.deathDamage = static_cast<std::uint8_t>(s.deathDamage);
    v.deathDamageRadius = static_cast<std::uint16_t>(s.deathRadius);
    v.animationLengths = s.anim;
    return v;
}

// Runs the world with fixed frames of `stepMs` for `ms` milliseconds.
void run(World& w, int ms, int stepMs = 17) {
    for (int t = 0; t < ms; t += stepMs) {
        w.advanceClock(stepMs);
        w.tact();
    }
}

void testRng() {
    MsvcRand r;   // CRT initial state 1
    const int expect1[] = {41, 18467, 6334, 26500, 19169};
    for (int e : expect1) CHECK_EQ(r.next(), e);
    r.seed(12345);
    const int expect2[] = {7584, 19164, 25795, 22125, 5828};
    for (int e : expect2) CHECK_EQ(r.next(), e);
}

void testFixedMath() {
    struct D { int x1, y1, x2, y2, dir, dist; };
    const D cases[] = {{0, 0, 0, -100, 1, 100},   {0, 0, 100, 0, 65, 100},      {0, 0, 0, 100, 127, 100},
                       {0, 0, -100, 0, 193, 100}, {0, 0, 300, 400, 102, 500},   {10, 20, -35, 77, 155, 72},
                       {100, 100, 103, -250, 1, 350}, {0, 0, -1, -1, 224, 4}, {500, 500, 500, 500, 133, 0},
                       {0, 0, 1000, -3, 63, 1000}};
    for (const auto& c : cases) {
        const auto r = fx::getDirection(c.x1, c.y1, c.x2, c.y2);
        CHECK_EQ(static_cast<int>(r.dir()), c.dir);
        CHECK_EQ(r.distance, c.dist);
    }
    CHECK_EQ(fx::Sin[0], 0);
    CHECK_EQ(fx::Sin[64], 1024);
    CHECK_EQ(fx::NegCos[0], -1024);
    CHECK_EQ(fx::Sin[32], 724);
    CHECK_EQ(fx::NegCos[128], 1024);
    CHECK_EQ(fx::Sin[200], -fx::Sin[72]);
    CHECK_EQ(fx::gameDistance(3, 4), 6);
    CHECK_EQ(fx::gameDistance(-10, 4), 10);
    CHECK_EQ(fx::tickPeriod(71, false, 0), 71u);
    CHECK_EQ(fx::tickPeriod(71, true, 41), 74u);   // 71 + 8 - 41 % 18
    CHECK_EQ(fx::integrateSpeed(0, 160, 80, 71, true), 11);
    CHECK_EQ(fx::integrateSpeed(80, 160, 80, 71, false), 58);
    CHECK_EQ(fx::integrateSpeed(10, 9999, 80, 71, true), 80);
    CHECK_EQ(fx::integrateSpeed(10, 9999, 80, 71, false), 0);
    CHECK_EQ(fx::integrateSpeed(5, 160, 80, 71, false), 0);
    // 80 px/1024 ms east for 16 x 64 ms -> exactly 80 px
    int x = 0, y = 0;
    std::int8_t fxa = 0, fya = 0;
    for (int i = 0; i < 16; ++i) {
        const auto m = fx::moveStep(80, 64, 64, fxa, fya);
        x += m.mx; y += m.my; fxa = m.fracX; fya = m.fracY;
    }
    CHECK_EQ(x, 80);
    CHECK_EQ(y, 0);
    x = y = 0; fxa = fya = 0;
    for (int i = 0; i < 10; ++i) {
        const auto m = fx::moveStep(123, 71, 37, fxa, fya);
        x += m.mx; y += m.my; fxa = m.fracX; fya = m.fracY;
    }
    CHECK_EQ(x, 67);
    CHECK_EQ(y, -53);
    CHECK_EQ(static_cast<int>(fxa), 24);
    CHECK_EQ(static_cast<int>(fya), 74);
    const auto vz = fx::applyGravity(-100, 71);
    CHECK_EQ(vz, -112);
    const auto vs = fx::verticalStep(vz, 71, 0);
    CHECK_EQ(vs.delta, -8);
    CHECK_EQ(static_cast<int>(vs.frac), 30);
    CHECK_EQ(fx::rotateStep(32, 0, false), 248);
    CHECK_EQ(fx::rotateStep(32, 0, true), 8);
    CHECK_EQ(fx::rotateStep(8, 0, true), 32);
    CHECK_EQ(fx::rotateStep(8, 0, false), 224);
    CHECK_EQ(fx::rotateStep(32, 250, true), 0);
    CHECK_EQ(fx::directionIndex(32, 250), 31);
    CHECK_EQ(fx::directionIndex(32, 252), 0);
    CHECK_EQ(fx::directionIndex(1, 200), 0);
}

void testClockAndScheduling() {
    std::vector<Vid> vids{makeVid({.cls = cls::Static, .category = cat::Object, .flags = 0}),
                          makeVid({.cls = cls::Static, .category = cat::Object, .flags = vf::Randomized})};
    World w{vids, {}};
    w.loadEmpty(1000, 1000, 1, 1000);
    w.advanceClock(100);
    CHECK_EQ(w.clock(), 1071u);
    w.advanceClock(-3);
    CHECK_EQ(w.clock(), 1071u);
    w.advanceClock(16);
    CHECK_EQ(w.clock(), 1087u);

    // one tick when 71 ms have elapsed, at most one per frame
    Sprite* s = w.createUnit(0, 100, 100, 0, 0, 0, nullptr);
    CHECK(s != nullptr);
    std::vector<std::uint32_t> ticks;
    for (int i = 0; i < 12; ++i) {
        const auto before = s->lastTact;
        w.advanceClock(17);
        w.tact();
        if (s->lastTact != before) ticks.push_back(w.clock());
    }
    CHECK_EQ(ticks.size(), 2u);
    if (ticks.size() == 2) {
        CHECK_EQ(ticks[0], 1087u + 5 * 17);    // 85 ms >= 71
        CHECK_EQ(ticks[1], ticks[0] + 5 * 17);
    }
    // a frame longer than the period still gives a single tick
    w.advanceClock(71);
    const auto before = s->lastTact;
    w.tact();
    CHECK(s->lastTact != before);

    // Randomized: +-8 px at creation (2 rand) and one rand per frame for the period jitter
    MsvcRand ref;
    ref.seed(w.rng().state);
    Sprite* r = w.createUnit(1, 500, 500, 0, 0, 0, nullptr);
    const int ex = 500 + 8 - ref.next() % 17, ey = 500 + 8 - ref.next() % 17;
    CHECK(r != nullptr);
    if (r) { CHECK_EQ(static_cast<int>(r->x), ex); CHECK_EQ(static_cast<int>(r->y), ey); }
    for (int i = 0; i < 7; ++i) { w.advanceClock(17); w.tact(); ref.next(); }
    CHECK_EQ(w.rng().state, ref.state);
}

void testCreateLimits() {
    std::vector<Vid> vids{makeVid({.cls = cls::Static, .category = cat::Object, .flags = 0})};
    World w{vids, {}};
    w.loadEmpty(500, 400, 7);
    CHECK(w.createUnit(0, 600, 10, 0, 0, 0, nullptr) == nullptr);   // outside the map
    CHECK(w.createUnit(0, -1, 10, 0, 0, 0, nullptr) == nullptr);
    CHECK(w.createUnit(0, 500, 400, 0, 0, 0, nullptr) != nullptr);  // the border is inside
    w.vid(0).maxUnit = 2;
    CHECK(w.createUnit(0, 10, 10, 0, 0, 0, nullptr) != nullptr);
    CHECK(w.createUnit(0, 20, 10, 0, 0, 0, nullptr) == nullptr);    // MaxUnit
    CHECK_EQ(w.vid(0).count, 2);
    w.vid(0).maxUnit = -1;
    // ids are never reused within a map
    Sprite* a = w.createUnit(0, 30, 30, 0, 0, 0, nullptr);
    const UnitId id = a->id;
    w.action(*a, act::destroy, 0, 0);
    CHECK(w.byId(id) == nullptr);
    w.tact();
    Sprite* b = w.createUnit(0, 30, 30, 0, 0, 0, nullptr);
    CHECK(b->id != id);
    CHECK(w.byId(id) == nullptr);
}

void testCommandStack() {
    VidSpec spec;
    spec.anim.fill(1);
    std::vector<Vid> vids{makeVid(spec)};
    World w{vids, {}};
    w.loadEmpty(1000, 1000, 3);
    Sprite* s = w.createUnit(0, 100, 100, 0, 0, 0, nullptr);
    // script AddCommand goes to the bottom: executed in the order given (FIFO)
    w.pushCommandBottom(*s, act::salut, 0, 0);
    w.pushCommandBottom(*s, act::close, 0, 0);
    w.pushCommandBottom(*s, act::open, 0, 0);
    CHECK_EQ(w.getCommands(*s), std::string("B0,0C0,0E0,0"));   // e[0]..e[2]: open, close, salut
    // the engine pushes on top: executed first
    w.pushCommandTop(*s, act::load, 0, 0);
    std::vector<int> seen;
    for (int i = 0; i < 40 && seen.size() < 4; ++i) {
        const int before = s->commands ? s->commands->count : 0;
        w.advanceClock(17);
        w.tact();
        const int after = s->commands ? s->commands->count : 0;
        if (after != before) seen.push_back(s->action);
    }
    CHECK((seen == std::vector<int>{act::load, act::salut, act::close, act::open}));
    CHECK(!s->commands);

    // act_cyclecommand at the bottom loops the sequence; further AddCommands are refused
    w.pushCommandBottom(*s, act::salut, 0, 0);
    w.pushCommandBottom(*s, act::close, 0, 0);
    w.pushCommandBottom(*s, act::cyclecommand, 2, 0);
    w.pushCommandBottom(*s, act::open, 0, 0);
    CHECK_EQ(s->commands->count, 3);
    std::vector<int> cycle;
    for (int i = 0; i < 80 && cycle.size() < 6; ++i) {
        const auto before = s->lastTact;
        w.advanceClock(17);
        w.tact();
        if (s->lastTact != before) cycle.push_back(s->action);
    }
    CHECK((cycle == std::vector<int>{act::salut, act::close, act::salut, act::close, act::salut, act::close}));
    CHECK(s->commands != nullptr);

    // SetCommands/GetCommands text form "%c%i,%i" (act + 60)
    w.action(*s, act::clearcom, 0, 0);
    w.setCommands(*s, "d100,200E0,0");
    CHECK_EQ(w.getCommands(*s), std::string("d100,200E0,0"));
}

void testMovementAndCollision() {
    std::vector<Vid> vids{
        makeVid({.speedX = 80, .dirs = 32, .rot = 35}),                                   // 0: tank
        makeVid({.size = 40}),                                                           // 1: obstacle unit
        makeVid({.cls = cls::Terrain, .category = cat::Terrain, .size = 24, .mask = 2}),  // 2: water tile
    };
    World w{vids, {}};
    w.loadEmpty(2000, 2000, 5, 0);
    Sprite* t = w.createUnit(0, 200, 500, 0, 0, 64, nullptr);
    w.action(*t, act::move, 500, 500);
    CHECK(t->state & st::Move);
    run(w, 6000);
    CHECK(std::abs(t->x - 500) <= w.scaleX());
    CHECK(std::abs(t->y - 500) <= w.scaleY());
    CHECK(!(t->state & st::Move));
    CHECK_EQ(t->wpX, -1);

    // a unit in the way blocks the move (AABB with heights)
    Sprite* o = w.createUnit(1, 800, 500, 0, 0, 0, nullptr);
    w.action(*t, act::move, 1100, 500);
    int minGap = 10000;
    for (int i = 0; i < 300; ++i) {
        w.advanceClock(17);
        w.tact();
        if (std::abs(t->y - o->y) < 30) minGap = std::min(minGap, std::abs(t->x - o->x));
    }
    CHECK(minGap >= 30);   // never overlaps: halfX 10 + 20

    // CheckMove: terrain blocks when the collision masks share no bit, units at z>0 pass over it
    w.createUnit(2, 300, 900, 0, 0, 0, nullptr);
    Sprite* u = w.createUnit(0, 250, 900, 0, 0, 0, nullptr);
    CHECK(w.checkMove(*u, 290, 900, 0) != nullptr);
    CHECK(w.checkMove(*u, 290, 900, 30) == nullptr);
    CHECK(w.checkMove(*u, 250, 960, 0) == nullptr);
}

void testDamage() {
    std::vector<Vid> vids{makeVid({.cls = cls::Static, .category = cat::Object, .maxHP = 20})};
    World w{vids, {}};
    w.loadEmpty(1000, 1000, 9);
    Sprite* s = w.createUnit(0, 100, 100, 0, 0, 0, nullptr);
    CHECK_EQ(static_cast<int>(s->hp), 20);
    w.action(*s, act::damage, 5, 0);
    CHECK_EQ(static_cast<int>(s->hp), 15);
    CHECK_EQ(static_cast<int>(s->action), act::stand);
    w.action(*s, act::damage, 6, 0);
    CHECK_EQ(static_cast<int>(s->hp), 9);
    CHECK_EQ(static_cast<int>(s->action), act::wound);   // crossed half of maxHP
    w.action(*s, act::damage, -15, 0);                   // negative damage heals
    CHECK_EQ(static_cast<int>(s->hp), 24);
    w.action(*s, act::damage, 24, 0);
    CHECK_EQ(static_cast<int>(s->action), act::death);

    // area damage at the end of the death animation: radius + half sizes, no falloff
    std::vector<Vid> vids2{
        makeVid({.flags = vf::PresentOnGrid, .size = 20, .maxHP = 30}),
        makeVid({.cls = cls::Projectile, .category = cat::Cannon, .flags = 0, .size = 4, .deathDamage = 7, .deathRadius = 10}),
    };
    World w2{vids2, {}};
    w2.loadEmpty(1000, 1000, 9);
    Sprite* nearU = w2.createUnit(0, 520, 500, 0, 0, 0, nullptr);
    Sprite* farU = w2.createUnit(0, 600, 500, 0, 0, 0, nullptr);
    Sprite* p = w2.createUnit(1, 500, 500, 0, 0, 0, nullptr);   // act_init -> act_death
    CHECK_EQ(static_cast<int>(p->action), act::death);
    run(w2, 200);
    CHECK_EQ(static_cast<int>(nearU->hp), 23);   // |dx| 20 < 10 + 2 + 10
    CHECK_EQ(static_cast<int>(farU->hp), 30);
    CHECK_EQ(w2.liveCount(), 2u);
}

void testTargets() {
    std::vector<Weapon> weapons(2);
    weapons[1] = Weapon{.targetCategory = ObjectCategory::Vehicle, .flags = 0, .alwaysZero = 0, .weaponRange = 150,
                        .scatter = 0, .cooldown = 50};
    std::vector<Vid> vids{
        makeVid({.army = 1, .weapon = 1, .visibility = 300}),   // 0: shooter
        makeVid({.army = 0}),                                    // 1: enemy
        makeVid({.army = 2}),                                    // 2: neutral
        makeVid({.army = 1}),                                    // 3: friend
    };
    World w{vids, weapons};
    w.loadEmpty(2000, 2000, 11);
    Sprite* s = w.createUnit(0, 500, 500, 0, 0, 0, nullptr);
    w.createUnit(2, 520, 500, 0, 0, 0, nullptr);
    w.createUnit(3, 530, 500, 0, 0, 0, nullptr);
    Sprite* far = w.createUnit(1, 750, 500, 0, 0, 0, nullptr);
    Sprite* nearE = w.createUnit(1, 600, 520, 0, 0, 0, nullptr);
    CHECK(w.findTarget(*s) == nearE);
    w.action(*nearE, act::setstate, st::Invisible, 0);
    CHECK(w.findTarget(*s) == far);
    w.createUnit(1, 900, 500, 0, 0, 0, nullptr);   // beyond the visibility radius
    CHECK(w.findTarget(*s) == far);
}

void testPause() {
    std::vector<Vid> vids{makeVid({.speedX = 100, .dirs = 1})};
    World w{vids, {}};
    w.loadEmpty(2000, 2000, 1, 5000);
    Sprite* t = w.createUnit(0, 100, 100, 0, 0, 64, nullptr);
    w.action(*t, act::move, 1500, 100);
    run(w, 1000);
    const int x0 = t->x;
    const auto c0 = w.clock();
    w.pause(true);
    run(w, 3000);
    CHECK_EQ(static_cast<int>(t->x), x0);   // frozen
    CHECK(w.clock() > c0);                   // GetTime keeps running in the pause menus
    w.pause(false);
    CHECK_EQ(w.clock(), c0);                 // brought back
    w.advanceClock(71);
    w.tact();
    CHECK(t->x - x0 < 20);                   // no jump forward after the pause
}

void testBaseActions() {
    std::vector<Vid> vids{makeVid({.speedX = 50, .dirs = 32, .maxHP = 10})};
    World w{vids, {}};
    w.loadEmpty(1000, 1000, 1);
    CHECK(isEngineBuiltin(65));
    CHECK(isEngineBuiltin(128));
    CHECK(!isEngineBuiltin(64));
    CHECK(!isEngineBuiltin(73));
    Sprite* s = w.createUnit(0, 100, 200, 0, 0, 0, nullptr);
    CHECK_EQ(w.action(*s, act::gethp, 0, 0), 10);
    CHECK_EQ(w.action(*s, act::getarmy, 0, 0), 1);
    w.action(*s, act::setarmy, 0, 0);
    CHECK_EQ(w.action(*s, act::getarmy, 0, 0), 0);
    CHECK_EQ(w.action(*s, act::additem, 0, 0), 0);
    CHECK_EQ(w.action(*s, act::askitem, 0, 0), 1);
    CHECK_EQ(w.action(*s, act::deleteitem, 0, 0), 1);
    CHECK_EQ(w.action(*s, act::deleteitem, 0, 0), 0);
    w.action(*s, act::changecoor, 300, 400);
    CHECK_EQ(static_cast<int>(s->x), 300);
    CHECK(w.getObject(0, 305, 402) == s);
    CHECK(w.getObject(0, 100, 200) == nullptr);
}

} // namespace

int main() {
    testRng();
    testFixedMath();
    testClockAndScheduling();
    testCreateLimits();
    testCommandStack();
    testMovementAndCollision();
    testDamage();
    testTargets();
    testPause();
    testBaseActions();
    return gromada_test::finish();
}
