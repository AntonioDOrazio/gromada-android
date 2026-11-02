#pragma once
// Game session, input: input::PcState (controller, touch, mouse, keyboard, already mapped to
// the PC game's mouse and keys by src/input) -> the original 12-byte input record
// (game::OriginalInput, docs/re/runtime.md §6) that scripts (GetKey/GetInputState), the menus
// and the engine read every frame.
//
// The original builds the record from window messages (0x401840):
//   WM_LBUTTONDOWN -> LCLICK|LDOWN, WM_LBUTTONUP clears LDOWN (right button: RCLICK|RDOWN);
//   arrow keys held -> LEFT/RIGHT/UP/DOWN bits; Shift held -> 0x100;
//   WM_CHAR -> key (13 Enter, 27 Esc, 9 Tab, 8 Backspace, ASCII), arrow WM_KEYDOWN -> key 1..4,
//   Delete -> 5. Click bits and the key last one frame (cleared at the end of the tick).
// Windows repeats WM_KEYDOWN/WM_CHAR while a key is held (typematic): reproduced for every
// key with KeyRepeat timing, so menus scroll when an arrow is held.
// A frame carries one key: when several keys go down in the same frame the others are
// queued and delivered in the next frames (the original would keep only the last one).
//
// Header-only; depends on input/pc_state.hpp and the engine contract.

#include "core/std.hpp"
#include "game/engine_api.hpp"
#include "input/pc_state.hpp"

#include <deque>

namespace gromada::game {

// GetKey() codes that are not plain characters.
namespace keycode {
inline constexpr std::uint8_t ArrowLeft = 1, ArrowRight = 2, ArrowUp = 3, ArrowDown = 4, Delete = 5;
inline constexpr std::uint8_t Backspace = 8, Tab = 9, Enter = 13, Escape = 27;
} // namespace keycode

struct KeyRepeat {
    float delay = 0.5f;   // s before the first repeat (Windows default keyboard delay 1 = 500 ms)
    float rate = 15.0f;   // repeats per second
};

// Size of the game view for InputRecordBuilder::build (0 = unknown).
struct InputView {
    int width = 0, height = 0;
};

class InputRecordBuilder {
public:
    friend struct SaveStateAccess; // save states (game/savestate.hpp)
    // GetKey() code of a game key, 0 if the key produces none.
    static std::uint8_t keyCode(input::Key k) {
        using enum input::Key;
        switch (k) {
        case Num1: return '1';
        case Num2: return '2';
        case Num3: return '3';
        case Num4: return '4';
        case Left: return keycode::ArrowLeft;
        case Right: return keycode::ArrowRight;
        case Up: return keycode::ArrowUp;
        case Down: return keycode::ArrowDown;
        case Tab: return keycode::Tab;
        case Escape: return keycode::Escape;
        case Enter: return keycode::Enter;
        case Backspace: return keycode::Backspace;
        default: return 0;
        }
    }

    // The engine scrolls the view while the mouse is within 5 px of its border (0x415e8d).
    // `view` (when set) is the view size: a cursor that must not scroll (PcState::edgeScroll
    // false: touch, aim point of the direct scheme) is kept out of that band in the record.
    using View = InputView;
    static constexpr int EdgeBand = 6;

    // Builds the record of this frame. `screenX/Y` is the cursor in the coordinates the engine
    // uses for its view (pixels from the top-left corner of the game view), `cameraX/Y` the
    // view origin in the world. `dt` (seconds) drives the key repeat.
    // A move order of the direct scheme (PcState::moveOrder) is a left click at its target
    // instead of the cursor, without the right button (Player::Tact would fire at that point).
    // Otherwise the aim point of the direct scheme (PcState::aimTarget, issue #19) is the mouse
    // point, also off screen: the reticle (cursor) is only where it is drawn.
    OriginalInput build(const input::PcState& pc, int screenX, int screenY, int cameraX, int cameraY, float dt,
                        View view = {}) {
        OriginalInput r;
        int sx = screenX, sy = screenY;
        if (const input::Vec2* point = pc.moveOrder ? &pc.moveTarget : pc.hasAimTarget ? &pc.aimTarget : nullptr) {
            sx = static_cast<int>(std::floor(point->x)) + (screenX - pc.cursorX());
            sy = static_cast<int>(std::floor(point->y)) + (screenY - pc.cursorY());
        }
        r.mouseWorldX = static_cast<std::int16_t>(sx + cameraX);
        r.mouseWorldY = static_cast<std::int16_t>(sy + cameraY);
        if (view.width > 2 * EdgeBand && view.height > 2 * EdgeBand && !pc.edgeScroll) {
            sx = std::clamp(sx, EdgeBand, view.width - 1 - EdgeBand);
            sy = std::clamp(sy, EdgeBand, view.height - 1 - EdgeBand);
        }
        r.mouseScreenX = static_cast<std::int16_t>(sx);
        r.mouseScreenY = static_cast<std::int16_t>(sy);

        std::uint16_t bits = 0;
        if (pc.moveOrder) {
            bits |= OriginalInput::LClick | OriginalInput::LDown;
        } else {
            if (pc.left.pressed) bits |= OriginalInput::LClick;
            if (pc.left.down) bits |= OriginalInput::LDown;
            if (pc.right.pressed) bits |= OriginalInput::RClick;
            if (pc.right.down) bits |= OriginalInput::RDown;
        }
        if (pc.key(input::Key::Left).down) bits |= OriginalInput::Left;
        if (pc.key(input::Key::Right).down) bits |= OriginalInput::Right;
        if (pc.key(input::Key::Up).down) bits |= OriginalInput::Up;
        if (pc.key(input::Key::Down).down) bits |= OriginalInput::Down;
        r.bits = bits;

        // Key events of this frame, in a stable order: special keys, then typed text.
        for (std::size_t i = 0; i < input::KeyCount; ++i) {
            const auto k = static_cast<input::Key>(i);
            const std::uint8_t code = keyCode(k);
            if (!code)
                continue;
            const auto& b = pc.keys[i];
            if (b.pressed) {
                m_held[i] = 0;
                push(code);
            } else if (b.down) {
                const float before = m_held[i];
                m_held[i] += dt;
                push(code, repeats(before, m_held[i]));
            } else {
                m_held[i] = 0;
            }
        }
        for (const char c : pc.text)
            push(static_cast<std::uint8_t>(c));

        if (!m_queue.empty()) {
            r.key = m_queue.front();
            m_queue.pop_front();
        }
        return r;
    }

    void clear() {
        m_queue.clear();
        m_held.fill(0);
    }
    void setRepeat(KeyRepeat r) { m_repeat = r; }
    [[nodiscard]] std::size_t pending() const noexcept { return m_queue.size(); }

private:
    static constexpr std::size_t MaxQueue = 8;

    void push(std::uint8_t code, int count = 1) {
        for (int i = 0; i < count && m_queue.size() < MaxQueue; ++i)
            m_queue.push_back(code);
    }

    // Number of typematic repeats between hold times t0 and t1 (seconds).
    [[nodiscard]] int repeats(float t0, float t1) const {
        auto count = [&](float t) {
            return t < m_repeat.delay ? 0 : static_cast<int>(std::floor((t - m_repeat.delay) * m_repeat.rate)) + 1;
        };
        return count(t1) - count(t0);
    }

    std::deque<std::uint8_t> m_queue;
    std::array<float, input::KeyCount> m_held{};
    KeyRepeat m_repeat;
};

} // namespace gromada::game
