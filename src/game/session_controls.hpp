#pragma once
// Game session, controls on the real gameplay (issue #16). Helpers the Session uses so that
// the console controls (src/input) work well on the original game:
//  - WeaponSlots: which of the four weapons of the Kassandra can be selected (script globals
//    weapon[0..3] + ammunition from the engine, the same test as HEADER.INI's Tact()) and
//    which one is selected (nweapon);
//  - FollowCamera: the view follows the player (dead zone + look-ahead in the direction of
//    travel), for the direct scheme and touch, where the cursor cannot scroll at the edges;
//  - WeaponHud: shows the selected weapon for a moment after a change when the status bar
//    (which shows it in the original) is hidden.
// Header-only; depends on the engine contract and src/input types.

#include "core/std.hpp"
#include "game/engine_api.hpp"
#include "input/mapper.hpp"

#include <cmath>

namespace gromada::game {

// Per frame information for the input mapper (input::FrameInfo), from the engine and the
// script. Screen coordinates are those of the output image.
struct ControlsFrame {
    std::optional<input::Vec2> player;       // flagman on the output (ground point)
    std::vector<input::AimTarget> targets;   // attackable units, on the output
    float weaponRange = 0;                   // world px, Euclidean (PlayerAim); 0 = unknown
    input::WeaponKind weaponKind = input::WeaponKind::Unknown; // lock corridor (issue #19)
    float aimLift = 0;                       // 80 for anti-aircraft weapons (input::FrameInfo)
    std::uint8_t weaponMask = 0x0F;          // bit k: weapon k+1 selectable
    int currentWeapon = -1;                  // 0..3, -1 = no flagman / not a level
    // for logs and tests
    int flagmanX = 0, flagmanY = 0;          // world
    int targetCount = 0, targetHp = 0;       // all hostile units of the map
    std::vector<int> targetHps;              // hp of each target (same order as `targets`)
};

// The weapon's kind for the invisible lock (issue #19), from the data of what its head fires
// (PlayerAim::FireChain), never from names. Player heads (vid: fire chain) and their kind:
//   HEPHEST 88 / HELIOS 202 laser, PHOEBUS 89 / THUNDER 203 cannon, VULCAN 208 machine gun,
//   PHOENIX 207 flamethrower: a projectile, flat (speedY < speedX) or instantaneous -> Direct
//   HAIL 91: projectile with speedY 400 >= speedX 200 (high arc)                     -> Ballistic
//   ZEPHYR 90 (CannonMissile), HARPY 204 (AviaMissile, homing), HYDRA 209: flags 1   -> Missile
//   PLUTO 210: ballistic projectile whose impact (act_death) leaves a Mine           -> MineLayer
//   CHAOS 205: a Mine straight from the head, flags 4 (fixed head)                   -> FixedMineLayer
inline input::WeaponKind weaponKind(const PlayerAim::FireChain& c) {
    using K = input::WeaponKind;
    constexpr int Projectile = 5, CannonMissile = 6, Mine = 13, AviaMissile = 15; // cls::* (game/sprite.hpp)
    constexpr int Instant = 9999;                                                  // vid speed "instantaneous"
    if (c.headNvid < 0)
        return K::Unknown;
    if (c.classes & (1u << Mine))
        return (c.flags & 4) ? K::FixedMineLayer : K::MineLayer;
    if ((c.flags & 1) || (c.classes & ((1u << CannonMissile) | (1u << AviaMissile))))
        return K::Missile;
    if (c.projectileClass < 0)
        return K::Unknown;
    if (c.projectileClass == Projectile && c.projectileSpeedX < Instant && c.projectileSpeedY >= c.projectileSpeedX)
        return K::Ballistic;
    return K::Direct;
}

struct WeaponSlots {
    std::uint8_t mask = 0;   // bit k: weapon[k] set and (k == 0 or ammunition left)
    int current = -1;        // nweapon
    std::array<int, 4> nvid{}; // head vid of each slot (0 = empty)
};

// HEADER.INI Tact(): keys '1'..'4' select weapon[k] (a head vid) if set; a weapon other than
// the first one without ammunition goes back to weapon[0]. The ammunition index of a head is
// its offset from V_FLAGMANHEAD11 (88), 12 (202) or 13 (207), five heads per series.
template <class GlobalInt, class GetAmmo>
WeaponSlots weaponSlots(GlobalInt&& globalInt, GetAmmo&& ammo) {
    constexpr int Head11 = 88, Head12 = 202, Head13 = 207;
    WeaponSlots w;
    w.current = std::clamp(static_cast<int>(globalInt("nweapon", 0)), 0, 3);
    for (int k = 0; k < 4; ++k) {
        const int head = globalInt("weapon", k);
        w.nvid[static_cast<std::size_t>(k)] = head;
        if (!head)
            continue;
        const int index = head < Head11 + 5 ? head - Head11 : head < Head12 + 5 ? head - Head12 : head - Head13;
        if (k == 0 || ammo(index) > 0)
            w.mask |= static_cast<std::uint8_t>(1u << k);
    }
    return w;
}

// The view follows a point (the player) like a console action game: nothing moves while the
// player stays in the central dead zone, then the view catches up exponentially (frame-rate
// independent), leading in the direction of travel. The engine still applies its own rules
// afterwards (edge scroll, leash, map limits): when they move the view, the follower takes
// the new position as its own.
class FollowCamera {
public:
    friend struct SaveStateAccess; // save states (game/savestate.hpp)
    struct Params {
        float deadZone = 0.10f;  // fraction of the view size, each side of the centre
        float rate = 4.0f;       // 1/s: catch-up speed on the part outside the dead zone
        float lookAhead = 1.5f;  // s of the player's velocity added to the target point
        float maxLead = 0.28f;   // fraction of the view size
        float velocityTau = 0.25f; // s: smoothing of the measured velocity
        // Driving at 100 px/s the player settles about 60 px behind the centre of a 640 px
        // view: lead 150 - dead zone 64 - lag 100/rate 25.
    };

