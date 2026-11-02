#pragma once
// Input module, part 2: control schemes. Turns RawInput into PcState once per frame.
// Pure logic (no libretro calls), so it is unit-tested directly.
//
// Schemes (core option):
//  - VirtualCursor: left stick moves the cursor (response curve + acceleration), right stick
//    moves it slowly for precision; A = left click, B = right click.
//  - Direct (twin stick): the left stick drives the Kassandra with move orders the original
//    understands, a left click at a point ahead of it in the stick direction (renewed while
//    the stick is held; a short order at its feet when released stops it). The arrows of the
//    original are tank controls (forward/back/turn while moving) and stay on the d-pad. The
//    right stick aims: the cursor is the aim point, on the best target in the stick direction
//    (aim assist) or at weapon range; pushing past fireThreshold holds the right button
//    (attack). With the right stick idle the aim follows the movement (the turret looks ahead).
// Common: L1/R1 = previous/next available weapon (keys 1-4, repeated until the game reports
// the switch: the original refuses a change while the turret is still loading), Start = Esc,
// Select = TAB, X = Enter, d-pad = arrows. Touchscreen: tap = left click, long press or two
// fingers = right click, drag = left button held. Mouse: relative cursor + buttons, wheel =
// weapon cycling.
// Contexts (set by the session every frame): Menu forces the cursor scheme (menus need a
// pointer), TextEntry turns the pad into a character picker for the player's name.
//
// All speeds are per second and integrated exactly over the frame, so 60 and 120 fps give
// the same motion for the same input; timers (orders, repeats, long press) are in seconds.

#include "input/pc_state.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <span>
#include <string_view>

