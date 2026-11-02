#pragma once
// Integer math of gromada.exe used by the simulation (docs/re/engine.md §4).
// Pure functions, exposed for unit tests and for the differential oracle (issue #11).
// Every function cites the original address.

#include <array>
#include <cstdint>
#include <cstdlib>

namespace gromada::game::fx {

// 0x428e14 SIN[i] = (int)(1024*sin(2*pi*i/256)), 0x429214 NEG_COS[i] = -(int)(1024*cos(...)).
// Embedded as a quarter wave (verified against the exe tables by tools/re/engine_reference.py).
inline constexpr std::array<std::int16_t, 65> SinQuarter = {
    0, 25, 50, 75, 100, 125, 150, 175, 199, 224, 248, 273, 297, 321, 344, 368, 391, 414,
    437, 460, 482, 504, 526, 547, 568, 589, 609, 629, 649, 668, 687, 706, 724, 741, 758,
    775, 791, 807, 822, 837, 851, 865, 878, 890, 903, 914, 925, 936, 946, 955, 964, 972,
    979, 986, 993, 999, 1004, 1008, 1012, 1016, 1019, 1021, 1022, 1023, 1024};

constexpr int sinEntry(int i) noexcept {
    i &= 255;
    if (i <= 64) return SinQuarter[i];
    if (i <= 128) return SinQuarter[128 - i];
    return -sinEntry(i - 128);
}

inline constexpr std::array<int, 256> Sin = [] {
    std::array<int, 256> t{};
    for (int i = 0; i < 256; ++i) t[i] = sinEntry(i);
    return t;
}();
inline constexpr std::array<int, 256> NegCos = [] {
    std::array<int, 256> t{};
    for (int i = 0; i < 256; ++i) t[i] = -sinEntry(i + 64);
    return t;
}();

// 0x429e18: CORDIC angles in 1/512 of a turn.
inline constexpr std::array<int, 7> CordicAngles = {64, 37, 19, 10, 5, 2, 1};

inline constexpr int Gravity = 174;        // 0x409ccb: vz -= 174*dt >> 10
inline constexpr int ClockStepCapMs = 71;  // 0x401f70
inline constexpr int Instant = 9999;       // speed/acceleration "instantaneous"

struct DirectionResult {
    int raw = 0;       // angle >> 1 (callers keep the low byte)
    int distance = 0;  // global 0x439d4c: (x * 636750) >> 20
    [[nodiscard]] constexpr std::uint8_t dir() const noexcept { return static_cast<std::uint8_t>(raw & 0xff); }
};

// 0x40b030 GetDirection(x1,y1,x2,y2): 7-step CORDIC; direction 0 = north, 64 = east.
// 32-bit x86 arithmetic (two's complement wraparound, like the original's registers).
constexpr std::int32_t wrapAdd(std::int32_t a, std::int32_t b) noexcept {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}
constexpr std::int32_t wrapSub(std::int32_t a, std::int32_t b) noexcept {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b));
}
constexpr std::int32_t wrapAbs(std::int32_t a) noexcept { return a < 0 ? wrapSub(0, a) : a; }   // abs(INT_MIN) = INT_MIN

// 0x40b030 GetDirection(x1,y1,x2,y2): 7-step CORDIC; direction 0 = north, 64 = east. The
// arguments are full int32 (script coordinates): differences wrap like the original.
constexpr DirectionResult getDirection(int x1, int y1, int x2, int y2) noexcept {
    std::int32_t x = wrapSub(x2, x1), y = wrapSub(y2, y1);
    int angle;
    if (x < 0) { x = wrapSub(0, x); y = wrapSub(0, y); angle = 0x180; } else angle = 0x80;
    for (int i = 0; i < 7; ++i) {
        const std::int32_t ox = x;
        if (y >= 0) { x = wrapAdd(x, y >> i); y = wrapSub(y, ox >> i); angle += CordicAngles[i]; }
        else        { x = wrapSub(x, y >> i); y = wrapAdd(y, ox >> i); angle -= CordicAngles[i]; }
    }
    const auto prod = static_cast<std::int32_t>(static_cast<std::uint32_t>(x) * 0x9b74eu);
    return {angle >> 1, prod >> 20};
}

// 0x40aff0 / 0x40afc0: the "game" distance max(|dx|, 3|dy| >> 1) used for ranges, visibility,
// squads, with the original's 32-bit wraparound (0x40afc0 takes an int32 point).
constexpr int gameDistance(int dx, int dy) noexcept {
    const std::int32_t ax = wrapAbs(dx);
    const std::int32_t ay = static_cast<std::int32_t>(static_cast<std::uint32_t>(wrapAbs(dy)) * 3u) >> 1;
    return ax < ay ? ay : ax;
}

// 0x40bc55: tick period of a sprite; `rand15` is consumed only for Randomized vids.
constexpr std::uint32_t tickPeriod(std::uint32_t frameDuration, bool randomized, int rand15) noexcept {
    std::uint32_t p = frameDuration;
    if (randomized) p += (p >> 3) - static_cast<std::uint32_t>(rand15) % ((p >> 2) + 1);
    return p;
}

