#pragma once
// Save states of the input mapper (issue #14, docs/savestates.md): the PC state it produced
// last (cursor, buttons for edge detection), the previous raw input, pulses, aim offset,
// cursor acceleration, touch tracker and the name-entry repeat. The Settings come from the
// core options and are not stored. Header-only.

#include "core/state_io.hpp"
#include "input/mapper.hpp"

namespace gromada::input {

namespace detail {
inline void putVec(state::Writer& w, Vec2 v) { w.f32(v.x); w.f32(v.y); }
inline Vec2 getVec(state::Reader& r) {
    Vec2 v;
    v.x = r.finite32();
    v.y = r.finite32();
    return v;
}
inline void putButton(state::Writer& w, const ButtonState& b) {
    w.u8(static_cast<std::uint8_t>((b.down ? 1 : 0) | (b.pressed ? 2 : 0) | (b.released ? 4 : 0)));
}
inline ButtonState getButton(state::Reader& r) {
    const std::uint8_t v = r.u8();
    if (v > 7) throw state::Error("save state: bad button state");
    return {(v & 1) != 0, (v & 2) != 0, (v & 4) != 0};
}
template <std::size_t N>
inline void putBools(state::Writer& w, const std::array<bool, N>& a) {
    for (const bool b : a) w.boolean(b);
}
template <std::size_t N>
inline void getBools(state::Reader& r, std::array<bool, N>& a) {
    for (bool& b : a) b = r.boolean();
}
inline void putBits(state::Writer& w, const std::bitset<128>& b) {
    for (std::size_t i = 0; i < 128; i += 8) {
        std::uint8_t v = 0;
        for (std::size_t k = 0; k < 8; ++k) v |= static_cast<std::uint8_t>(b[i + k] ? 1u << k : 0u);
        w.u8(v);
    }
}
inline std::bitset<128> getBits(state::Reader& r) {
    std::bitset<128> b;
    for (std::size_t i = 0; i < 128; i += 8) {
        const std::uint8_t v = r.u8();
        for (std::size_t k = 0; k < 8; ++k) b[i + k] = (v >> k) & 1u;
    }
    return b;
}
} // namespace detail

inline void writePcState(state::Writer& w, const PcState& s) {
    detail::putVec(w, s.cursor);
    detail::putButton(w, s.left);
    detail::putButton(w, s.right);
    for (const auto& k : s.keys) detail::putButton(w, k);
    w.str(s.text);
    detail::putVec(w, s.move);
    w.boolean(s.aiming);
    w.boolean(s.aimLocked);
    w.boolean(s.moveOrder);
    detail::putVec(w, s.moveTarget);
    w.boolean(s.edgeScroll);
    w.u8(static_cast<std::uint8_t>(s.textCandidate));
    w.i32(s.weaponSlot);
    w.boolean(s.weaponPending);
    w.u8(static_cast<std::uint8_t>(s.source));
}
inline PcState readPcState(state::Reader& r) {
    PcState s;
    s.cursor = detail::getVec(r);
    s.left = detail::getButton(r);
    s.right = detail::getButton(r);
    for (auto& k : s.keys) k = detail::getButton(r);
    s.text = r.str(4096);
    s.move = detail::getVec(r);
    s.aiming = r.boolean();
    s.aimLocked = r.boolean();
    s.moveOrder = r.boolean();
    s.moveTarget = detail::getVec(r);
    s.edgeScroll = r.boolean();
    s.textCandidate = static_cast<char>(r.u8());
    s.weaponSlot = r.ranged<std::int32_t>(0, 3, "weapon slot");
    s.weaponPending = r.boolean();
    s.source = static_cast<PointerSource>(r.ranged<std::uint8_t>(0, 2, "pointer source"));
    return s;
}

inline void writeRawInput(state::Writer& w, const RawInput& in) {
    w.u16(in.buttons);
    detail::putVec(w, in.leftStick);
    detail::putVec(w, in.rightStick);
    for (const auto& t : in.touches) { w.f32(t.x); w.f32(t.y); }
    w.i32(in.touchCount);
    detail::putVec(w, in.mouseDelta);
    w.boolean(in.mouseLeft);
    w.boolean(in.mouseRight);
    w.i32(in.mouseWheel);
    detail::putBools(w, in.keys);
    detail::putBits(w, in.chars);
    w.boolean(in.shift);
}
inline RawInput readRawInput(state::Reader& r) {
    RawInput in;
    in.buttons = r.u16();
    in.leftStick = detail::getVec(r);
    in.rightStick = detail::getVec(r);
    for (auto& t : in.touches) { t.x = r.finite32(); t.y = r.finite32(); }
    in.touchCount = r.ranged<std::int32_t>(0, static_cast<std::int32_t>(in.touches.size()), "touch count");
    in.mouseDelta = detail::getVec(r);
    in.mouseLeft = r.boolean();
    in.mouseRight = r.boolean();
    in.mouseWheel = r.i32();
    detail::getBools(r, in.keys);
    in.chars = detail::getBits(r);
    in.shift = r.boolean();
    return in;
}

// Staged mapper state (everything but the settings).
struct MapperState {
    PcState state;
    bool cursorInit = false;
    RawInput prevPad;
    std::array<bool, KeyCount> prevKeys{};
    std::bitset<128> prevChars;
    std::array<bool, KeyCount> keyPulse{};
    bool leftPulse = false, rightPulse = false;
    Vec2 aimDir, aimOffset;
    bool aimFree = false, driving = false;
    Vec2 orderDir;
    float orderClock = 0;
    bool weaponPending = false, weaponTracked = false;
    float weaponAge = 0, weaponWait = 0;
    float accelHold = 0;
    Vec2 accelDir;
    std::uint8_t touchPhase = 0;
    Vec2 touchStart, touchLast;
    float touchHeld = 0;
    float touchGrace = 0;
    int repeatDir = 0;
    float repeatHeld = 0;
};

struct SaveStateAccess {
    static constexpr std::uint32_t Tag = state::fourcc("INP2");