namespace gromada::input {

enum class Scheme : std::uint8_t { VirtualCursor, Direct };
enum class Context : std::uint8_t { Gameplay, Menu, TextEntry };
// View following the player (applied by the game session): Auto = direct scheme and touch,
// where the cursor cannot scroll at the screen edges.
enum class CameraFollow : std::uint8_t { Auto, Always, Off };

struct Settings {
    Scheme scheme = Scheme::VirtualCursor;
    // Cursor (virtual cursor scheme, menus)
    float cursorSpeed = 500;   // px/s at full deflection, before acceleration
    float acceleration = 2.0f; // speed multiplier reached holding the stick at the rim (1 = off)
    float accelDelay = 0.25f;  // s at the rim before acceleration starts
    float accelRamp = 0.6f;    // s from 1x to the full multiplier
    float curve = 2.0f;        // response exponent: higher = finer control near the center
    float deadzone = 0.15f;    // radial, on the raw stick
    float preciseFactor = 0.3f; // right stick speed relative to cursorSpeed (linear, no acceleration)
    // Direct scheme
    float moveThreshold = 0.3f;  // left stick deflection that drives
    float moveDistance = 120;    // px: move orders point this far ahead of the player
    float minOrderDistance = 48; // px: never closer (the original arrives within 24 x 16 px)
    float orderInterval = 0.2f;  // s: a held stick renews its order this often
    float orderAngle = 0.14f;    // rad (8 degrees): turning the stick gives a new order at once
    float stopDistance = 3;      // px: releasing the stick orders a move this close (= stop)
    float aimThreshold = 0.15f;  // right stick: aim (cursor moves, no attack)
    float fireThreshold = 0.7f;  // right stick: aim and hold the right button
    float aimDistance = 160;     // aim point distance when the engine gives no weapon range
    bool aimAssist = true;       // the aim point snaps to the best target in the aim direction
    float assistCone = 0.45f;    // rad (26 degrees) each side of the aim direction
    // Weapons: a request is repeated until the game reports the switch (FrameInfo::currentWeapon)
    float weaponRetry = 0.12f, weaponTimeout = 1.5f;
    CameraFollow camera = CameraFollow::Auto;
    // Pointing devices
    bool touch = true, mouse = true, keyboard = false;
    float longPress = 0.45f;     // s without moving: right click
    float touchSlop = 10;        // px a finger may wander before a tap becomes a drag
    float touchMouseGrace = 0.35f; // s: ignore the mouse after touches (Android emulates a mouse from taps)
    float mouseScale = 1;
    // Name entry with the pad
    float repeatDelay = 0.4f, repeatRate = 12;
};

// The engine tells the input module where the player is on screen (direct scheme aims and
// drives from there). The viewer has no player and uses the screen center.
struct PlayerPosition {
    virtual ~PlayerPosition() = default;
    [[nodiscard]] virtual std::optional<Vec2> screenPosition() const = 0;
};

// Something the player can shoot at (aim assist), in screen pixels.
struct AimTarget {
    Vec2 point;   // where the cursor selects it (the original attacks the unit under the cursor)
    Vec2 ground;  // its position on the ground, for distances (same space as the player position)
};

struct FrameInfo {
    float dt = 1.0f / 60;   // seconds since the previous frame
    int width = 640, height = 480;
    Context context = Context::Gameplay;
    const PlayerPosition* player = nullptr; // nullptr or nullopt: screen center
    std::uint8_t weaponMask = 0x0F;         // bit i: weapon i+1 available
    int currentWeapon = -1;                 // 0..3: weapon the game has selected; -1 unknown
    // > 0 overrides Settings::aimDistance: weapon range in the game's metric max(|dx|, 1.5|dy|)
    float aimDistance = 0;
    std::span<const AimTarget> targets;     // attackable units (direct scheme aim assist)
};

inline constexpr std::string_view TextEntryCharset = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -";

// Radial deadzone with rescale: returns direction (unit) and magnitude in 0..1.
struct StickPolar {
    Vec2 dir;
    float mag = 0;
};
inline StickPolar radial(Vec2 v, float deadzone) {
    const float len = v.length();
    if (len <= deadzone || len == 0)
        return {};
    const float mag = std::min(1.0f, (len - deadzone) / (1.0f - deadzone));
    return {{v.x / len, v.y / len}, mag};
}

// Distance in the game's metric (fx::gameDistance: y counts 1.5 times, perspective).
inline float gameDistance(Vec2 v) { return std::max(std::abs(v.x), 1.5f * std::abs(v.y)); }

// Largest t >= 0 such that p + d * t stays inside [m, w-1-m] x [m, h-1-m].
inline float rayToBorder(Vec2 p, Vec2 d, float w, float h, float m) {
    float t = 1e9f;
    if (d.x > 1e-6f) t = std::min(t, (w - 1 - m - p.x) / d.x);
    if (d.x < -1e-6f) t = std::min(t, (m - p.x) / d.x);
    if (d.y > 1e-6f) t = std::min(t, (h - 1 - m - p.y) / d.y);
    if (d.y < -1e-6f) t = std::min(t, (m - p.y) / d.y);
    return std::max(0.0f, t);
}

// Aim assist: index of the target closest to the aim direction `dir` (unit) from `from`,
// within `cone` radians and `range` (game metric, 0 = any), on screen; -1 if none. Small
// angles win, distance breaks near-ties (a quarter of the range costs as much as 0.1 rad).
inline int pickTarget(std::span<const AimTarget> targets, Vec2 from, Vec2 dir, float cone, float range, Vec2 screen) {
    int best = -1;
    float bestScore = 1e9f;
    for (std::size_t i = 0; i < targets.size(); ++i) {
        const AimTarget& t = targets[i];
        if (t.point.x < 0 || t.point.y < 0 || t.point.x >= screen.x || t.point.y >= screen.y)
            continue;
        const Vec2 v = t.ground - from;
        const float len = v.length();
        if (len < 1)
            continue;
        if (range > 0 && gameDistance(v) > range + 8)
            continue;
        const float c = std::clamp((v.x * dir.x + v.y * dir.y) / len, -1.0f, 1.0f);
        const float angle = std::acos(c);
        if (angle > cone)
            continue;
        const float score = angle + 0.4f * (range > 0 ? gameDistance(v) / range : len / 640);
        if (score < bestScore) {
            bestScore = score;
            best = static_cast<int>(i);
        }
    }
    return best;
}

// RTS-style edge scrolling helper: -1..1 per axis when the cursor is within `zone` px of
// a screen border (full speed on the border itself).
inline Vec2 edgeScroll(Vec2 cursor, int width, int height, float zone = 16) {
    auto axis = [zone](float c, int size) {
        if (c < zone) return -std::clamp((zone - c) / zone, 0.0f, 1.0f);
        const float far = static_cast<float>(size - 1) - zone;
        if (c > far) return std::clamp((c - far) / zone, 0.0f, 1.0f);
        return 0.0f;
    };
    return {axis(cursor.x, width), axis(cursor.y, height)};
}

class Mapper {
public:
    explicit Mapper(const Settings& s = {}) : m_settings(s) {}

