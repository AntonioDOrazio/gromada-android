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

// What StateBar::Tact (0x416de0) reads from the world for the local player's status bar
// (issue #15). Colours are 16-bit RGB565 values of the original (game/hicolor.hpp).
struct StateBarInfo {
    int hp = 0;                  // flagman hp (sprite +0x3f)
    int maxHp = 0;               // vid maxHP (+0x2d); 0 = the hp bar is reset to direction 0
    int turretNvid = -1;         // the flagman's sub-object (weapon head), -1 none
    std::string turretName;      // its vid name (CP866 bytes)
    std::array<int, 5> ammo{};   // s16 counters at +0x54 (the fifth is the jumps)
    struct Dot { int dx = 0, dy = 0; std::uint16_t color = 0; };
    std::vector<Dot> radar;      // objects around the flagman: world offset, colour
    int enemyDirection = -1;     // GetDirection to the nearest enemy unit (<= 60000), -1 none
};

struct Camera {
    int x = 0, y = 0;            // top-left corner of the view, world pixels
    int width = 640, height = 480;
};

// What the local player can shoot at (issue #16: aim assist of the direct control scheme,
// weapon range for the aim point, automated tests). World coordinates.
struct PlayerAim {
    struct Target {
        UnitId id = 0;
        int x = 0, y = 0;             // a point that puts the unit under the cursor (cursorAt → Fight)
        int groundX = 0, groundY = 0; // the unit on the ground (distances, ranges)
        int hp = 0;
    };
    // What the current weapon head fires (issue #19: the controls classify the weapon from
    // the data, e.g. a wider lock corridor for slow or inaccurate weapons). The chain starts
    // at the head's act_fight children and follows, for up to 6 objects, the act_fight child
    // of each object and the act_death (impact) child of the projectiles.
    struct FireChain {
        int headNvid = -1;               // the flagman's sub-object (weapon head), -1 none
        std::uint8_t flags = 0;          // head weapon flags: 1 fires without turning, 4 fixed head
        std::uint8_t targetCategory = 0; // head weapon target categories (cat::*)
        std::uint32_t classes = 0;       // bit c: an object of class c (cls::*) is in the chain
        int projectileClass = -1;        // class of the first object that flies or lies (-1 none)
        int projectileSpeedX = 0, projectileSpeedY = 0; // its vid speeds (9999 = instantaneous)
    };
    bool valid = false;          // false: no local flagman (menus, cutscenes)
    int flagmanX = 0, flagmanY = 0;
    // Current weapon range. The flagman's shots (act_coor_attack → Aim 0x40ada0) compare it
    // with GetDirection's distance, which is Euclidean: a farther point is pulled back to
    // this distance along the same direction. (AI units use the game metric max(|dx|, 1.5|dy|).)
    int weaponRange = 0;         // 0 = unknown
    std::vector<Target> targets; // hostile units the current weapon can hit, anywhere on the map
    FireChain fire;              // (default: nothing known)
};

// ---- presentation (issue #20, docs/rendering.md) ------------------------------------------
// A sprite as drawView() drew it (enhancement passes: lights, ambient occlusion...).
struct DrawnSprite {
    UnitId id = 0;
    int nvid = 0;
    std::uint8_t cls = 0;        // ObjectClass of the vid (game/sprite.hpp cls::)
    std::uint8_t category = 0;   // vid category bits (cat::)
    bool screen = false;         // screen sprite (st_screen): screen coordinates, not zoomed
    bool child = false;          // sub-object drawn with the root before it
    float worldX = 0, worldY = 0, worldZ = 0;   // drawn position (sub-pixel, extrapolated), world px
    float fbX = 0, fbY = 0;      // ground point (worldX, worldY) on the framebuffer
    int left = 0, top = 0, right = 0, bottom = 0; // body rectangle on the framebuffer [left, right)
};

