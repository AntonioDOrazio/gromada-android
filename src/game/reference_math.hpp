#pragma once
// Pure reference implementations of integer formulas of the original gromada.exe (issue #11).
//
// Every function reproduces the original bit for bit, including 16/32-bit wraparound and the
// quirks documented in docs/oracle.md, and is checked by tests/oracle_test.cpp against vectors
// produced by executing the original code in an x86 emulator (tools/oracle/). The functions
// only use their own small structs (no engine types), so the engine (issue #8) can call them
// directly or be checked against them.
//
// Conventions: "x86 wrap" = two's complement modulo 2^32 (or 2^16 for 16-bit fields); shifts
// of signed values are arithmetic; divisions truncate toward zero (idiv). Functions that can
// raise a CPU exception in the original (#DE) return std::nullopt (or document a precondition).

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace gromada::game::ref {

// ---- integer helpers --------------------------------------------------------------------
constexpr std::int32_t wrap32(std::uint32_t v) { return static_cast<std::int32_t>(v); }
constexpr std::int32_t add32(std::int32_t a, std::int32_t b) { return wrap32(std::uint32_t(a) + std::uint32_t(b)); }
constexpr std::int32_t sub32(std::int32_t a, std::int32_t b) { return wrap32(std::uint32_t(a) - std::uint32_t(b)); }
constexpr std::int32_t mul32(std::int32_t a, std::int32_t b) { return wrap32(std::uint32_t(a) * std::uint32_t(b)); }
constexpr std::int32_t neg32(std::int32_t a) { return wrap32(0u - std::uint32_t(a)); }
constexpr std::int32_t abs32(std::int32_t a) { return a < 0 ? neg32(a) : a; }  // abs(INT_MIN) = INT_MIN
constexpr std::int16_t lo16(std::int32_t v) { return static_cast<std::int16_t>(static_cast<std::uint16_t>(std::uint32_t(v))); }
constexpr std::int8_t lo8(std::int32_t v) { return static_cast<std::int8_t>(static_cast<std::uint8_t>(std::uint32_t(v))); }
// idiv without the exceptions: callers check b != 0 and !(a == INT_MIN && b == -1).
constexpr std::int32_t idiv(std::int32_t a, std::int32_t b) { return a / b; }
constexpr bool idivFaults(std::int32_t a, std::int32_t b) { return b == 0 || (a == INT32_MIN && b == -1); }

// ---- tables -----------------------------------------------------------------------------
// 0x428e14 / 0x429214: int(1024*sin(2*pi*i/256)) and -int(1024*cos(2*pi*i/256)).
// Direction 0 = north (y decreasing), 64 = east, clockwise.
inline constexpr std::array<std::int32_t, 256> kSin = {
    0, 25, 50, 75, 100, 125, 150, 175, 199, 224, 248, 273, 297, 321, 344, 368,
    391, 414, 437, 460, 482, 504, 526, 547, 568, 589, 609, 629, 649, 668, 687, 706,
    724, 741, 758, 775, 791, 807, 822, 837, 851, 865, 878, 890, 903, 914, 925, 936,
    946, 955, 964, 972, 979, 986, 993, 999, 1004, 1008, 1012, 1016, 1019, 1021, 1022, 1023,
    1024, 1023, 1022, 1021, 1019, 1016, 1012, 1008, 1004, 999, 993, 986, 979, 972, 964, 955,
    946, 936, 925, 914, 903, 890, 878, 865, 851, 837, 822, 807, 791, 775, 758, 741,
    724, 706, 687, 668, 649, 629, 609, 589, 568, 547, 526, 504, 482, 460, 437, 414,
    391, 368, 344, 321, 297, 273, 248, 224, 199, 175, 150, 125, 100, 75, 50, 25,
    0, -25, -50, -75, -100, -125, -150, -175, -199, -224, -248, -273, -297, -321, -344, -368,
    -391, -414, -437, -460, -482, -504, -526, -547, -568, -589, -609, -629, -649, -668, -687, -706,
    -724, -741, -758, -775, -791, -807, -822, -837, -851, -865, -878, -890, -903, -914, -925, -936,
    -946, -955, -964, -972, -979, -986, -993, -999, -1004, -1008, -1012, -1016, -1019, -1021, -1022, -1023,
    -1024, -1023, -1022, -1021, -1019, -1016, -1012, -1008, -1004, -999, -993, -986, -979, -972, -964, -955,
    -946, -936, -925, -914, -903, -890, -878, -865, -851, -837, -822, -807, -791, -775, -758, -741,
    -724, -706, -687, -668, -649, -629, -609, -589, -568, -547, -526, -504, -482, -460, -437, -414,
    -391, -368, -344, -321, -297, -273, -248, -224, -199, -175, -150, -125, -100, -75, -50, -25,
};
inline constexpr std::array<std::int32_t, 256> kNegCos = {
    -1024, -1023, -1022, -1021, -1019, -1016, -1012, -1008, -1004, -999, -993, -986, -979, -972, -964, -955,
    -946, -936, -925, -914, -903, -890, -878, -865, -851, -837, -822, -807, -791, -775, -758, -741,
    -724, -706, -687, -668, -649, -629, -609, -589, -568, -547, -526, -504, -482, -460, -437, -414,
    -391, -368, -344, -321, -297, -273, -248, -224, -199, -175, -150, -125, -100, -75, -50, -25,
    0, 25, 50, 75, 100, 125, 150, 175, 199, 224, 248, 273, 297, 321, 344, 368,
    391, 414, 437, 460, 482, 504, 526, 547, 568, 589, 609, 629, 649, 668, 687, 706,
    724, 741, 758, 775, 791, 807, 822, 837, 851, 865, 878, 890, 903, 914, 925, 936,
    946, 955, 964, 972, 979, 986, 993, 999, 1004, 1008, 1012, 1016, 1019, 1021, 1022, 1023,
    1024, 1023, 1022, 1021, 1019, 1016, 1012, 1008, 1004, 999, 993, 986, 979, 972, 964, 955,
    946, 936, 925, 914, 903, 890, 878, 865, 851, 837, 822, 807, 791, 775, 758, 741,
    724, 706, 687, 668, 649, 629, 609, 589, 568, 547, 526, 504, 482, 460, 437, 414,
    391, 368, 344, 321, 297, 273, 248, 224, 199, 175, 150, 125, 100, 75, 50, 25,
    0, -25, -50, -75, -100, -125, -150, -175, -199, -224, -248, -273, -297, -321, -344, -368,
    -391, -414, -437, -460, -482, -504, -526, -547, -568, -589, -609, -629, -649, -668, -687, -706,
    -724, -741, -758, -775, -791, -807, -822, -837, -851, -865, -878, -890, -903, -914, -925, -936,
    -946, -955, -964, -972, -979, -986, -993, -999, -1004, -1008, -1012, -1016, -1019, -1021, -1022, -1023,
};
// 0x429e18: CORDIC angles in 1/512 of a turn.
inline constexpr std::array<std::uint8_t, 7> kCordicAngles = {64, 37, 19, 10, 5, 2, 1};

// 0x429614/0x429a14 after a map load (0x414f38): trunc(scaleY * table[i] / scaleX), with the
// map's tile size scaleX (0x428e04, normally 24) and scaleY (0x428e08, normally 16). Before
// the first map they hold trunc(2 * table[i] / 3) (0x40d03d), i.e. the same with 24/16.
// Precondition: scaleX != 0.
constexpr std::int32_t perspectiveScaled(std::int32_t tableValue, std::int32_t scaleX, std::int32_t scaleY) {
    return idiv(mul32(scaleY, tableValue), scaleX);
}
constexpr std::int32_t startupScaled(std::int32_t tableValue) { return mul32(2, tableValue) / 3; }
// 0x4316ac: trunc(negCos / sin), 0xfffff where sin == 0.
constexpr std::int32_t cotEntry(int i) { return kSin[i & 255] ? kNegCos[i & 255] / kSin[i & 255] : 0xfffff; }

// Rotation step tables built at startup by 0x40cf70: the direction one step to the left
// (counter-clockwise, 0x431bf4) or right (0x435bf4) of `dir` for a vid with `dirs`
// directions (2..64). Row 1 is filled with the values of 32 directions (never read: TurnTo
// returns early when dirs == 1). Rows above 64 do not exist in the original (dirs > 64 reads
// unrelated memory). The result is the next direction byte.
constexpr std::uint8_t rotationStep(int dirs, int dir, bool right) {
    const int d = dirs == 1 ? 32 : dirs;
    const int base = (((dir + 128 / d) & 0xff) * d) & ~0xff;
    return static_cast<std::uint8_t>((right ? base + 256 : base - 256) / d);
}

// ---- vid fields derived by the loader (0x4066ae..0x406717) --------------------------------
struct VidDerived {
    std::uint8_t dirStep = 0;          // +0x127: 256/dirs (as a byte: 0 when dirs == 1)
    std::int16_t halfX = 0, halfY = 0; // +0x121/+0x123: size >> 1
    std::int16_t absSizeZ = 0;         // +0x125: |sizeZ| (16-bit)
    std::int8_t rotationPeriod = 0;    // +0x37 after loading: rotationPeriod * 6 / dirs
};
// nullopt: dirs == 0 divides by zero in the original.
constexpr std::optional<VidDerived> deriveVid(std::uint8_t dirs, std::int16_t sizeX, std::int16_t sizeY,
                                              std::int16_t sizeZ, std::int8_t rotationPeriod) {
    if (dirs == 0) return std::nullopt;
    VidDerived v;
    v.dirStep = static_cast<std::uint8_t>(256 / dirs);
    v.halfX = static_cast<std::int16_t>(sizeX >> 1);
    v.halfY = static_cast<std::int16_t>(sizeY >> 1);
    v.absSizeZ = lo16(abs32(sizeZ));
    v.rotationPeriod = lo8(rotationPeriod * 6 / dirs);
    return v;
}

// ---- RNG (MSVC CRT rand/srand, 0x41c7b5/0x41c7ab, state at 0x42aebc, initial value 1) -------
struct MsvcRand {
    std::uint32_t state = 1;
    constexpr void seed(std::uint32_t s) { state = s; }
    constexpr std::int32_t next() {
        state = state * 214013u + 2531011u;
        return static_cast<std::int32_t>((state >> 16) & 0x7fff);
    }
};

// Script builtin Random(max) (id 90, 0x4130a4): rand() % (max + 1). The rand() call happens
// first (the state advances even when the original then crashes); nullopt = #DE (max == -1).
constexpr std::optional<std::int32_t> scriptRandom(MsvcRand& rng, std::int32_t max) {
    const std::int32_t r = rng.next();
    const std::int32_t m = add32(max, 1);
    if (idivFaults(r, m)) return std::nullopt;
    return r % m;
}

// ---- direction and distances -------------------------------------------------------------
struct Direction {
    std::int32_t direction = 0; // angle >> 1, NOT masked: -5..261 (callers store the low byte)
    std::int32_t distance = 0;  // g_dist 0x439d4c: (cordicX * 636750) >> 20, wraps beyond ~3372/1.647 px
};
// GetDirection(x1, y1, x2, y2) 0x40b030 (cdecl): 7-step CORDIC from (x1,y1) towards (x2,y2).
constexpr Direction getDirection(std::int32_t x1, std::int32_t y1, std::int32_t x2, std::int32_t y2) {
    std::int32_t x = sub32(x2, x1), y = sub32(y2, y1), angle = 128;
    if (x < 0) { x = neg32(x); y = neg32(y); angle = 384; }
    for (int i = 0; i < 7; ++i) {
        const std::int32_t ox = x, oy = y;
        if (oy >= 0) { x = add32(x, oy >> i); y = sub32(y, ox >> i); angle += kCordicAngles[i]; }
        else         { x = sub32(x, oy >> i); y = add32(y, ox >> i); angle -= kCordicAngles[i]; }
    }
    return {angle >> 1, mul32(x, 636750) >> 20};
}

// "Game" distance max(|dx|, (3|dy|) >> 1): Distance(sprite) 0x40aff0 on the s16 positions.
constexpr std::int32_t gameDistance(std::int16_t ax, std::int16_t ay, std::int16_t bx, std::int16_t by) {
    const std::int32_t dx = abs32(ax - bx), dy = (abs32(ay - by) * 3) >> 1;
    return dx < dy ? dy : dx;
}
// Distance(x, y) 0x40afc0: the point is a full int32 (differences wrap).
constexpr std::int32_t gameDistanceToPoint(std::int16_t ax, std::int16_t ay, std::int32_t px, std::int32_t py) {
    const std::int32_t dx = abs32(sub32(ax, px)), dy = mul32(abs32(sub32(ay, py)), 3) >> 1;
    return dx < dy ? dy : dx;
}
// Script builtin SizeTo(unit, x, y) (id 65): 60000 for a null unit, else the game distance to
// the point truncated to s16.
constexpr std::int32_t sizeTo(bool nullUnit, std::int16_t ux, std::int16_t uy, std::int32_t x, std::int32_t y) {
    return nullUnit ? 60000 : gameDistance(ux, uy, lo16(x), lo16(y));
}

// ---- time ------------------------------------------------------------------------------------
// SVGA::BeginFrame 0x401fb0: curclock += min(now - lastReal, 71) (unsigned, wraps); lastReal = now.
constexpr std::uint32_t clockAdvance(std::uint32_t curclock, std::uint32_t lastReal, std::uint32_t now) {
    const std::uint32_t dt = now - lastReal;
    return curclock + (dt > 71 ? 71u : dt);
}
// World::Tact 0x40bc35: tick period of a sprite; one rand() when the vid is Randomized (0x100).
// Returns nullopt for SkipUpdate (flag 8: never ticks, no rand()).
constexpr std::optional<std::uint32_t> tickPeriod(std::uint16_t frameDuration, std::uint16_t vidFlags, MsvcRand& rng) {
    if (vidFlags & 8) return std::nullopt;
    std::uint32_t p = frameDuration;
    if (vidFlags & 0x100) {
        const std::uint32_t r = static_cast<std::uint32_t>(rng.next());
        p += (p >> 3) - r % ((p >> 2) + 1);
    }
    return p;
}
constexpr bool tickDue(std::uint32_t curclock, std::uint32_t lastTact, std::uint32_t period) {
    return curclock - lastTact >= period;  // unsigned: lastTact > curclock means "due"
}
// SPRITE::Tact 0x409b39: frame++ (16-bit) and the countdown of the timer +0x33.
constexpr std::uint16_t timerCountdown(std::uint16_t timer, std::uint32_t dt) {
    if (!timer) return 0;
    return dt > timer ? 0 : static_cast<std::uint16_t>(timer - dt);
}

// ---- script VM integer opcodes (0x41a790) ----------------------------------------------------
enum VmOp : int {
    VmAdd = 3, VmSub = 4, VmMul = 5, VmDiv = 6, VmMod = 7, VmNeg = 8, VmNot = 9, VmXor = 10, VmOr = 11, VmAnd = 12,
    VmEq = 13, VmLor = 14, VmGt = 15, VmLt = 16, VmGe = 17, VmLe = 18, VmLnot = 19, VmNe = 20, VmLand = 21,
    VmShr = 37, VmShl = 38,
};
// Binary opcode on (a, b) where b is the top of the stack. nullopt = the original crashes (#DE):
// x % 0, INT_MIN / -1, INT_MIN % -1. x / 0 is 0x0FFFFFFF. Shift counts use b & 31.
constexpr std::optional<std::int32_t> vmBinary(int op, std::int32_t a, std::int32_t b) {
    switch (op) {
    case VmAdd: return add32(a, b);
    case VmSub: return sub32(a, b);
    case VmMul: return mul32(a, b);
    case VmDiv: if (b == 0) return 0x0FFFFFFF; if (idivFaults(a, b)) return std::nullopt; return a / b;
    case VmMod: if (idivFaults(a, b)) return std::nullopt; return a % b;
    case VmXor: return a ^ b;
    case VmOr: return a | b;
    case VmAnd: return a & b;
    case VmEq: return a == b;
    case VmNe: return a != b;
    case VmLor: return (a != 0) || (b != 0);
    case VmLand: return (a != 0) && (b != 0);
    case VmGt: return a > b;
    case VmLt: return a < b;
    case VmGe: return a >= b;
    case VmLe: return a <= b;
    case VmShr: return a >> (b & 31);
    case VmShl: return wrap32(std::uint32_t(a) << (b & 31));
    default: return std::nullopt;
    }
}
constexpr std::optional<std::int32_t> vmUnary(int op, std::int32_t a) {
    switch (op) {
    case VmNeg: return neg32(a);
    case VmNot: return ~a;
    case VmLnot: return a == 0;
    default: return std::nullopt;
    }
}

// ---- animation: SetAnimation 0x40b330 / ChangeAction 0x40b3e0 --------------------------------
struct AnimVid {
    std::array<std::uint8_t, 16> lengths{}; // animationLengths (+0x47)
    std::uint8_t dirs = 1;                  // directionsCount (+0x45)
    std::uint8_t dirStep = 0;               // +0x127 = 256/dirs as a byte
};
struct AnimSprite {
    std::uint8_t action = 0, direction = 0;
    std::int16_t frame = 0, firstFrame = 0, lastFrame = 0;
    std::uint16_t state = 0;
};
// Precondition: s.action < 16 (the original stops with a fatal error otherwise).
constexpr void setAnimation(AnimSprite& s, const AnimVid& v) {
    const int a = v.lengths[s.action] ? s.action : 0;
    std::uint16_t first = 0;
    for (int k = 0; k < a; ++k) first = static_cast<std::uint16_t>(first + v.lengths[k] * v.dirs);
    const int index = (((v.dirStep >> 1) + s.direction) & 0xff) * v.dirs >> 8;
    first = static_cast<std::uint16_t>(first + index * v.lengths[a]);
    s.frame = s.firstFrame = static_cast<std::int16_t>(first);
    s.lastFrame = static_cast<std::int16_t>(static_cast<std::uint16_t>(first + v.lengths[a] - 1));
}
// ChangeAction(act, dir): dir == -1 keeps the current direction. `child` = the linked
// sub-object (turret) if any: it follows death (15) and open/close/stand_open (6, 7, 10)
// when its own vid has that animation. Precondition: (act & 0xff) < 16.
constexpr void changeAction(AnimSprite& s, const AnimVid& v, std::int32_t act, std::int32_t dir,
                            AnimSprite* child = nullptr, const AnimVid* childVid = nullptr) {
    const std::uint8_t a = static_cast<std::uint8_t>(act);
    s.state &= 0xfdff;
    if (child) {
        if (a == 15) changeAction(*child, *childVid, act, -1);
        if ((a == 6 || a == 7 || a == 10) && childVid->lengths[a]) changeAction(*child, *childVid, act, -1);
    }
    if (dir == -1) dir = s.direction;
    if (s.action == a && s.direction == dir) return;
    const bool keepFrames = s.action == 10 && !v.lengths[a];
    s.direction = static_cast<std::uint8_t>(dir);
    s.action = a;
    if (!keepFrames) setAnimation(s, v);
    // death, wound and birth without an animation end at the next tick
    if ((a == 15 || a == 13 || a == 14) && !v.lengths[a]) s.frame = s.lastFrame;
}

// ---- TurnTo 0x40b0b0 ------------------------------------------------------------------------
struct TurnResult {
    bool done = false;          // return value: 1 = already facing (direction set), 0 = rotating
    std::uint8_t direction = 0; // sprite direction afterwards
    int changeAct = -1;         // ChangeAction requested (-1 none): 4 l_rotate, 5 r_rotate, 2 go
    std::int32_t changeDir = 0; // its direction argument (-1 = keep)
};
// Precondition: dirs <= 64 (rotation tables). vidFlags bit 1 = RandomDirection.
constexpr TurnResult turnTo(std::uint8_t direction, std::uint8_t action, std::int32_t target,
                            std::uint8_t dirs, std::uint8_t dirStep, std::uint16_t vidFlags) {
    TurnResult r{true, direction, -1, 0};
    if (direction == target) return r;
    if (dirs == 1 || (vidFlags & 1)) { r.direction = static_cast<std::uint8_t>(target); return r; }
    const int half = dirStep >> 1;
    const int si = ((half + direction) & 0xff) * dirs >> 8;
    const int di = ((half + target) & 0xff) * dirs >> 8;
    const int diff = si > di ? si - di : di - si, other = dirs - diff;
    if (si == di) { r.direction = static_cast<std::uint8_t>(target); return r; }
    r.done = false;
    const bool left = si > di ? diff < other : diff > other;
    if (left) {
        if (action == 5) { r.changeAct = 2; r.changeDir = -1; }
        else { r.changeAct = 4; r.changeDir = rotationStep(dirs, direction, false); }
    } else {
        if (action == 4) { r.changeAct = 2; r.changeDir = -1; }
        else { r.changeAct = 5; r.changeDir = rotationStep(dirs, direction, true); }
    }
    return r;
}

// ---- SetDestination 0x409770 ----------------------------------------------------------------
struct DestVid {
    std::int16_t speedX = 0, speedY = 0;
    std::uint8_t dirs = 1;
    std::uint16_t flags = 0;    // 1 RandomDirection, 2 Gravity
    std::uint8_t category = 0;  // 0x10 = Cannon (compared with ==)
    std::uint8_t cls = 0;       // 15 = AviaMissile
};
struct DestSprite {
    std::int16_t x = 0, y = 0, z = 0;
    std::uint8_t direction = 0;
    std::uint16_t state = 0;
    std::int16_t speed = 0, vz = 0;
    std::int8_t fracX = 0, fracY = 0, fracZ = 0;
    std::int16_t goX = 0, goY = 0, goZ = 0;
};
struct DestResult {
    std::int32_t gDist = 0;     // value left in 0x439d4c
    int changeAct = -1;         // ChangeAction(act, -1): 2 go (speed != 0), 3 start_move, -1 none
};
constexpr DestResult setDestination(DestSprite& s, const DestVid& v, std::int32_t x, std::int32_t y, std::int32_t z) {
    DestResult r;
    s.state &= 0xfffc;
    const std::uint16_t st = s.state;
    std::int32_t dist = 0;
    if (v.speedX != 0) {
        const Direction d = getDirection(s.x, s.y, x, y);
        if (v.dirs == 1 || (v.flags & 1)) s.direction = static_cast<std::uint8_t>(d.direction);
        dist = r.gDist = d.distance;
        if (dist == 0) return r;  // nothing else changes
        s.state = st | 0x110;
    }
    s.goX = lo16(x); s.goY = lo16(y); s.goZ = lo16(z);
    s.fracX = s.fracY = s.fracZ = 0;
    if ((v.category == 0x10 || v.cls == 15) && dist != 0 && v.speedX != 0) {
        // ballistic shot (precondition: none of the divisions overflows, see docs/oracle.md)
        const std::int32_t q = idiv(mul32(s.goZ - s.z, v.speedX), dist);
        if (v.flags & 2) s.vz = lo16(add32(q, idiv(mul32(174, dist), v.speedX) >> 1));
        else s.vz = lo16(q);
        if (s.vz > v.speedY) s.vz = v.speedY;
        if (s.vz < -std::int32_t(v.speedY)) s.vz = lo16(-std::int32_t(v.speedY));
    } else {
        s.vz = s.goZ > s.z ? v.speedY : s.goZ < s.z ? lo16(-std::int32_t(v.speedY)) : 0;
    }
    if (s.x != s.goX || s.y != s.goY) r.changeAct = s.speed != 0 ? 2 : 3;
    return r;
}

// ---- physics step of SPRITE::Tact (0x409cae..0x40a116) ---------------------------------------
struct PhysicsVid {
    std::uint16_t flags = 0;        // 2 Gravity, 0x40 PresentOnGrid, 0x1000 Wind
    std::int16_t speedX = 0;
    std::uint16_t acceleration = 0; // 9999 = instantaneous
    std::int8_t rotationPeriod = 0; // value after loading (rotationPeriod*6/dirs)
};
struct PhysicsSprite {
    std::int16_t x = 0, y = 0, z = 0;
    std::uint8_t direction = 0;
    std::uint16_t state = 0;
    std::uint32_t lastTact = 0;
    std::int16_t speed = 0, vz = 0;
    std::int8_t fracX = 0, fracY = 0, fracZ = 0;
    std::uint8_t stepAccum = 0;
    std::int16_t goX = 0, goY = 0, goZ = 0;
};
struct PhysicsWorld {
    std::uint32_t curclock = 0;
    std::int32_t mapW = 0, mapH = 0;    // 0x428e0c / 0x428e10
    std::int16_t windStrength = 0;      // 0x439d54
    std::uint32_t windDir = 0;          // 0x429e14 (low byte used)
    std::uint8_t gridShiftX = 0, gridShiftY = 0;
    std::int16_t gridNoX = 0, gridNoY = 0;
};
struct GridCell { std::int32_t x = -1, y = -1; };
struct PhysicsResult {
    bool passive = false;               // state & 0x40: nothing done
    std::optional<std::int32_t> step;   // the "step" local used by the command logic (not set by
                                        // the speed-9999 beam path); 0x7fff when not moving
    std::optional<GridCell> cellRemoved, cellAdded;  // PresentOnGrid cell change (valid cells only)
};
// `action(act, v1, v2)`: the sprite's virtual Action (31 path_block, 38 path_limit); it may
// modify the sprite, which is re-read afterwards exactly like the original.
// `blocked(x, y, z) -> bool`: CheckMove 0x40a5c0 returns a sprite.
// `obstacle(goX, goY, goZ) -> uint32`: FirstObstacleOnLine 0x40a770 (0 or x | y << 16).
template <class ActionFn, class BlockedFn, class ObstacleFn>
constexpr PhysicsResult physicsStep(PhysicsSprite& s, const PhysicsVid& v, const PhysicsWorld& w,
                                    ActionFn&& action, BlockedFn&& blocked, ObstacleFn&& obstacle) {
    PhysicsResult r;
    if (s.state & 0x40) { r.passive = true; return r; }
    const std::uint32_t dt = w.curclock - s.lastTact;
    const bool gravity = v.flags & 2;
    if (gravity && s.z != 0)
        s.vz = static_cast<std::int16_t>(static_cast<std::uint16_t>(s.vz) - static_cast<std::uint16_t>((dt * 174u) >> 10));
    if (s.state & 0x100) {
        if (s.speed < v.speedX)
            s.speed = v.acceleration == 9999 ? v.speedX
                    : static_cast<std::int16_t>(static_cast<std::uint16_t>(((dt * v.acceleration) >> 10) + std::uint32_t(s.speed)));
    } else if (s.speed > 0) {
        if (v.acceleration == 9999) s.speed = 0;
        else {
            s.speed = static_cast<std::int16_t>(static_cast<std::uint16_t>(std::uint32_t(s.speed) - ((dt * v.acceleration) >> 9)));
            if (s.speed < 0) s.speed = 0;
        }
    }
    if (s.vz != 0) {
        const std::int32_t a = add32(mul32(wrap32(dt), s.vz) >> 3, s.fracZ);
        s.fracZ = static_cast<std::int8_t>(a & 0x7f);
        const std::int32_t dz = a >> 7;
        if (s.z + dz < 0 && s.z >= 0) action(31, 0, 0);
        else if (gravity) s.z = lo16(s.z + dz);
        else if (s.goZ > s.z) { if (s.goZ - s.z <= dz) { s.z = s.goZ; s.vz = 0; } else s.z = lo16(s.z + dz); }
        else if (s.goZ < s.z) { if (dz < s.goZ - s.z) { s.z = s.goZ; s.vz = 0; } else s.z = lo16(s.z + dz); }
        else s.vz = 0;
    }
    const bool windy = (v.flags & 0x1000) != 0;
    if (s.speed == 0 && !(w.windStrength != 0 && windy)) { r.step = 0x7fff; return r; }
    const std::int32_t gx0 = std::int32_t(s.x) >> (w.gridShiftX & 31), gy0 = std::int32_t(s.y) >> (w.gridShiftY & 31);
    if (s.speed == 9999) {
        const std::uint32_t p = obstacle(std::int32_t(s.goX), std::int32_t(s.goY), std::int32_t(s.goZ));
        if (p) { s.x = lo16(wrap32(p)); s.y = lo16(wrap32(p >> 16)); }
        else { s.x = s.goX; s.y = s.goY; }
        s.state |= 3;
        s.z = s.goZ;
    } else {
        const std::int32_t d = mul32(s.speed, wrap32(dt));
        std::int32_t ax = add32(mul32(kSin[s.direction], d) >> 13, s.fracX);
        std::int32_t ay = add32(mul32(kNegCos[s.direction], d) >> 13, s.fracY);
        // the step counts the wind displacement instead of the own speed for Wind vids (the
        // original reuses the register holding speed*dt for windStrength*dt)
        std::int32_t travelled = d;
        if (windy) {
            const std::int32_t wv = travelled = mul32(w.windStrength, wrap32(dt));
            const std::uint32_t wd = w.windDir & 0xff;
            ax = add32(ax, mul32(kSin[wd], wv) >> 13);
            ay = add32(ay, mul32(kNegCos[wd], wv) >> 13);
        }
        s.fracX = static_cast<std::int8_t>(ax & 0x7f);
        s.fracY = static_cast<std::int8_t>(ay & 0x7f);
        const std::int32_t step = add32(abs32(travelled) >> 10, s.stepAccum);
        s.stepAccum = static_cast<std::uint8_t>(step >= v.rotationPeriod ? step - v.rotationPeriod : step);
        r.step = step;
        const std::int32_t mx = ax >> 7, my = ay >> 7;
        const std::int32_t nx = s.x + mx, ny = s.y + my;
        if (s.state & 0x10) {
            if ((s.goX >= s.x && s.goX <= nx) || (s.goX >= nx && s.goX <= s.x)) s.state |= 1;
            if ((s.goY >= s.y && s.goY <= ny) || (s.goY >= ny && s.goY <= s.y)) s.state |= 2;
        }
        if (nx < 0 || ny < 0 || nx > w.mapW || ny > w.mapH) action(38, mx, my);
        else if (blocked(nx, ny, std::int32_t(s.z))) action(31, mx, my);
        else { s.x = lo16(s.x + mx); s.y = lo16(s.y + my); }
    }
    if (v.flags & 0x40) {
        const std::int32_t gx1 = std::int32_t(s.x) >> (w.gridShiftX & 31), gy1 = std::int32_t(s.y) >> (w.gridShiftY & 31);
        if (gx0 != gx1 || gy0 != gy1) {
            if (gx0 >= 0 && gy0 >= 0 && gx0 < w.gridNoX && gy0 < w.gridNoY) r.cellRemoved = GridCell{gx0, gy0};
            if (gx1 >= 0 && gy1 >= 0 && gx1 < w.gridNoX && gy1 < w.gridNoY) r.cellAdded = GridCell{gx1, gy1};
        }
    }
    return r;
}

// ---- spatial grid query (0x40d6d0 / 0x40d7a0) ------------------------------------------------
struct GridSpec {
    std::uint8_t shiftX = 0, shiftY = 0;  // cell size = 1 << shift
    std::int16_t noX = 0, noY = 0;        // number of cells
};
struct GridRange { std::int32_t x0 = 0, y0 = 0, x1 = -1, y1 = -1; };  // inclusive, rows y0..y1 outer
// The rectangle's cells widened by one cell on every side, clamped to the grid (16-bit math).
constexpr GridRange gridQuery(const GridSpec& g, std::int32_t x1, std::int32_t y1, std::int32_t x2, std::int32_t y2) {
    GridRange r;
    r.x0 = lo16((x1 >> (g.shiftX & 31)) - 1); if (r.x0 < 0) r.x0 = 0;
    r.x1 = lo16((x2 >> (g.shiftX & 31)) + 1); if (r.x1 >= g.noX) r.x1 = g.noX - 1;
    r.y0 = lo16((y1 >> (g.shiftY & 31)) - 1); if (r.y0 < 0) r.y0 = 0;
    r.y1 = lo16((y2 >> (g.shiftY & 31)) + 1); if (r.y1 >= g.noY) r.y1 = g.noY - 1;
    return r;
}

// ---- collisions: CheckMove 0x40a5c0 ----------------------------------------------------------
struct CollisionVid {
    std::uint8_t category = 0;      // 1 Terrain, 4 Vehicle, 8 Avia, 0x10 Cannon
    std::uint16_t flags = 0;        // 0x20 NoCollision, 0x4000 CrushOnCollision
    std::uint8_t collisionMask = 0;
    std::int16_t sizeZ = 0, halfX = 0, halfY = 0, absSizeZ = 0;
};
struct CollisionObject {
    std::int16_t x = 0, y = 0, z = 0;
    std::uint8_t action = 0;
    CollisionVid vid;
    bool isSelf = false;            // the moving sprite itself, met in the grid
};
inline constexpr int kBlockedBySelf = -2;  // z < 0: CheckMove returns `this`
// `candidates` in grid iteration order (gridQuery of the moving sprite's box around (x, y)).
// Returns the index of the blocking object, -1 if free, kBlockedBySelf if z < 0.
// `crush(index)`: Action(act_damage, 2, 0) sent to a CrushOnCollision object in the way.
template <class CrushFn>
constexpr int checkMove(const CollisionVid& self, std::int32_t x, std::int32_t y, std::int32_t z,
                        std::span<const CollisionObject> candidates, CrushFn&& crush) {
    if (self.category != 4 && self.category != 8 && self.category != 0x10) return -1;
    if (z < 0) return kBlockedBySelf;
    if (self.flags & 0x20) return -1;
    for (int i = 0; i < int(candidates.size()); ++i) {
        const CollisionObject& s = candidates[i];
        const CollisionVid& o = s.vid;
        if ((o.flags & 0x20) || s.action == 15) continue;
        if (abs32(x - s.x) >= o.halfX + self.halfX) continue;
        if (abs32(y - s.y) >= o.halfY + self.halfY) continue;
        if ((o.category & 1) && z == 0) {
            if (!(o.collisionMask & self.collisionMask)) return i;
            continue;
        }
        if (s.isSelf) continue;
        if (abs32(add32(sub32(mul32(sub32(z, s.z), 2), o.sizeZ), self.sizeZ)) >= o.absSizeZ + self.absSizeZ) continue;
        if (o.flags & 0x4000) { crush(i); continue; }
        return i;
    }
    return -1;
}
// The query rectangle CheckMove uses.
constexpr GridRange checkMoveQuery(const GridSpec& g, const CollisionVid& self, std::int32_t x, std::int32_t y) {
    return gridQuery(g, x - self.halfX, y - self.halfY, x + self.halfX, y + self.halfY);
}

// ---- damage (material base Action 0x412580) --------------------------------------------------
struct DamageResult {
    std::uint8_t hp = 0;
    int changeAct = -1;              // ChangeAction(act, -1): 15 death, 13 wound, -1 none
    bool woundChildRemoved = false;  // 0x40c0e0(vid.childNvid[13]): back above half (repair)
};
// act_damage(n) (43). Negative n heals (8-bit wrap). n >= hp kills without changing hp.
constexpr DamageResult applyDamage(std::uint8_t maxHP, std::uint8_t hp, std::int32_t n) {
    DamageResult r{hp, -1, false};
    if (maxHP == 0) return r;
    const std::uint8_t half = maxHP >> 1;
    const bool wasAbove = half < hp;
    if (n >= hp) { r.changeAct = 15; return r; }
    r.hp = static_cast<std::uint8_t>(hp - n);
    if (r.hp > half && !wasAbove) r.woundChildRemoved = true;
    if (r.hp <= half && wasAbove) r.changeAct = 13;
    return r;
}
// Area damage at the end of the death animation (act_death 15, 0x412825).
struct Exploder {
    std::int16_t x = 0, y = 0, z = 0;
    std::uint8_t deathDamage = 0;
    std::int16_t deathDamageRadius = 0;  // the u16 field is read with movsx
    std::int16_t halfX = 0, halfY = 0;
};
struct DamageTarget {
    std::int16_t x = 0, y = 0, z = 0;
    std::uint8_t maxHP = 0;
    std::int16_t halfX = 0, halfY = 0, absSizeZ = 0;
    bool isSelf = false;
};
constexpr GridRange areaDamageQuery(const GridSpec& g, const Exploder& e) {
    const std::int32_t rx = e.deathDamageRadius + e.halfX, ry = e.deathDamageRadius + e.halfY;
    return gridQuery(g, e.x - rx, e.y - ry, e.x + rx, e.y + ry);
}
// Returns false when the vid has no deathDamage (nothing happens). Otherwise the exploding
// sprite's hp becomes 0 and `hit(index, damage)` = Action(act_damage, damage, this) for every
// candidate (grid iteration order) in range: |dx| < radius + halfX + o.halfX (same for y) and
// |dz| < o.absSizeZ + 20, with maxHP != 0. No attenuation.
template <class HitFn>
constexpr bool areaDamage(const Exploder& e, std::span<const DamageTarget> candidates, HitFn&& hit) {
    if (!e.deathDamage) return false;
    const std::int32_t rx = e.deathDamageRadius + e.halfX, ry = e.deathDamageRadius + e.halfY;
    for (int i = 0; i < int(candidates.size()); ++i) {
        const DamageTarget& t = candidates[i];
        if (t.isSelf || !t.maxHP) continue;
        if (abs32(e.x - t.x) >= t.halfX + rx) continue;
        if (abs32(e.y - t.y) >= t.halfY + ry) continue;
        if (abs32(e.z - t.z) >= t.absSizeZ + 20) continue;
        hit(i, e.deathDamage);
    }
    return true;
}

// ---- command stack step of SPRITE::Tact (0x40a116..0x40a201) ---------------------------------
struct Command {
    std::uint8_t act = 0;
    std::int32_t var1 = 0, var2 = 0;
};
struct CommandStepResult {
    int kind = 0;               // 0 nothing, 1 virtual Action(act, v1, v2), 2 ChangeAction(act, -1)
    int act = -1;
    std::int32_t v1 = 0, v2 = 0;
    bool freed = false;         // the stack became empty and was freed (commands = 0)
};
// Runs only when `frame > lastFrame || action < 6` (the sprite *action*, not the vid class).
// `entries`/`count`: the stack records (capacity) and the s16 count, top = entries[count-1];
// `hasStack` = commands pointer != 0. `step` = the physics step local (see physicsStep).
// A command is popped when the sprite is free (state & 0xf0 == 0) or busy but not moving with
// act_submove (74) on top; act_cyclecommand (61) sets count = var1 and runs entries[var1].
// Otherwise act_nextcommand (45) is sent when both arrival bits are set, or when the action
// is not go/l_rotate/r_rotate (2/4/5), or when step >= rotationPeriod.
// The caller performs the returned action (the original calls it before checking count == 0).
constexpr CommandStepResult commandStep(std::int16_t frame, std::int16_t lastFrame, std::uint8_t action,
                                        std::uint16_t state, std::int32_t step, std::int8_t rotationPeriod,
                                        std::span<const Command> entries, std::int16_t& count, bool& hasStack) {
    CommandStepResult r;
    if (!(frame > lastFrame) && action >= 6) return r;
    if (hasStack && (!(state & 0xf0) || (!(state & 0x10) && entries[count - 1].act == 74))) {
        count = static_cast<std::int16_t>(count - 1);
        if (entries[count].act == 61) count = lo16(entries[count].var1);
        const Command& c = entries[count];
        if (c.act > 15) { r.kind = 1; r.act = c.act; r.v1 = c.var1; r.v2 = c.var2; }
        else { r.kind = 2; r.act = c.act; }
        if (count == 0) { hasStack = false; r.freed = true; }
        return r;
    }
    if ((state & 3) == 3 || !(action == 2 || action == 4 || action == 5) || step >= rotationPeriod) {
        r.kind = 1; r.act = 45;
    }
    return r;
}

} // namespace gromada::game::ref
