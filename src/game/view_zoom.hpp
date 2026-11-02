#pragma once
// Contract of the analog view zoom (wave 4, see docs/architecture.md "Zoom e risoluzione").
//
//  - The INPUT layer (src/input, controls issue) only produces a zoom *rate* per frame in
//    input::PcState::zoomRate: -1..+1, analog (L2/R2 trigger pressure in the twin-stick
//    scheme; A/B held = -1/+1 in the cursor scheme; 0 in menus). +1 = zoom in (closer).
//  - The SESSION owns one ViewZoom, integrates it every frame with the real frame time and
//    hands zoom() to the rendering pipeline (rendering issue), which decides how many world
//    pixels are visible (view size = output size / zoom) and maps screen <-> world for the
//    cursor, touch and the input record.
//  - The zoom is a presentation setting: it never changes the simulation (demos, oracle and
//    save-state determinism are unaffected). It is not part of the original game.
//
// Header-only, no dependencies beyond the standard library.

#include <algorithm>
#include <cmath>

namespace gromada::game {

struct ViewZoom {
    static constexpr float Min = 0.5f;   // 0.5x: twice as much map in each direction
    static constexpr float Max = 2.0f;   // 2x: closer
    // Full-rate speed: one doubling (or halving) per this many seconds. Exponential, so the
    // zoom feels the same at every level.
    static constexpr float SecondsPerDoubling = 0.6f;

    float zoom = 1.0f;

    // rate in -1..1 (analog), dt in seconds (frame time: same feel at 60 and 120 fps).
    void integrate(float rate, float dt) {
        rate = std::clamp(rate, -1.0f, 1.0f);
        if (rate == 0.0f || dt <= 0.0f)
            return;
        zoom = std::clamp(zoom * std::exp2(rate * dt / SecondsPerDoubling), Min, Max);
    }
    void reset() { zoom = 1.0f; }
};

} // namespace gromada::game