// 0x409d02..0x409d7c: speed update (accelerating flag = state bit 0x100).
constexpr std::int16_t integrateSpeed(std::int16_t speed, int accel, std::int16_t maxSpeed, std::uint32_t dt,
                                      bool accelerating) noexcept {
    if (accelerating) {
        if (speed < maxSpeed)
            speed = accel == Instant ? maxSpeed
                                     : static_cast<std::int16_t>(speed + ((dt * static_cast<std::uint32_t>(accel)) >> 10));
    } else if (speed > 0) {
        if (accel == Instant) speed = 0;
        else {
            speed = static_cast<std::int16_t>(speed - ((dt * static_cast<std::uint32_t>(accel)) >> 9));
            if (speed < 0) speed = 0;
        }
    }
    return speed;
}

// 0x409cc5: gravity (only applied when z != 0).
constexpr std::int16_t applyGravity(std::int16_t vz, std::uint32_t dt) noexcept {
    return static_cast<std::int16_t>(vz - static_cast<std::int16_t>((dt * Gravity) >> 10));
}

struct AxisStep {
    int delta;           // whole pixels
    std::int8_t frac;    // new 7-bit accumulator
};

// 0x409d89: vertical integration a = ((vz*dt) >> 3) + fracZ.
constexpr AxisStep verticalStep(std::int16_t vz, std::uint32_t dt, std::int8_t fracZ) noexcept {
    const std::int32_t a = (static_cast<std::int32_t>(dt * static_cast<std::uint32_t>(static_cast<std::int32_t>(vz))) >> 3) + fracZ;
    return {a >> 7, static_cast<std::int8_t>(a & 0x7f)};
}

struct MoveStep {
    int mx, my;
    std::int8_t fracX, fracY;
    int step;  // |speed*dt| >> 10 (|windStrength*dt| >> 10 for Wind vids), before adding stepAccum
};

// 0x409e7c..0x409f3e: horizontal move with 7-bit accumulators, plus the wind contribution.
constexpr MoveStep moveStep(std::int16_t speed, std::uint32_t dt, std::uint8_t dir, std::int8_t fracX, std::int8_t fracY,
                            int windStrength = 0, std::uint8_t windDir = 0, bool wind = false) noexcept {
    const auto d = static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::int32_t>(speed)) * dt);
    std::int32_t ax = (static_cast<std::int32_t>(static_cast<std::uint32_t>(d) * static_cast<std::uint32_t>(Sin[dir])) >> 13) + fracX;
    std::int32_t ay = (static_cast<std::int32_t>(static_cast<std::uint32_t>(d) * static_cast<std::uint32_t>(NegCos[dir])) >> 13) + fracY;
    // The step counts |speed*dt| >> 10, except for Wind vids: there the original reuses the
    // register for windStrength*dt (0x409ee6), so the step is the wind's (0 without wind) and
    // not the sprite's own (docs/oracle.md §3.3).
    std::int32_t travelled = d;
    if (wind) {
        const auto w = static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(windStrength))) * dt);
        ax = wrapAdd(ax, static_cast<std::int32_t>(static_cast<std::uint32_t>(w) * static_cast<std::uint32_t>(Sin[windDir])) >> 13);
        ay = wrapAdd(ay, static_cast<std::int32_t>(static_cast<std::uint32_t>(w) * static_cast<std::uint32_t>(NegCos[windDir])) >> 13);
        travelled = w;
    }
    return {ax >> 7, ay >> 7, static_cast<std::int8_t>(ax & 0x7f), static_cast<std::int8_t>(ay & 0x7f), wrapAbs(travelled) >> 10};
}

// 0x40cf70: rotation step tables 0x431bf4 (one step counter-clockwise, act_l_rotate) and
// 0x435bf4 (one step clockwise, act_r_rotate), indexed [dirs][direction].
constexpr std::uint8_t rotateStep(int dirs, std::uint8_t dir, bool clockwise) noexcept {
    int q;
    int div;
    if (dirs <= 1) {  // the dirs==1 row is filled by a separate loop with a factor 32
        q = ((dir + 4) & 0xff) >> 3;
        div = 32;
    } else {
        q = (((dir + 128 / dirs) & 0xff) * dirs) >> 8;
        div = dirs;
    }
    const int v = clockwise ? (q * 256 + 256) / div : (q * 256 - 256) / div;
    return static_cast<std::uint8_t>(v & 0xff);
}

// Quantised direction index used by SetAnimation, TurnTo and MenuDir.
constexpr int directionIndex(int dirs, std::uint8_t dir) noexcept {
    const int half = (static_cast<std::uint8_t>(256 / (dirs ? dirs : 1))) >> 1;  // vid+0x127 is a byte
    return (((dir + half) & 0xff) * dirs) >> 8;
}

// 0x414f38: perspective-scaled tables used to rotate child offsets (scaleY/scaleX, 16/24).
struct PerspectiveTables {
    std::array<int, 256> sinS{}, negCosS{};
    void build(int scaleX, int scaleY) noexcept {
        for (int i = 0; i < 256; ++i) {
            sinS[i] = scaleX ? (scaleY * Sin[i]) / scaleX : 0;
            negCosS[i] = scaleX ? (scaleY * NegCos[i]) / scaleX : 0;
        }
    }
};

} // namespace gromada::game::fx
