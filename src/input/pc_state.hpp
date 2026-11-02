#pragma once
// Input module, part 1: data types.
//  - RawInput: what the handheld gives us this frame (RetroPad, sticks, touchscreen, mouse,
//    keyboard), already decoded from libretro but not interpreted.
//  - PcState: what the original PC game expects (mouse cursor + two buttons, a few keys,
//    typed text). The engine consumes only PcState, so it never knows which scheme or
//    device produced it.

#include <array>
#include <bitset>
#include <cmath>
#include <cstdint>
#include <string>

namespace gromada::input {

struct Vec2 {
    float x = 0, y = 0;
    friend constexpr Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
    friend constexpr Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
    friend constexpr Vec2 operator*(Vec2 a, float s) { return {a.x * s, a.y * s}; }
    friend constexpr bool operator==(Vec2, Vec2) = default;
    [[nodiscard]] float length() const { return std::hypot(x, y); }
};

// Keys of the original game: 1-4 weapons, arrows = keyboard movement, TAB = info screen,
// Esc = main menu, Enter/Backspace for dialogs and name entry.
enum class Key : std::uint8_t { Num1, Num2, Num3, Num4, Left, Right, Up, Down, Tab, Escape, Enter, Backspace, Count };
inline constexpr std::size_t KeyCount = static_cast<std::size_t>(Key::Count);

struct ButtonState {
    bool down = false;     // held this frame
    bool pressed = false;  // went down this frame
    bool released = false; // went up this frame
};

// Which device moved the cursor last: lets the engine/UI adapt (e.g. no edge scrolling
// while the player is tapping the screen).
enum class PointerSource : std::uint8_t { Pad, Touch, Mouse };

struct PcState {
    Vec2 cursor;                        // screen pixels (subpixel precision), inside the screen
    ButtonState left, right;            // mouse buttons: left = move, right = attack
    std::array<ButtonState, KeyCount> keys{};
    std::string text;                   // characters typed this frame (ASCII)
    Vec2 move;                          // analog movement intent, |move| <= 1 (direct scheme, d-pad)
    bool aiming = false;                // direct scheme: right stick held, cursor = aim point
    bool aimLocked = false;             // direct scheme: the aim assist put the cursor on a target
    // Direct scheme: a left click at `moveTarget` this frame (move order of the left stick).
    // The cursor stays where it is drawn (the aim point); only the input record of this
    // frame carries the order. `moveTarget` may lie outside the screen.
    bool moveOrder = false;
    Vec2 moveTarget;
    // False when the cursor must not scroll the view at the screen edges: touch (a tap near
    // the border is not a request to scroll) and the aim point of the direct scheme.
    bool edgeScroll = true;
    char textCandidate = 'A';           // name entry with the pad: character under selection
    // Analog view zoom request of this frame, -1..+1 (+1 = zoom in), see game/view_zoom.hpp.
    // Presentation only: never part of the original input record.
    float zoomRate = 0.0f;
    int weaponSlot = 0;                 // 0..3, last weapon requested (keys 1-4)
    bool weaponPending = false;         // weaponSlot asked for, the game has not switched yet
    PointerSource source = PointerSource::Pad;

    [[nodiscard]] const ButtonState& key(Key k) const { return keys[static_cast<std::size_t>(k)]; }
    [[nodiscard]] int cursorX() const { return static_cast<int>(std::floor(cursor.x)); }
    [[nodiscard]] int cursorY() const { return static_cast<int>(std::floor(cursor.y)); }
};

// RetroPad button ids: same numbering as RETRO_DEVICE_ID_JOYPAD_* (positions, SNES layout:
// B bottom, A right, Y left, X top).
namespace pad {
enum : unsigned { B = 0, Y, Select, Start, Up, Down, Left, Right, A, X, L, R, L2, R2, L3, R3, Count };
}

struct RawInput {
    struct Touch {
        float x = 0, y = 0; // 0..1 across the game image
    };

    std::uint16_t buttons = 0;          // bit = pad::*
    Vec2 leftStick, rightStick;         // -1..1, y grows downwards
    std::array<Touch, 4> touches{};     // fingers on the screen, first one down first
    int touchCount = 0;
    Vec2 mouseDelta;                    // relative motion in screen pixels
    bool mouseLeft = false, mouseRight = false;
    int mouseWheel = 0;                 // +1 up, -1 down (one notch per frame at most)
    std::array<bool, KeyCount> keys{};  // physical keyboard, already mapped to game keys
    std::bitset<128> chars;             // printable keys held (lowercase letters, digits, space, '-')
    bool shift = false;

    [[nodiscard]] bool has(unsigned id) const { return (buttons >> id) & 1u; }
    void set(unsigned id, bool on = true) {
        if (on) buttons |= static_cast<std::uint16_t>(1u << id);
        else buttons &= static_cast<std::uint16_t>(~(1u << id));
    }
};

} // namespace gromada::input