    static void save(state::Writer& w, const Mapper& m) {
        const std::size_t chunk = w.beginChunk(Tag);
        writePcState(w, m.m_state);
        w.boolean(m.m_cursorInit);
        writeRawInput(w, m.m_prevPad);
        detail::putBools(w, m.m_prevKeys);
        detail::putBits(w, m.m_prevChars);
        detail::putBools(w, m.m_keyPulse);
        w.boolean(m.m_leftPulse);
        w.boolean(m.m_rightPulse);
        detail::putVec(w, m.m_aimDir);
        detail::putVec(w, m.m_aimOffset);
        w.boolean(m.m_aimFree); w.boolean(m.m_driving);
        detail::putVec(w, m.m_orderDir);
        w.f32(m.m_orderClock);
        w.boolean(m.m_weaponPending); w.boolean(m.m_weaponTracked);
        w.f32(m.m_weaponAge); w.f32(m.m_weaponWait);
        w.f32(m.m_accelHold);
        detail::putVec(w, m.m_accelDir);
        w.u8(static_cast<std::uint8_t>(m.m_touch.phase));
        detail::putVec(w, m.m_touch.start);
        detail::putVec(w, m.m_touch.last);
        w.f32(m.m_touch.held);
        w.f32(m.m_touchGrace);
        w.i32(m.m_repeatDir);
        w.f32(m.m_repeatHeld);
        w.endChunk(chunk);
    }

    static MapperState decode(state::Reader& outer) {
        state::Reader r = outer.chunk(Tag);
        MapperState s;
        s.state = readPcState(r);
        s.cursorInit = r.boolean();
        s.prevPad = readRawInput(r);
        detail::getBools(r, s.prevKeys);
        s.prevChars = detail::getBits(r);
        detail::getBools(r, s.keyPulse);
        s.leftPulse = r.boolean();
        s.rightPulse = r.boolean();
        s.aimDir = detail::getVec(r);
        s.aimOffset = detail::getVec(r);
        s.aimFree = r.boolean(); s.driving = r.boolean();
        s.orderDir = detail::getVec(r);
        s.orderClock = r.finite32();
        s.weaponPending = r.boolean(); s.weaponTracked = r.boolean();
        s.weaponAge = r.finite32(); s.weaponWait = r.finite32();
        s.accelHold = r.finite32();
        s.accelDir = detail::getVec(r);
        s.touchPhase = r.ranged<std::uint8_t>(0, 3, "touch phase");
        s.touchStart = detail::getVec(r);
        s.touchLast = detail::getVec(r);
        s.touchHeld = r.finite32();
        s.touchGrace = r.finite32();
        s.repeatDir = r.ranged<std::int32_t>(-1, 1, "repeat direction");
        s.repeatHeld = r.finite32();
        r.expectEnd("input mapper");
        return s;
    }

    static void apply(Mapper& m, MapperState&& s) {
        m.m_state = std::move(s.state);
        m.m_cursorInit = s.cursorInit;
        m.m_prevPad = s.prevPad;
        m.m_prevKeys = s.prevKeys;
        m.m_prevChars = s.prevChars;
        m.m_keyPulse = s.keyPulse;
        m.m_leftPulse = s.leftPulse;
        m.m_rightPulse = s.rightPulse;
        m.m_aimDir = s.aimDir;
        m.m_aimOffset = s.aimOffset;
        m.m_aimFree = s.aimFree; m.m_driving = s.driving;
        m.m_orderDir = s.orderDir;
        m.m_orderClock = s.orderClock;
        m.m_weaponPending = s.weaponPending; m.m_weaponTracked = s.weaponTracked;
        m.m_weaponAge = s.weaponAge; m.m_weaponWait = s.weaponWait;
        m.m_accelHold = s.accelHold;
        m.m_accelDir = s.accelDir;
        m.m_touch.phase = static_cast<Mapper::TouchTracker::Phase>(s.touchPhase);
        m.m_touch.start = s.touchStart;
        m.m_touch.last = s.touchLast;
        m.m_touch.held = s.touchHeld;
        m.m_touchGrace = s.touchGrace;
        m.m_repeatDir = s.repeatDir;
        m.m_repeatHeld = s.repeatHeld;
    }
};

} // namespace gromada::input