    void configure(const Settings& s) {
        if (s.scheme != m_settings.scheme) {
            m_aimOffset = {};
            m_aimFree = false;
            m_driving = false;
            m_accelHold = 0;
        }
        m_settings = s;
    }
    [[nodiscard]] const Settings& settings() const { return m_settings; }
    [[nodiscard]] const PcState& state() const { return m_state; }

    void warpCursor(Vec2 p) { m_state.cursor = p; m_cursorInit = true; }
    // The engine reports weapon changes it makes itself (pick-ups, empty ammo). With
    // FrameInfo::currentWeapon the mapper follows the game by itself.
    void setWeaponSlot(int slot) {
        m_state.weaponSlot = std::clamp(slot, 0, 3);
        m_weaponPending = false;
    }

    // Cursor displacement factor for holding the stick at the rim from time t0 to t1
    // (seconds since the hold started): integral of the speed multiplier, exact.
    [[nodiscard]] float accelIntegral(float t0, float t1) const {
        const float d = m_settings.accelDelay, r = std::max(1e-4f, m_settings.accelRamp);
        const float extra = std::max(0.0f, m_settings.acceleration - 1);
        auto F = [&](float t) {
            float g;
            if (t <= d) g = 0;
            else if (t <= d + r) g = (t - d) * (t - d) / (2 * r);
            else g = r / 2 + (t - d - r);
            return t + extra * g;
        };
        return F(t1) - F(t0);
    }