    void reset() { m_valid = false; m_hold = false; }
    void setParams(const Params& p) { m_params = p; }
    [[nodiscard]] bool holding() const noexcept { return m_hold; }
    // The view's corner with sub-pixel precision while `cam` is still where update() put it
    // (issue #20: smooth scrolling); nullopt when another rule moved it since.
    [[nodiscard]] std::optional<std::pair<float, float>> position(const Camera& cam) const noexcept {
        if (!m_valid || cam.x != m_lastX || cam.y != m_lastY) return std::nullopt;
        return std::pair{m_fx, m_fy};
    }

    // `userScroll`: the player is scrolling with the cursor at the screen edge; following
    // pauses until the player's vehicle moves again.
    // (px, py): the player, world pixels (sub-pixel where it is drawn with smooth motion).
    void update(Camera& cam, float px, float py, float dt, bool userScroll) {
        const Params& p = m_params;
        if (!m_valid || cam.x != m_lastX || cam.y != m_lastY) {
            m_fx = static_cast<float>(cam.x);
            m_fy = static_cast<float>(cam.y);
        }
        if (!m_valid || std::abs(px - m_px) > 200 || std::abs(py - m_py) > 200) {
            m_vx = m_vy = 0; // first frame, new map, teleport
        } else if (dt > 0) {
            const float a = 1 - std::exp(-dt / p.velocityTau);
            m_vx += ((px - m_px) / dt - m_vx) * a;
            m_vy += ((py - m_py) / dt - m_vy) * a;
        }
        m_px = px;
        m_py = py;
        m_valid = true;
        if (userScroll)
            m_hold = true;
        else if (m_hold && std::hypot(m_vx, m_vy) > 30)
            m_hold = false;
        if (!m_hold && dt > 0) {
            const float w = static_cast<float>(cam.width), h = static_cast<float>(cam.height);
            const float leadX = std::clamp(m_vx * p.lookAhead, -p.maxLead * w, p.maxLead * w);
            const float leadY = std::clamp(m_vy * p.lookAhead, -p.maxLead * h, p.maxLead * h);
            const float k = 1 - std::exp(-p.rate * dt);
            auto axis = [&](float& pos, float size, float target) {
                const float e = target - (pos + size / 2);
                const float dz = p.deadZone * size;
                const float excess = e > dz ? e - dz : e < -dz ? e + dz : 0.0f;
                pos += excess * k;
            };
            axis(m_fx, w, px + leadX);
            axis(m_fy, h, py + leadY);
        }
        cam.x = static_cast<int>(std::lround(m_fx));
        cam.y = static_cast<int>(std::lround(m_fy));
        m_lastX = cam.x;
        m_lastY = cam.y;
    }

private:
    Params m_params;
    bool m_valid = false, m_hold = false;
    float m_fx = 0, m_fy = 0, m_vx = 0, m_vy = 0;
    float m_px = 0, m_py = 0;   // the player at the last update
    int m_lastX = 0, m_lastY = 0;
};

// Selected weapon, shown for `duration` seconds after it changes (or is requested) when the
// status bar is hidden: "2 ZEPHYR: LANCIAMISSILI  12".
class WeaponHud {
public:
    friend struct SaveStateAccess; // save states (game/savestate.hpp)
    static constexpr float Duration = 2.0f;

    // Returns true while the indicator should be drawn. `poke`: a weapon key was pressed
    // (shown even when nothing changes, e.g. a single weapon available).
    bool update(int current, int requested, float dt, bool poke = false) {
        if (poke && current >= 0)
            m_timer = Duration;
        if (current != m_current || requested != m_requested) {
            if (m_current >= 0 && current >= 0)
                m_timer = Duration; // not on the first frame of a level
            m_current = current;
            m_requested = requested;
        }
        m_timer = std::max(0.0f, m_timer - dt);
        return m_timer > 0 && current >= 0;
    }
    void reset() { m_current = m_requested = -1; m_timer = 0; }
    [[nodiscard]] float remaining() const noexcept { return m_timer; }

    // "ZEPHYR   : LANCIAMISSILI" -> "ZEPHYR: LANCIAMISSILI"
    static std::string label(int slot, std::string_view vidName, int ammo, bool pending) {
        std::string name;
        for (const char c : vidName) {
            if (c == '\0') break;
            if (c == ' ' && (name.empty() || name.back() == ' ')) continue;
            name += c;
        }
        while (!name.empty() && name.back() == ' ') name.pop_back();
        for (std::size_t i; (i = name.find(" :")) != std::string::npos;) name.erase(i, 1);
        std::string s = std::format("{} {}", slot + 1, name);
        if (ammo >= 0) s += std::format("  {}", ammo);
        if (pending) s += " ...";
        return s;
    }

private:
    int m_current = -1, m_requested = -1;
    float m_timer = 0;
};

} // namespace gromada::game
