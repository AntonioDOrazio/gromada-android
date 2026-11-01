// Input module: pad/touch/mouse/keyboard -> PC state, for both control schemes, and
// frame-rate independence (60 vs 120 fps). Synthetic input only.
#include "input/mapper.hpp"
#include "test.hpp"

#include <functional>

using namespace gromada::input;

namespace {

bool near(float a, float b, float eps = 0.01f) { return std::abs(a - b) <= eps; }
#define CHECK_NEAR(a, b) do { const float _x = (a), _y = (b); if (!near(_x, _y)) { ++gromada_test::failures; \
    std::fprintf(stderr, "%s:%d: CHECK_NEAR failed: %s = %f, %s = %f\n", __FILE__, __LINE__, #a, _x, #b, _y); } } while (0)

RawInput padWith(std::initializer_list<unsigned> ids) {
    RawInput in;
    for (unsigned id : ids) in.set(id);
    return in;
}

FrameInfo frame(int fps = 60, Context ctx = Context::Gameplay) {
    FrameInfo f;
    f.dt = 1.0f / fps;
    f.context = ctx;
    return f;
}

struct FixedPlayer : PlayerPosition {
    std::optional<Vec2> pos;
    std::optional<Vec2> screenPosition() const override { return pos; }
};

// Runs `seconds` of input at `fps`; input(t) gives the raw input for the frame starting at t.
PcState run(Mapper& m, int fps, float seconds, const std::function<RawInput(float)>& input, FrameInfo f = frame()) {
    f.dt = 1.0f / fps;
    const int frames = static_cast<int>(std::lround(seconds * fps));
    PcState s;
    for (int i = 0; i < frames; ++i) s = m.update(input(static_cast<float>(i) / fps), f);
    return s;
}

void testButtons() {
    Mapper m;
    auto s = m.update(padWith({pad::A}), frame());
    CHECK(s.left.down && s.left.pressed && !s.right.down);
    s = m.update(padWith({pad::A}), frame());
    CHECK(s.left.down && !s.left.pressed);
    s = m.update(padWith({}), frame());
    CHECK(!s.left.down && s.left.released);

    s = m.update(padWith({pad::B, pad::Start, pad::Select, pad::X, pad::Up, pad::Left}), frame());
    CHECK(s.right.pressed && !s.left.down);
    CHECK(s.key(Key::Escape).pressed && s.key(Key::Tab).pressed && s.key(Key::Enter).pressed);
    CHECK(s.key(Key::Up).down && s.key(Key::Left).down && !s.key(Key::Down).down && !s.key(Key::Right).down);
    CHECK(near(s.move.x, -0.7071f) && near(s.move.y, -0.7071f));
    s = m.update(padWith({}), frame());
    CHECK(s.right.released && s.key(Key::Escape).released && s.key(Key::Tab).released && s.key(Key::Up).released);
}

void testWeapons() {
    Mapper m;
    auto s = m.update(padWith({pad::R}), frame());
    CHECK_EQ(s.weaponSlot, 1);
    CHECK(s.key(Key::Num2).pressed && s.key(Key::Num2).down);
    s = m.update(padWith({pad::R}), frame()); // held: no repeat, key pulse ends
    CHECK_EQ(s.weaponSlot, 1);
    CHECK(s.key(Key::Num2).released && !s.key(Key::Num3).down);
    m.update(padWith({}), frame());
    s = m.update(padWith({pad::L}), frame());
    s = m.update(padWith({}), frame());
    s = m.update(padWith({pad::L}), frame()); // 1 -> 0 -> 3 (wraps)
    CHECK_EQ(s.weaponSlot, 3);
    CHECK(s.key(Key::Num4).pressed);

    // Only weapons 1 and 3 available: R from 1 goes to 3, then back to 1.
    FrameInfo f = frame();
    f.weaponMask = 0b0101;
    m.setWeaponSlot(0);
    m.update(padWith({}), f);
    s = m.update(padWith({pad::R}), f);
    CHECK_EQ(s.weaponSlot, 2);
    CHECK(s.key(Key::Num3).pressed);
    m.update(padWith({}), f);
    s = m.update(padWith({pad::R}), f);
    CHECK_EQ(s.weaponSlot, 0);
    f.weaponMask = 0;
    m.update(padWith({}), f);
    s = m.update(padWith({pad::R}), f); // nothing available: no change, no key
    CHECK_EQ(s.weaponSlot, 0);
    CHECK(!s.key(Key::Num1).pressed);
}

void testCursorStick() {
    Settings st;
    st.cursorSpeed = 200;
    FrameInfo big = frame();
    big.width = 4000;
    big.height = 4000;

    // Below the deadzone: no motion.
    Mapper m0(st);
    m0.warpCursor({100, 100});
    auto s = run(m0, 60, 1, [](float) { RawInput in; in.leftStick = {0.1f, -0.1f}; return in; }, big);
    CHECK(s.cursor == (Vec2{100, 100}));

    // Half deflection: response curve, no acceleration.
    for (int fps : {60, 120}) {
        Mapper m(st);
        m.warpCursor({100, 100});
        s = run(m, fps, 1, [](float) { RawInput in; in.leftStick = {0.575f, 0}; return in; }, big);
        CHECK_NEAR(s.cursor.x, 100 + 200 * 0.25f); // ((0.575-0.15)/0.85)^2 = 0.25
        CHECK_NEAR(s.cursor.y, 100);
        CHECK(s.source == PointerSource::Pad);
    }

    // Full deflection with acceleration: same distance at 60 and 120 fps, equal to the integral.
    Mapper ref(st);
    const float expected = 200 * ref.accelIntegral(0, 1.5f);
    CHECK(expected > 200 * 1.5f * 1.2f); // acceleration really kicks in
    float x60 = 0, x120 = 0;
    for (int fps : {60, 120}) {
        Mapper m(st);
        m.warpCursor({0, 0});
        s = run(m, fps, 1.5f, [](float) { RawInput in; in.leftStick = {1, 0}; return in; }, big);
        CHECK_NEAR(s.cursor.x, expected);
        (fps == 60 ? x60 : x120) = s.cursor.x;
    }
    CHECK_NEAR(x60, x120);

    // A trajectory: right at the rim, then down at half, then up-left at the rim.
    auto path = [](float t) {
        RawInput in;
        in.leftStick = t < 0.5f ? Vec2{1, 0} : t < 1.0f ? Vec2{0, 0.6f} : Vec2{-0.8f, -0.8f};
        return in;
    };
    Mapper a(st), b(st);
    a.warpCursor({2000, 2000});
    b.warpCursor({2000, 2000});
    const auto s60 = run(a, 60, 2, path, big), s120 = run(b, 120, 2, path, big);
    CHECK_NEAR(s60.cursor.x, s120.cursor.x);
    CHECK_NEAR(s60.cursor.y, s120.cursor.y);

    // Turning back at the rim restarts the acceleration (no overshoot when reversing).
    Mapper r(st);
    r.warpCursor({2000, 2000});
    run(r, 60, 1, [](float) { RawInput in; in.leftStick = {1, 0}; return in; }, big);
    const float before = r.state().cursor.x;
    s = run(r, 60, 0.2f, [](float) { RawInput in; in.leftStick = {-1, 0}; return in; }, big);
    CHECK_NEAR(before - s.cursor.x, 200 * 0.2f); // back to 1x (0.2 s < accelDelay)

    // Right stick: slow, linear, no acceleration.
    Mapper p(st);
    p.warpCursor({100, 100});
    s = run(p, 120, 1, [](float) { RawInput in; in.rightStick = {0, 1}; return in; }, big);
    CHECK_NEAR(s.cursor.y, 100 + 200 * st.preciseFactor);

    // The cursor stays on screen.
    Mapper c(st);
    s = run(c, 60, 5, [](float) { RawInput in; in.leftStick = {1, 1}; return in; });
    CHECK(s.cursor.x == 639 && s.cursor.y == 479);
    CHECK_EQ(s.cursorX(), 639);
}

void testDirect() {
    Settings st;
    st.scheme = Scheme::Direct;
    FixedPlayer player;
    player.pos = Vec2{100, 200};
    FrameInfo f = frame();
    f.player = &player;

    Mapper m(st);
    const Vec2 start = m.update(RawInput{}, f).cursor;
    auto stick = [](Vec2 l, Vec2 r = {}) { RawInput in; in.leftStick = l; in.rightStick = r; return in; };

    auto s = m.update(stick({1, 0}), f);
    CHECK(s.key(Key::Right).down && !s.key(Key::Up).down && !s.key(Key::Down).down);
    CHECK(near(s.move.x, 1) && near(s.move.y, 0));
    CHECK(s.cursor == start); // the left stick never moves the cursor
    s = m.update(stick({0.7f, 0.7f}), f);
    CHECK(s.key(Key::Right).down && s.key(Key::Down).down);
    s = m.update(stick({0.9f, 0.2f}), f); // 12.5 degrees: still pure right
    CHECK(s.key(Key::Right).down && !s.key(Key::Down).down);
    s = m.update(stick({0, -0.3f}), f); // under the move threshold
    CHECK(!s.key(Key::Up).down && s.move == Vec2{});
    CHECK(s.key(Key::Right).released);

    // Right stick half way: aim only.
    s = m.update(stick({}, {0.5f, 0}), f);
    CHECK(s.aiming && !s.right.down);
    CHECK(s.cursor == (Vec2{260, 200}));
    // All the way: aim and fire (right button held at the aim point).
    s = m.update(stick({}, {0, -1}), f);
    CHECK(s.aiming && s.right.pressed);
    CHECK(s.cursor == (Vec2{100, 40}));
    s = m.update(stick({}, {0, -1}), f);
    CHECK(s.right.down && !s.right.pressed);
    // Released: no fire, the aim point stays and travels with the player.
    s = m.update(stick({}), f);
    CHECK(!s.aiming && s.right.released);
    player.pos = Vec2{300, 250};
    s = m.update(stick({}), f);
    CHECK(s.cursor == (Vec2{300, 90}));
    // B attacks at the aim point; aim distance can come from the engine.
    f.aimDistance = 50;
    s = m.update(stick({}, {-1, 0}), f);
    CHECK(s.cursor == (Vec2{250, 250}) && s.right.down);
    // No player position: aim from the screen center.
    player.pos.reset();
    s = m.update(stick({}, {1, 0}), f);
    CHECK(s.cursor == (Vec2{370, 240}));
    // D-pad: arrows and analog move vector.
    s = m.update(padWith({pad::Down}), f);
    CHECK(s.key(Key::Down).down && near(s.move.y, 1));

    // Menu context: direct scheme falls back to the cursor (menus need a pointer).
    FrameInfo menu = f;
    menu.context = Context::Menu;
    const Vec2 before = m.update(RawInput{}, menu).cursor;
    s = m.update(stick({1, 0}), menu);
    CHECK(s.cursor.x > before.x && !s.key(Key::Right).down);
}

RawInput touch(std::initializer_list<Vec2> fingers) {
    RawInput in;
    for (Vec2 p : fingers) in.touches[in.touchCount++] = {p.x, p.y};
    return in;
}

void testTouch() {
    Mapper m;
    // Tap: cursor jumps on contact, left click on release, lasting one frame.
    auto s = m.update(touch({{0.5f, 0.25f}}), frame());
    CHECK(s.cursor == (Vec2{320, 120}) && !s.left.down && s.source == PointerSource::Touch);
    s = m.update(touch({}), frame());
    CHECK(s.left.pressed && s.left.down && !s.right.down);
    s = m.update(touch({}), frame());
    CHECK(s.left.released);

    // Long press: right button after longPress, held until the finger lifts; no left click.
    for (int fps : {60, 120}) {
        Mapper lp;
        float firedAt = -1;
        for (int i = 0; i < fps; ++i) {
            s = lp.update(touch({{0.1f, 0.1f}}), frame(fps));
            if (s.right.pressed) firedAt = static_cast<float>(i) / fps; // finger down at frame 0
            CHECK(!s.left.down);
        }
        CHECK(firedAt >= 0.45f - 0.001f && firedAt <= 0.45f + 1.0f / fps + 0.001f);
        CHECK(s.right.down);
        s = lp.update(touch({}), frame(fps));
        CHECK(s.right.released && !s.left.down);
    }

    // Two fingers: right click at the first finger.
    Mapper two;
    s = two.update(touch({{0.25f, 0.5f}, {0.75f, 0.5f}}), frame());
    CHECK(s.right.pressed && s.cursor == (Vec2{160, 240}));
    // Second finger joining a pending touch.
    Mapper join;
    join.update(touch({{0.25f, 0.5f}}), frame());
    s = join.update(touch({{0.25f, 0.5f}, {0.7f, 0.5f}}), frame());
    CHECK(s.right.pressed && !s.left.down);

    // Drag: left held, cursor follows the finger.
    Mapper drag;
    drag.update(touch({{0.5f, 0.5f}}), frame());
    s = drag.update(touch({{0.505f, 0.5f}}), frame()); // 3.2 px: within slop
    CHECK(!s.left.down);
    s = drag.update(touch({{0.6f, 0.5f}}), frame());
    CHECK(s.left.pressed && s.cursor == (Vec2{384, 240}));
    s = drag.update(touch({{0.7f, 0.6f}}), frame());
    CHECK(s.left.down && s.cursor == (Vec2{448, 288}));
    s = drag.update(touch({}), frame());
    CHECK(s.left.released && !s.right.down);

    // Disabled touch: ignored.
    Settings off;
    off.touch = false;
    Mapper none(off);
    none.update(touch({{0.1f, 0.1f}}), frame());
    s = none.update(touch({}), frame());
    CHECK(!s.left.down && s.cursor == (Vec2{320, 240}));
}

void testMouse() {
    Mapper m;
    RawInput in;
    in.mouseDelta = {10, -5};
    auto s = m.update(in, frame());
    CHECK(s.cursor == (Vec2{330, 235}) && s.source == PointerSource::Mouse);
    in = {};
    in.mouseRight = true;
    s = m.update(in, frame());
    CHECK(s.right.pressed);
    in = {};
    in.mouseWheel = -1;
    s = m.update(in, frame());
    CHECK(s.right.released && s.weaponSlot == 1 && s.key(Key::Num2).pressed);

    // Android emulates mouse clicks from taps: the mouse is ignored around touches.
    Mapper t;
    RawInput both = touch({{0.5f, 0.5f}});
    both.mouseLeft = true;
    both.mouseDelta = {50, 0};
    s = t.update(both, frame());
    CHECK(!s.left.down && s.cursor == (Vec2{320, 240}));
    RawInput ghost;
    ghost.mouseLeft = true; // quick-tap emulation right after the finger lifts
    s = t.update(ghost, frame());
    CHECK(s.left.pressed); // this is the tap itself (pulse), not the mouse
    s = t.update(ghost, frame());
    CHECK(s.left.released);
    for (int i = 0; i < 30; ++i) s = t.update(RawInput{}, frame());
    s = t.update(ghost, frame()); // after the grace period the mouse works again
    CHECK(s.left.pressed);
}

void testTextEntry() {
    for (int fps : {60, 120}) {
        Mapper m;
        auto s = m.update(padWith({pad::Down}), frame(fps, Context::TextEntry));
        CHECK_EQ(s.textCandidate, 'B');
        CHECK(!s.key(Key::Down).down); // the d-pad picks letters, no arrow keys here
        // Held 0.95 s more: auto-repeat after 0.4 s at 12/s -> floor(0.55 * 12) + 1 = 7 steps -> 'I'.
        s = run(m, fps, 0.95f, [](float) { return padWith({pad::Down}); }, frame(fps, Context::TextEntry));
        CHECK_EQ(s.textCandidate, 'I');
        s = m.update(padWith({}), frame(fps, Context::TextEntry));
        s = m.update(padWith({pad::Up}), frame(fps, Context::TextEntry));
        CHECK_EQ(s.textCandidate, 'H');
        s = m.update(padWith({pad::A}), frame(fps, Context::TextEntry));
        CHECK(s.text == "H" && !s.left.down);
        s = m.update(padWith({pad::A}), frame(fps, Context::TextEntry));
        CHECK(s.text.empty());
        s = m.update(padWith({pad::B}), frame(fps, Context::TextEntry));
        CHECK(s.key(Key::Backspace).pressed && !s.right.down);
        s = m.update(padWith({pad::Start}), frame(fps, Context::TextEntry));
        CHECK(s.key(Key::Enter).pressed && !s.key(Key::Escape).down);
        s = m.update(padWith({pad::Select}), frame(fps, Context::TextEntry));
        CHECK(s.key(Key::Escape).pressed);
    }
    Mapper wrap;
    auto s = wrap.update(padWith({pad::Up}), frame(60, Context::TextEntry));
    CHECK_EQ(s.textCandidate, '-');
}

void testKeyboard() {
    Settings st;
    st.keyboard = true;
    Mapper m(st);
    RawInput in;
    in.chars['a'] = in.chars['7'] = true;
    in.keys[static_cast<std::size_t>(Key::Num3)] = true;
    auto s = m.update(in, frame());
    CHECK(s.text == "7a" && s.weaponSlot == 2 && s.key(Key::Num3).pressed);
    s = m.update(in, frame()); // held: typed once
    CHECK(s.text.empty() && s.key(Key::Num3).down);
    in = {};
    in.shift = true;
    in.chars['g'] = true;
    s = m.update(in, frame());
    CHECK(s.text == "G");

    Mapper off; // keyboard disabled by default (desktop RetroArch maps keys to the pad too)
    s = off.update(in, frame());
    CHECK(s.text.empty());
}

void testEdgeScroll() {
    CHECK(edgeScroll({320, 240}, 640, 480) == Vec2{});
    CHECK(edgeScroll({0, 479}, 640, 480) == (Vec2{-1, 1}));
    const Vec2 e = edgeScroll({8, 240}, 640, 480);
    CHECK(near(e.x, -0.5f) && e.y == 0);
    CHECK(edgeScroll({639, 0}, 640, 480) == (Vec2{1, -1}));
}

} // namespace

int main() {
    testButtons();
    testWeapons();
    testCursorStick();
    testDirect();
    testTouch();
    testMouse();
    testTextEntry();
    testKeyboard();
    testEdgeScroll();
    return gromada_test::finish();
}