    const PcState& update(const RawInput& in, const FrameInfo& f) {
        const float dt = std::max(0.0f, f.dt);
        const Settings& s = m_settings;
        const Vec2 screen{static_cast<float>(f.width), static_cast<float>(f.height)};
        if (!m_cursorInit) {
            m_state.cursor = screen * 0.5f;
            m_cursorInit = true;
        }
        const Vec2 player = playerPos(f);
        const Scheme scheme = f.context == Context::Menu ? Scheme::VirtualCursor : s.scheme;
        const bool direct = scheme == Scheme::Direct && f.context == Context::Gameplay;
        const bool textEntry = f.context == Context::TextEntry;
        const auto edge = [&](unsigned id) { return in.has(id) && !m_prevPad.has(id); };

        bool leftDown = false, rightDown = false;
        std::array<bool, KeyCount> keyDown{};
        auto hold = [&](Key k, bool on = true) { keyDown[static_cast<std::size_t>(k)] |= on; };
        m_state.text.clear();
        m_state.move = {};
        m_state.aiming = false;
        m_state.aimLocked = false;
        m_state.moveOrder = false;
        if (!direct)
            m_driving = false;

        followWeapon(f, dt);

        // --- RetroPad -----------------------------------------------------------
        hold(Key::Escape, in.has(textEntry ? pad::Select : pad::Start));
        if (textEntry) {
            updateTextEntry(in, dt);
            if (edge(pad::A) || edge(pad::Right)) m_state.text += m_state.textCandidate;
            if (edge(pad::B) || edge(pad::Left)) pulse(Key::Backspace);
            hold(Key::Enter, in.has(pad::Start));
        } else {
            leftDown |= in.has(pad::A);
            rightDown |= in.has(pad::B);
            hold(Key::Tab, in.has(pad::Select));
            hold(Key::Enter, in.has(pad::X));
            hold(Key::Left, in.has(pad::Left));
            hold(Key::Right, in.has(pad::Right));
            hold(Key::Up, in.has(pad::Up));
            hold(Key::Down, in.has(pad::Down));
            if (edge(pad::L)) cycleWeapon(-1, f.weaponMask);
            if (edge(pad::R)) cycleWeapon(+1, f.weaponMask);
        }
        const Vec2 dpad{static_cast<float>(in.has(pad::Right)) - in.has(pad::Left),
                        static_cast<float>(in.has(pad::Down)) - in.has(pad::Up)};

        std::optional<Vec2> order; // direct scheme: move order wanted this frame
        Vec2 orderDir;
        if (direct) {
            // Left stick: drive. A new order when the stick starts, turns or every
            // orderInterval; on release an order at the player's feet stops the Kassandra.
            m_orderClock += dt;
            const auto ls = radial(in.leftStick, s.deadzone);
            if (ls.mag > s.moveThreshold) {
                m_state.move = ls.dir * ls.mag;
                const bool turned = ls.dir.x * m_orderDir.x + ls.dir.y * m_orderDir.y < std::cos(s.orderAngle);
                if (!m_driving || turned || m_orderClock >= s.orderInterval) {
                    const float room = rayToBorder(player, ls.dir, screen.x, screen.y, 8);
                    const float dist = std::max(s.minOrderDistance, std::min(s.moveDistance, room));
                    order = player + ls.dir * dist;
                    orderDir = ls.dir;
                }
            } else {
                if (m_driving) {
                    order = player + m_orderDir * s.stopDistance;
                    orderDir = {};
                }
                if (dpad != Vec2{})
                    m_state.move = dpad * (1.0f / dpad.length());
            }
            // Right stick: aim, then fire. Idle: the aim follows the movement.
            const auto rs = radial(in.rightStick, s.deadzone);
            if (rs.mag > s.aimThreshold) {
                m_aimDir = rs.dir;
                m_aimFree = false;
                m_state.aiming = true;
                m_state.source = PointerSource::Pad;
                rightDown |= rs.mag >= s.fireThreshold;
            } else if (ls.mag > s.moveThreshold) {
                m_aimDir = ls.dir;
                m_aimFree = false;
            }
            m_state.cursor = aimPoint(player, f, screen);
        } else {
            if (!textEntry && dpad != Vec2{})
                m_state.move = dpad * (1.0f / dpad.length());
            moveCursorWithSticks(in, dt);
        }

        // --- touchscreen ------------------------------------------------------------
        bool pointerMoved = false; // touch or mouse placed the cursor this frame
        if (s.touch) {
            updateTouch(in, dt, screen, leftDown, rightDown);
            pointerMoved = in.touchCount > 0;
        }
        if (!s.touch)
            m_touch = {};

        // --- mouse --------------------------------------------------------------------
        if (in.touchCount > 0 && s.touch)
            m_touchGrace = s.touchMouseGrace;
        else
            m_touchGrace = std::max(0.0f, m_touchGrace - dt);
        if (s.mouse && m_touchGrace <= 0) {
            if (in.mouseDelta != Vec2{}) {
                m_state.cursor = m_state.cursor + in.mouseDelta * s.mouseScale;
                m_state.source = PointerSource::Mouse;
                pointerMoved = true;
            }
            leftDown |= in.mouseLeft;
            rightDown |= in.mouseRight;
            if (in.mouseWheel && !textEntry) cycleWeapon(in.mouseWheel > 0 ? -1 : +1, f.weaponMask);
        }

        // --- keyboard -------------------------------------------------------------------
        if (s.keyboard) {
            for (std::size_t k = 0; k < KeyCount; ++k)
                keyDown[k] |= in.keys[k];
            for (int k = 0; k < 4; ++k)
                if (in.keys[k] && !m_prevKeys[k]) {
                    m_state.weaponSlot = k;
                    m_weaponPending = false;
                }
            for (std::size_t c = 0; c < in.chars.size(); ++c)
                if (in.chars[c] && !m_prevChars[c])
                    m_state.text += static_cast<char>(in.shift ? std::toupper(static_cast<int>(c)) : c);
            m_prevKeys = in.keys;
            m_prevChars = in.chars;
        } else {
            m_prevKeys = {};
            m_prevChars.reset();
        }

        // --- finalize ------------------------------------------------------------------
        m_state.cursor.x = std::clamp(m_state.cursor.x, 0.0f, screen.x - 1);
        m_state.cursor.y = std::clamp(m_state.cursor.y, 0.0f, screen.y - 1);
        if (direct && pointerMoved) {
            // touch/mouse re-aim: keep that exact point (relative to the player)
            m_aimOffset = m_state.cursor - player;
            m_aimFree = true;
        }
        m_state.edgeScroll = !direct && m_state.source != PointerSource::Touch;

        const bool leftPressed = (leftDown || m_leftPulse) && !m_state.left.down;
        const bool rightPressed = (rightDown || m_rightPulse) && !m_state.right.down;
        // A move order is a left click somewhere else than the cursor: it cannot share the
        // frame with a real click (that one wins; the order waits for the next frame).
        if (order && !leftPressed && !rightPressed) {
            m_state.moveOrder = true;
            m_state.moveTarget = *order;
            m_driving = orderDir != Vec2{};
            m_orderDir = m_driving ? orderDir : m_orderDir;
            m_orderClock = 0;
        }

        setButton(m_state.left, leftDown || m_leftPulse);
        setButton(m_state.right, rightDown || m_rightPulse);
        m_leftPulse = m_rightPulse = false;
        for (std::size_t k = 0; k < KeyCount; ++k) {
            setButton(m_state.keys[k], keyDown[k] || m_keyPulse[k]);
            m_keyPulse[k] = false;
        }
        m_state.weaponPending = m_weaponPending;
        m_prevPad = in;
        return m_state;
    }

private:
    struct TouchTracker {
        enum Phase : std::uint8_t { Idle, Pending, Drag, Right } phase = Idle;
        Vec2 start, last;
        float held = 0;
    };

