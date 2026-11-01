#pragma once
// Input module, part 2: control schemes. Turns RawInput into PcState once per frame.
// Pure logic (no libretro calls), so it is unit-tested directly.
//
// Schemes (core option):
//  - VirtualCursor: left stick moves the cursor (response curve + acceleration), right stick
//    moves it slowly for precision; A = left click, B = right click.
//  - Direct: left stick = arrow keys (keyboard movement of the original), right stick = aim:
//    the cursor sits at the player's screen position + direction * aimDistance; pushing past
//    fireThreshold holds the right button there (attack).
// Common: L1/R1 = previous/next weapon (keys 1-4), Start = Esc, Select = TAB, X = Enter,
// d-pad = arrows. Touchscreen: tap = left click, long press or two fingers = right click,
// drag = left button held. Mouse: relative cursor + buttons, wheel = weapon cycling.
// Contexts (set by the engine every frame): Menu forces the cursor scheme (menus need a
// pointer), TextEntry turns the pad into a character picker for the player's name.
//
// All speeds are per second and integrated exactly over the frame, so 60 and 120 fps give
// the same motion for the same input.

#include "input/pc_state.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <string_view>

namespace gromada::input {

enum class Scheme : std::uint8_t { VirtualCursor, Direct };
enum class Context : std::uint8_t { Gameplay, Menu, TextEntry };

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
    float moveThreshold = 0.3f;  // stick deflection that presses arrow keys
    float aimThreshold = 0.15f;  // right stick: aim (cursor moves, no attack)
    float fireThreshold = 0.7f;  // right stick: aim and hold the right button
    float aimDistance = 160;     // px from the player to the aim point
    // Pointing devices
    bool touch = true, mouse = true, keyboard = false;
    float longPress = 0.45f;     // s without moving: right click
    float touchSlop = 10;        // px a finger may wander before a tap becomes a drag
    float touchMouseGrace = 0.35f; // s: ignore the mouse after touches (Android emulates a mouse from taps)
    float mouseScale = 1;
    // Name entry with the pad
    float repeatDelay = 0.4f, repeatRate = 12;
};

// The engine tells the input module where the player is on screen (direct scheme aims from
// there). The viewer has no player and uses the screen center.
struct PlayerPosition {
    virtual ~PlayerPosition() = default;
    [[nodiscard]] virtual std::optional<Vec2> screenPosition() const = 0;
};

struct FrameInfo {
    float dt = 1.0f / 60;   // seconds since the previous frame
    int width = 640, height = 480;
    Context context = Context::Gameplay;
    const PlayerPosition* player = nullptr; // nullptr or nullopt: screen center
    std::uint8_t weaponMask = 0x0F;         // bit i: weapon i+1 available
    float aimDistance = 0;                  // > 0 overrides Settings::aimDistance (e.g. weapon range)
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
            m_accelHold = 0;
        }
        m_settings = s;
    }
    [[nodiscard]] const Settings& settings() const { return m_settings; }
    [[nodiscard]] const PcState& state() const { return m_state; }

    void warpCursor(Vec2 p) { m_state.cursor = p; m_cursorInit = true; }
    // The engine reports weapon changes it makes itself (pick-ups, empty ammo).
    void setWeaponSlot(int slot) { m_state.weaponSlot = std::clamp(slot, 0, 3); }

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

        // In the direct scheme the cursor is the aim point and travels with the player.
        if (direct)
            m_state.cursor = player + m_aimOffset;

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

        if (direct) {
            // Left stick: arrow keys in 8 sectors of 45 degrees, plus the analog vector.
            const auto ls = radial(in.leftStick, s.deadzone);
            if (ls.mag > s.moveThreshold) {
                m_state.move = ls.dir * ls.mag;
                constexpr float sin22 = 0.38268343f; // sector boundary
                hold(Key::Right, ls.dir.x > sin22);
                hold(Key::Left, ls.dir.x < -sin22);
                hold(Key::Down, ls.dir.y > sin22);
                hold(Key::Up, ls.dir.y < -sin22);
            } else if (dpad != Vec2{}) {
                m_state.move = dpad * (1.0f / dpad.length());
            }
            // Right stick: aim, then fire.
            const auto rs = radial(in.rightStick, s.deadzone);
            if (rs.mag > s.aimThreshold) {
                const float dist = f.aimDistance > 0 ? f.aimDistance : s.aimDistance;
                m_aimOffset = rs.dir * dist;
                m_state.cursor = player + m_aimOffset;
                m_state.aiming = true;
                m_state.source = PointerSource::Pad;
                rightDown |= rs.mag >= s.fireThreshold;
            }
        } else {
            if (!textEntry && dpad != Vec2{})
                m_state.move = dpad * (1.0f / dpad.length());
            moveCursorWithSticks(in, dt);
        }

        // --- touchscreen ------------------------------------------------------------
        if (s.touch)
            updateTouch(in, dt, screen, leftDown, rightDown);
        else
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
                if (in.keys[k] && !m_prevKeys[k]) m_state.weaponSlot = k;
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
        if (direct)
            m_aimOffset = m_state.cursor - player; // touch/mouse re-aim; keeps the clamped point

        setButton(m_state.left, leftDown || m_leftPulse);
        setButton(m_state.right, rightDown || m_rightPulse);
        m_leftPulse = m_rightPulse = false;
        for (std::size_t k = 0; k < KeyCount; ++k) {
            setButton(m_state.keys[k], keyDown[k] || m_keyPulse[k]);
            m_keyPulse[k] = false;
        }
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

    void cycleWeapon(int step, std::uint8_t mask) {
        mask &= 0x0F;
        if (!mask)
            return;
        int slot = m_state.weaponSlot;
        for (int i = 0; i < 4; ++i) {
            slot = (slot + step + 4) % 4;
            if (mask & (1u << slot)) break;
        }
        m_state.weaponSlot = slot;
        pulse(static_cast<Key>(static_cast<int>(Key::Num1) + slot));
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
    Vec2 m_aimOffset;
    float m_accelHold = 0;
    Vec2 m_accelDir;
    TouchTracker m_touch;
    float m_touchGrace = 0;
    int m_repeatDir = 0;
    float m_repeatHeld = 0;
};

} // namespace gromada::input
