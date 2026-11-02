#pragma once
// Contract between the simulation engine (issue #8) and the game session (issue #10).
// See docs/architecture.md for responsibilities, frame order and the builtin split.
//
// Changing this file: keep it backward compatible while both branches are open; if a
// change is unavoidable, describe it in the final report so the orchestrator can adapt
// the other side at merge time.

#include "core/std.hpp"
#include "libgromada/software_renderer.hpp"
#include "script/value.hpp"

namespace gromada::game {

// Unit handle as seen by scripts. 0 = no unit. (The original used sprite pointers; any
// stable non-zero integer works, but handles must not be reused while a script may still
// hold them during the same map.)
using UnitId = std::int32_t;

// The original per-frame input record (docs/re/runtime.md §6, 12 bytes at 0x42bf88,
// copied at the start of every frame). Demo files replay exactly this.
struct OriginalInput {
    std::int16_t mouseWorldX = 0, mouseWorldY = 0;    // screen position + view origin
    std::int16_t mouseScreenX = 0, mouseScreenY = 0;
    std::uint16_t bits = 0;                           // GetInputState() bits, see below
    std::uint8_t key = 0;                             // GetKey(): WM_CHAR code, arrows 1..4, Del 5

    static constexpr std::uint16_t LClick = 0x01, RClick = 0x02, Left = 0x04, Right = 0x08,
                                   LDown = 0x10, RDown = 0x20, Up = 0x40, Down = 0x80, Shift = 0x100;
};

// Original cursor indices (docs/re/runtime.md §6).
enum class CursorKind : std::uint8_t { Arrow = 0, Move = 1, Fight = 2, NoMove = 3, Cycle = 4 };

// A sound the engine wants played this frame (vid nsfx tables, explosions...). World
// coordinates let the session pan/attenuate relative to the camera.
struct SoundEvent {
    int nsfx = 0;
    int x = 0, y = 0;
};

struct Camera {
    int x = 0, y = 0;            // top-left corner of the view, world pixels
    int width = 640, height = 480;
};

class Engine {
public:
    virtual ~Engine() = default;

    // --- map lifecycle ---------------------------------------------------------------
    // Loads a level/menu .map (objects, armies, header incl. startTimer and observer),
    // resets the clock to the map's startTimer and seeds the shared MSVC rand() state.
    virtual void loadMap(const std::filesystem::path& mapFile, std::uint32_t rngSeed) = 0;
    virtual void unloadMap() = 0;

    // --- frame phases, called by the session in the original order -------------------
    // (docs/re/engine.md §1.3: input → script main() → world → UI/scroll → players →
    //  group AI → camera clamp → sounds). The session runs main() between beginFrame and
    //  tactWorld.
    virtual void beginFrame(int elapsedMs) = 0;           // curclock += min(elapsed, 71) unless paused
    virtual void setInput(const OriginalInput& input) = 0; // input of this frame
    virtual void tactWorld() = 0;                          // sprites whose tick is due (World::Tact)
    virtual void tactPlayers() = 0;                        // player commands to the flagman(s)
    virtual void tactGroupAi() = 0;                        // squad/army AI (0x416730)
    virtual void updateCamera() = 0;                       // edge scroll / auto scroll / clamp

    // Draws the world as the original does (z layers, painter order, shadows, gamma).
    virtual void draw(FramebufferRef framebuffer) = 0;

    // --- queries used by the session ---------------------------------------------------
    [[nodiscard]] virtual int clock() const = 0;              // GetTime(), ms
    [[nodiscard]] virtual Camera& camera() = 0;
    [[nodiscard]] virtual CursorKind cursorAt(int screenX, int screenY) const = 0;
    [[nodiscard]] virtual std::vector<SoundEvent> takeSounds() = 0;

    // --- script builtins owned by the engine (IDs listed in docs/architecture.md) ------
    // Returns nullopt if `id` is not an engine builtin. The returned Value is what the
    // script sees (builtins documented as returning nothing may return Value{0}).
    virtual std::optional<script::Value> callBuiltin(int id, std::span<const script::Value> args) = 0;

    // Shared MSVC CRT rand(): scripts' Random() and the engine draw from the same state.
    virtual int rand() = 0;

    // ScrollLock(flag) (builtin 73, owned by the session) stops the edge scroll, the
    // auto-scroll and the leash around the flagman, which live in updateCamera().
    // Added by issue #8 with a default no-op so other implementations keep compiling.
    virtual void setScrollLock(bool /*locked*/) {}

    // Demo playback starts from the clock recorded in the .dem (0x414e8b) instead of the
    // map's startTimer: call right after loadMap(). Added by issue #8, default no-op.
    virtual void setClock(std::uint32_t /*curclock*/) {}
};

// Engine builtin IDs (extern IDs of maps/HEADER.INI). Everything else belongs to the session.
inline constexpr std::array<int, 28> EngineBuiltinIds = {
    65,  // SizeTo
    67,  // Action
    68,  // Create
    69,  // GetTime
    79,  // MaxUnit
    80,  // Effect
    85,  // Flagman
    86,  // GetObject
    87,  // AddCommand
    89,  // Pause
    90,  // Random
    94,  // GetUnitVid
    95,  // FirstUnit
    96,  // NextUnit
    106, // SetGamma
    107, // SetWind
    108, // CreateLight
    109, // GetX
    110, // GetY
    119, // SetFlagman
    120, // Save (writes a .map)
    121, // GetCommands
    122, // SetCommands
    123, // AskPlace
    124, // GetVidName
    126, // SetPosition (camera)
    127, // SetVid
    128, // SetAutoScroll
};

[[nodiscard]] constexpr bool isEngineBuiltin(int id) noexcept {
    return std::ranges::find(EngineBuiltinIds, id) != EngineBuiltinIds.end();
}

} // namespace gromada::game