    static void setButton(ButtonState& b, bool down) {
        b.pressed = down && !b.down;
        b.released = !down && b.down;
        b.down = down;
    }
    // A key/click that lasts exactly one frame (down now, released next frame).
    void pulse(Key k) { m_keyPulse[static_cast<std::size_t>(k)] = true; }

    [[nodiscard]] Vec2 playerPos(const FrameInfo& f) const {
        if (f.player)
            if (const auto p = f.player->screenPosition()) return *p;
        return {f.width * 0.5f, f.height * 0.5f};
    }

    // Direct scheme: the aim point. Aim assist on a target near the aim direction, else the
    // weapon range along it (kept on screen along the same line, so the turret direction is
    // exact). Touch/mouse aim keeps the point they chose.
    [[nodiscard]] Vec2 aimPoint(Vec2 player, const FrameInfo& f, Vec2 screen) {
        const Settings& s = m_settings;
        if (m_aimFree)
            return player + m_aimOffset;
        const float range = f.aimDistance > 0 ? f.aimDistance : s.aimDistance;
        if (s.aimAssist && !f.targets.empty()) {
            const int t = pickTarget(f.targets, player, m_aimDir, s.assistCone, f.aimDistance, screen);
            if (t >= 0) {
                m_state.aimLocked = true;
                return f.targets[static_cast<std::size_t>(t)].point;
            }
        }
        const float along = range / std::max(1e-3f, gameDistance(m_aimDir));
        const float t = std::min(along, rayToBorder(player, m_aimDir, screen.x, screen.y, 2));
        return player + m_aimDir * t;
    }

    // The weapon the game reports: requests are repeated until it has switched (the original
    // ignores keys 1-4 while the turret loads or unloads), given up after weaponTimeout or
    // when the weapon is no longer available.
    void followWeapon(const FrameInfo& f, float dt) {
        m_weaponTracked = f.currentWeapon >= 0 && f.currentWeapon <= 3;
        if (!m_weaponTracked) {
            m_weaponPending = false;
            return;
        }
        if (m_weaponPending) {
            m_weaponAge += dt;
            m_weaponWait += dt;
            if (f.currentWeapon == m_state.weaponSlot || !(f.weaponMask & (1u << m_state.weaponSlot)) ||
                m_weaponAge > m_settings.weaponTimeout) {
                m_weaponPending = false;
            } else if (m_weaponWait >= m_settings.weaponRetry) {
                m_weaponWait = 0;
                pulse(static_cast<Key>(static_cast<int>(Key::Num1) + m_state.weaponSlot));
            }
        }
        if (!m_weaponPending)
            m_state.weaponSlot = f.currentWeapon;
    }

    void cycleWeapon(int step, std::uint8_t mask) {
        mask &= 0x0F;
        if (!mask)
            return;
        int slot = m_state.weaponSlot; // the pending request when there is one
        for (int i = 0; i < 4; ++i) {
            slot = (slot + step + 4) % 4;
            if (mask & (1u << slot)) break;
        }
        m_state.weaponSlot = slot;
        pulse(static_cast<Key>(static_cast<int>(Key::Num1) + slot));
        m_weaponPending = m_weaponTracked; // without a game to follow: fire and forget
        m_weaponAge = m_weaponWait = 0;
    }

    void moveCursorWithSticks(const RawInput& in, float dt) {
        const Settings& s = m_settings;
        Vec2 delta;
        const auto ls = radial(in.leftStick, s.deadzone);
        if (ls.mag > 0) {
            const float response = std::pow(ls.mag, s.curve);
            float factor = dt;
            constexpr float rim = 0.9f, keepDirection = 0.70710678f; // 45 degrees
            if (ls.mag >= rim && s.acceleration > 1) {
                const bool sameDir = m_accelHold > 0 && ls.dir.x * m_accelDir.x + ls.dir.y * m_accelDir.y >= keepDirection;
                if (!sameDir) {
                    m_accelHold = 0;
                    m_accelDir = ls.dir;
                }
                factor = accelIntegral(m_accelHold, m_accelHold + dt);
                m_accelHold += dt;
            } else {
                m_accelHold = 0;
            }
            delta = ls.dir * (s.cursorSpeed * response * factor);
        } else {
            m_accelHold = 0;
        }
        const auto rs = radial(in.rightStick, s.deadzone);
        if (rs.mag > 0)
            delta = delta + rs.dir * (s.cursorSpeed * s.preciseFactor * rs.mag * dt);
        if (delta != Vec2{}) {
            m_state.cursor = m_state.cursor + delta;
            m_state.source = PointerSource::Pad;
        }
    }