// What the session asks drawView() for. The view shows `width` x `height` world pixels; one
// world pixel is `scale` framebuffer pixels (zoom x internal resolution).
struct ViewRequest {
    double centerX = 320, centerY = 240;   // world point the session wants at the centre
    double width = 640, height = 480;      // world pixels visible
    double scale = 1;                      // framebuffer pixels per world pixel
    double screenScale = 1;                // framebuffer pixels per screen pixel (screen sprites)
    double renderClock = 0;                // game clock (ms, fractional) the picture shows
    bool extrapolate = false;              // smooth motion: positions extrapolated to renderClock
    bool fitMap = false;                   // keep the view inside the map (centred if the map is smaller)
    std::vector<DrawnSprite>* drawn = nullptr; // filled by drawView() when set
    // Runs fn(0) .. fn(n - 1), possibly on several threads, and returns when all are done
    // (the session's workers); with `threads` > 1 drawView() may split the picture into that
    // many bands (same picture for any number). Empty: one thread.
    std::function<void(int n, const std::function<void(int)>& fn)> parallel;
    int threads = 1;
    // The framebuffer still holds what the previous drawView() left in it (same buffer, nothing
    // else wrote it since): drawView() may then only redraw what changed in place.
    bool targetIntact = false;
};
struct ViewOrigin {
    double x = 0, y = 0;                   // world point at the view's top-left corner
};
// The Effect() picture operation of the last drawScreenEffects() (pipelines that redraw the
// world at a higher resolution apply it there too).
struct ScreenEffectView {
    int colorKind = 0, colorLevel = 0;     // ScreenEffects::applyOp(kind, level, c); 0 = none
    bool window = false;                   // eff_openwindow: outside the rectangle, the old screen
    int left = 0, right = -1, top = 0, bottom = -1;   // inclusive, picture pixels
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

    // --- optional hooks (default no-ops: engines may ignore them) ----------------------
    // ScrollLock(flag) is a session builtin (73) but the state it sets (obj+0x32d5) stops
    // the engine's edge scrolling, the leash around the flagman and the +/- view keys
    // (docs/re/runtime.md §6). The session calls this whenever a script changes it and
    // with `false` after every loadMap (the original clears it on map load).
    virtual void setScrollLock(bool locked) { (void)locked; }
    // Demo playback starts from the clock recorded in the .dem (0x414e8b) instead of the
    // map's startTimer: call right after loadMap().
    virtual void setClock(std::uint32_t curclock) { (void)curclock; }
    // Full-screen effects of Effect() (eff_fade, eff_nuke...: palette/flash effects applied
    // in the original's EndFrame 0x406ea0 to the whole picture). Called by the session
    // after it has drawn menus, texts and the status bar over draw()'s output, just
    // before the cursor.
    virtual void drawScreenEffects(FramebufferRef framebuffer) { (void)framebuffer; }
    // Status bar data of the local player, read at the time the original runs StateBar::Tact
    // (inside Player::Tact, right after tactPlayers()). nullopt when the original would not
    // run it: no flagman, paused, or an engine that does not provide it.
    [[nodiscard]] virtual std::optional<StateBarInfo> stateBarInfo() { return std::nullopt; }
    // Local player's flagman, weapon range and attackable units (issue #16, read-only query
    // for the controls: aim assist and the aim point at weapon range). Default: nothing known.
    [[nodiscard]] virtual PlayerAim playerAim() const { return {}; }

    // --- presentation (issue #20, docs/rendering.md): zoom, internal resolution, smooth
    // motion. Default: not supported, the session uses camera() and draw() (zoom 1).
    // The view's final top-left corner for `request` (the engine may move the requested
    // centre: leash around the flagman at its drawn position, map limits, small maps centred).
    [[nodiscard]] virtual std::optional<ViewOrigin> viewOrigin(const ViewRequest& request) { (void)request; return std::nullopt; }
    // Draws the world like draw() through the view transform: framebuffer pixel =
    // (world - origin) * scale; screen sprites at screen position * screenScale. False: not
    // supported (nothing drawn).
    virtual bool drawView(FramebufferRef framebuffer, const ViewRequest& request, ViewOrigin origin) {
        (void)framebuffer; (void)request; (void)origin;
        return false;
    }
    [[nodiscard]] virtual ScreenEffectView screenEffectView() const { return {}; }
    // Where drawView() with `extrapolate` draws a unit at `renderClock` (world, sub-pixel):
    // the camera that follows the player aims at it. nullopt: no such unit / not supported.
    [[nodiscard]] virtual std::optional<std::array<double, 2>> drawnPosition(UnitId unit, double renderClock) const {
        (void)unit; (void)renderClock;
        return std::nullopt;
    }
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