    void updateTouch(const RawInput& in, float dt, Vec2 screen, bool& leftDown, bool& rightDown) {
        const Settings& s = m_settings;
        TouchTracker& t = m_touch;
        const int count = in.touchCount;
        if (count > 0) {
            t.last = {in.touches[0].x * screen.x, in.touches[0].y * screen.y};
            m_state.cursor = t.last;
            m_state.source = PointerSource::Touch;
        }
        switch (t.phase) {
        case TouchTracker::Idle:
            if (count > 0) {
                t = {count >= 2 ? TouchTracker::Right : TouchTracker::Pending, t.last, t.last, 0};
                rightDown |= count >= 2;
            }
            break;
        case TouchTracker::Pending:
            if (count == 0) {
                m_state.cursor = t.last;
                m_leftPulse = true; // tap
                t.phase = TouchTracker::Idle;
                break;
            }
            t.held += dt;
            if (count >= 2 || t.held >= s.longPress) {
                t.phase = TouchTracker::Right;
                rightDown = true;
            } else if ((t.last - t.start).length() > s.touchSlop) {
                t.phase = TouchTracker::Drag;
                leftDown = true;
            }
            break;
        case TouchTracker::Drag:
            if (count == 0) { t.phase = TouchTracker::Idle; break; }
            leftDown = true;
            rightDown |= count >= 2; // second finger while dragging: attack too
            break;
        case TouchTracker::Right:
            if (count == 0) { t.phase = TouchTracker::Idle; break; }
            rightDown = true;
            break;
        }
    }

    void updateTextEntry(const RawInput& in, float dt) {
        const int dir = in.has(pad::Down) ? 1 : in.has(pad::Up) ? -1 : 0;
        if (dir != m_repeatDir) {
            m_repeatDir = dir;
            m_repeatHeld = 0;
            if (dir) stepCandidate(dir);
            return;
        }
        if (!dir)
            return;
        auto due = [&](float h) {
            const float d = m_settings.repeatDelay;
            return h < d ? 0 : static_cast<int>(std::floor((h - d) * m_settings.repeatRate)) + 1;
        };
        const int steps = due(m_repeatHeld + dt) - due(m_repeatHeld);
        m_repeatHeld += dt;
        for (int i = 0; i < steps; ++i) stepCandidate(dir);
    }

    void stepCandidate(int dir) {
        const auto n = static_cast<int>(TextEntryCharset.size());
        auto pos = static_cast<int>(TextEntryCharset.find(m_state.textCandidate));
        if (pos < 0) pos = 0;
        m_state.textCandidate = TextEntryCharset[static_cast<std::size_t>((pos + dir + n) % n)];
    }

    Settings m_settings;
    PcState m_state;
    bool m_cursorInit = false;
    RawInput m_prevPad;
    std::array<bool, KeyCount> m_prevKeys{};
    std::bitset<128> m_prevChars;
    std::array<bool, KeyCount> m_keyPulse{};
    bool m_leftPulse = false, m_rightPulse = false;
    // direct scheme
    Vec2 m_aimDir{0, -1};      // aim direction (stick or movement)
    Vec2 m_aimOffset;          // touch/mouse aim: point relative to the player
    bool m_aimFree = false;    // the aim point comes from touch/mouse (m_aimOffset)
    bool m_driving = false;    // the last move order was a drive (not a stop)
    Vec2 m_orderDir;
    float m_orderClock = 0;
    // weapons
    bool m_weaponPending = false, m_weaponTracked = false;
    float m_weaponAge = 0, m_weaponWait = 0;
    float m_accelHold = 0;
    Vec2 m_accelDir;
    TouchTracker m_touch;
    float m_touchGrace = 0;
    int m_repeatDir = 0;
    float m_repeatHeld = 0;
};

} // namespace gromada::input
