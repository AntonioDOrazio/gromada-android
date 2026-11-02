#pragma once
// Game session (issue #10): runs the original game the way gromada.exe does, on top of the
// simulation engine contract game::Engine (src/game/engine_api.hpp) and the script
// interpreter (src/script). See docs/session.md.
//
// One Session = one running game. Per output frame the core calls runFrame(pc, dt) with the
// input of the frame (input::PcState from src/input) and gets a picture (xrgb()) and sounds
// (through the Mixer). Inside a frame, in the order of the original (docs/architecture.md,
// docs/re/engine.md §1.3):
//   1. engine.beginFrame(elapsed ms)            game clock (clamped to 71 ms by the engine)
//   2. engine.setInput(record)                  the 12-byte record: controller or demo
//   3. script main()                            the interpreter calls back callBuiltin()
//   4. engine.draw() + menus/texts/status bar   the original draws before updating
//      engine.tactWorld()                       sprites whose tick is due
//   5. menu update (0x40c1d0)                   may consume the click: engine.setInput() again
//      engine.tactPlayers(), tactGroupAi(), updateCamera(); sounds
//   6. after the frame: the Load()/PlayAVI()/SetGraphMode()/Exit() posted by the script.
// While a movie plays nothing else runs (the original's loop does not call the tick).
//
// Builtins: the 28 of EngineBuiltinIds go to Engine::callBuiltin, except Action/GetX/GetY/
// GetUnitVid on handles of session objects (menu objects and texts, see session_ui.hpp);
// all the others are implemented here (callBuiltin below, docs/re/runtime.md §3).
//
// Screen spaces: the "view" is what the engine draws (the whole output, 640 or 854 x 480;
// 640 x 480 centred while a demo plays, like the original). Menus and texts are laid out by
// the scripts for their screen (640x480, or 800x600 after SetGraphMode(2)) and drawn centred
// in the view; the status bar is anchored to the bottom of the view.
//
// Header-only. Depends on the engine contract (not on any engine implementation), script/,
// ui/, input/, demo/, engine/audio.hpp (Mixer).

#include "core/std.hpp"
#include "engine/audio.hpp"
#include "game/engine_api.hpp"
#include "game/sound_channels.hpp"
#include "game/session_demo.hpp"
#include "game/session_files.hpp"
#include "game/session_controls.hpp"
#include "game/session_input.hpp"
#include "game/session_ui.hpp"
#include "input/cursor_file.hpp"
#include "input/mapper.hpp"
#include "script/script.hpp"

#include <deque>
#include <functional>

namespace gromada::game {

// Movie playback used by PlayAVI (implemented by the core on top of src/video, faked in tests).
class MoviePlayer {
public:
    virtual ~MoviePlayer() = default;
    virtual bool open(const std::filesystem::path& file) = 0;  // false: cannot be played
    virtual bool step(double dt) = 0;                           // false once finished
    virtual void blit(std::uint32_t* xrgb, int width, int height, std::size_t pitchPixels) = 0;
    virtual AudioStream* audio() = 0;
    virtual void close() = 0;
    // Save states (issue #14): the playback clock, and moving the clock to `seconds` (forward:
    // frames are decoded up to it; backward: the clock moves, the picture waits for it).
    // Players without them make save states fail while a movie plays.
    [[nodiscard]] virtual std::optional<double> position() const { return std::nullopt; }
    virtual bool seek(double seconds) { (void)seconds; return false; }
};

enum class LogLevel : std::uint8_t { Debug, Info, Warn, Error };

struct SessionConfig {
    std::filesystem::path gameDir;   // original data (read-only)
    std::filesystem::path saveDir;   // writable: save1..5, fw.ini, demos (empty: memory only)
    int outputWidth = 640, outputHeight = 480;
    bool render = true;              // false: no drawing (tests, fast demo checks)
    // Declare every known builtin before compiling. Only for synthetic scripts that do not
    // include maps\header.ini: the game's scripts declare their builtins themselves, and
    // declaring them twice exceeds the original compiler's 128-function limit.
    bool predeclareBuiltins = false;
    std::function<void(LogLevel, std::string_view)> log;
};

class Session final : public script::ScriptHost {
public:
    friend struct SaveStateAccess; // save states (game/savestate.hpp)
    static constexpr std::string_view DefaultStartMap = "maps\\logo.map";
    static constexpr int MaxFrameMs = 71;

    Session(std::span<const Vid> vids, std::span<const SoundData> sounds, std::unique_ptr<Engine> engine, Mixer* mixer,
            std::unique_ptr<MoviePlayer> movies, SessionConfig config)
        : m_vids{vids}, m_sounds{sounds}, m_engine{std::move(engine)}, m_mixer{mixer}, m_movies{std::move(movies)},
          m_config{std::move(config)}, m_files{m_config.gameDir, m_config.saveDir}, m_ui{vids},
          m_loader{m_config.gameDir} {
        if (const int n = m_files.seed())
            log(LogLevel::Info, std::format("save directory {}: copied {} original file(s)", m_config.saveDir.string(), n));
        setOutputSize(m_config.outputWidth, m_config.outputHeight);
        m_builtins = script::knownBuiltinRegistry();
        m_sfxSink.mixer = m_mixer;
        m_sfxSink.session = this;
        applyVolumes();
        m_ui.setRandom([this] { return m_engine ? m_engine->rand() : 0; });
    }

    ~Session() override {
        finishRecording();
        m_files.closeAll();
        stopMovie();
        if (m_mixer)
            m_mixer->stopSfx(); // voices may point at PlayWAV sounds owned here
        if (m_engine)
            m_engine->unloadMap();
    }

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // ---- setup ----------------------------------------------------------------------------
    // Loads the first map (maps\logo.map, or a level like the original's command line).
    void start(std::string_view map = DefaultStartMap) { loadMap(std::string{map}); }

    void setOutputSize(int width, int height) {
        m_outW = std::clamp(width, 320, 1024);
        m_outH = std::clamp(height, 240, 768);
        m_view.assign(static_cast<std::size_t>(m_outW) * m_outH, RGBA8{{0, 0, 0}, 255});
        m_xrgb.assign(static_cast<std::size_t>(m_outW) * m_outH, 0);
        updateView();
    }
    // Core option volumes (0..1), multiplied by the script's SetMusic/SetSound.
    void setVolumes(float music, float sfx) {
        m_coreMusic = std::clamp(music, 0.0f, 1.0f);
        m_coreSfx = std::clamp(sfx, 0.0f, 1.0f);
        applyVolumes();
    }
    void setCursors(const input::CursorSet* cursors) { m_cursors = cursors; }
    void setRender(bool render) { m_config.render = render; }
    void setKeyRepeat(KeyRepeat r) { m_inputBuilder.setRepeat(r); }

    // Starts maps\demoN.dem now (core option / tests), like Load() from a script.
    void playDemo(int n) {
        stopMovie();
        m_ops.clear();
        loadMap(std::format("maps\\demo{}.dem", n));
    }

    // ---- per frame ----------------------------------------------------------------------------
    void runFrame(const input::PcState& rawPc, double dtSeconds) {
        const double dtMs = std::max(0.0, dtSeconds) * 1000.0;
        m_realMs += dtMs;
        const input::PcState& pc = filterStale(rawPc);
        m_pc = pc;
        ++m_frames;
        if (m_exit)
            return;

        if (m_movieActive) {
            const bool skip = anyPress(pc);
            if (skip || !m_movies->step(dtSeconds)) {
                stopMovie();
                std::ranges::fill(m_xrgb, 0u);
                return; // the skipping press does not reach the game
            }
            m_movies->blit(m_xrgb.data(), m_outW, m_outH, static_cast<std::size_t>(m_outW));
            return;
        }

        if (m_demo) {
            if (anyPress(pc)) {
                log(LogLevel::Info, "demo interrupted by the player");
                endDemo();
                runOps();
                return;
            }
            m_demoDebtMs += dtMs;
            const int due = std::max(0, m_demo->due());
            if (m_demo->finished()) {
                log(LogLevel::Info, "demo finished");
                endDemo();
                runOps();
                return;
            }
            if (m_demoDebtMs < due)
                return; // the original waits for real time to reach the recorded clock
            m_demoDebtMs = std::min(m_demoDebtMs - due, 250.0);
            const auto step = m_demo->next();
            tick(step.input, std::clamp(static_cast<int>(step.clock) - m_engine->clock(), 0, MaxFrameMs), dtSeconds);
        } else {
            m_msDebt += dtMs;
            const int elapsed = static_cast<int>(m_msDebt + 1e-6);
            m_msDebt -= elapsed;
            const auto vp = viewPointer(pc);
            const auto& cam = m_engine->camera();
            const OriginalInput rec = m_inputBuilder.build(pc, vp.first, vp.second, cam.x, cam.y, static_cast<float>(dtSeconds),
                                                           m_ui.hasMenu() ? InputRecordBuilder::View{} : InputRecordBuilder::View{m_viewW, m_outH});
            tick(rec, elapsed, dtSeconds);
        }
        runOps();
    }

    // ---- output ---------------------------------------------------------------------------------
    [[nodiscard]] const std::uint32_t* xrgb() const noexcept { return m_xrgb.data(); }
    [[nodiscard]] int width() const noexcept { return m_outW; }
    [[nodiscard]] int height() const noexcept { return m_outH; }
    [[nodiscard]] std::size_t pitch() const noexcept { return static_cast<std::size_t>(m_outW) * 4; }
    [[nodiscard]] bool exitRequested() const noexcept { return m_exit; }
    void clearExit() { m_exit = false; }

    // Context for the input mapper of the next frame.
    [[nodiscard]] input::Context inputContext() const {
        if (textEntry())
            return input::Context::TextEntry;
        if (m_movieActive || m_ui.hasMenu())
            return input::Context::Menu;
        return input::Context::Gameplay;
    }

    // Player position on the output (direct control scheme aims from there).
    [[nodiscard]] std::optional<input::Vec2> playerScreenPosition() {
        if (!m_engine || !m_mapLoaded)
            return std::nullopt;
        const std::array<script::Value, 1> army{script::Value{0}};
        const auto f = m_engine->callBuiltin(85, army);
        if (!f || f->toInt() == 0)
            return std::nullopt;
        const std::array<script::Value, 1> unit{*f};
        const auto x = m_engine->callBuiltin(109, unit), y = m_engine->callBuiltin(110, unit);
        if (!x || !y)
            return std::nullopt;
        const auto& cam = m_engine->camera();
        return input::Vec2{static_cast<float>(x->toInt() - cam.x + m_viewX), static_cast<float>(y->toInt() - cam.y)};
    }

    // Everything the input mapper needs about the game this frame (issue #16): player and
    // targets on the output, weapon range, selectable and selected weapon. See
    // session_controls.hpp.
    [[nodiscard]] ControlsFrame controlsFrame() {
        ControlsFrame c;
        if (!m_engine || !m_mapLoaded || m_demo)
            return c;
        const PlayerAim aim = m_engine->playerAim();
        const auto& cam = m_engine->camera();
        const auto toScreen = [&](int x, int y) {
            return input::Vec2{static_cast<float>(x - cam.x + m_viewX), static_cast<float>(y - cam.y)};
        };
        if (aim.valid) {
            c.player = toScreen(aim.flagmanX, aim.flagmanY);
            c.flagmanX = aim.flagmanX;
            c.flagmanY = aim.flagmanY;
            c.weaponRange = static_cast<float>(aim.weaponRange);
            c.weaponKind = weaponKind(aim.fire);
            for (const auto& t : aim.targets) {
                c.targets.push_back({toScreen(t.x, t.y), toScreen(t.groundX, t.groundY)});
                c.targetHp += t.hp;
            }
            c.targetCount = static_cast<int>(aim.targets.size());
        } else {
            c.player = playerScreenPosition(); // engines without the query
        }
        if (c.player && m_vm && m_vm->hasGlobal("weapon") && m_vm->hasGlobal("nweapon")) {
            const WeaponSlots w = currentWeapons();
            c.weaponMask = w.mask;
            c.currentWeapon = w.current;
        }
        return c;
    }
    // The view follows the player (direct scheme, touch; core option). Off during demos,
    // menus and ScrollLock(1).
    void setCameraFollow(bool on) {
        if (on != m_followOn) m_follow.reset();
        m_followOn = on;
    }

    // ---- inspection (tests, tools, logs) -------------------------------------------------------
    [[nodiscard]] const std::string& currentMap() const noexcept { return m_map; }
    [[nodiscard]] const std::vector<std::string>& mapHistory() const noexcept { return m_history; }
    [[nodiscard]] script::Interpreter* interpreter() noexcept { return m_vm.get(); }
    [[nodiscard]] Engine& engine() noexcept { return *m_engine; }
    [[nodiscard]] SessionUi& ui() noexcept { return m_ui; }
    [[nodiscard]] SaveFiles& files() noexcept { return m_files; }
    [[nodiscard]] bool demoPlaying() const noexcept { return static_cast<bool>(m_demo); }
    [[nodiscard]] const DemoPlayer* demo() const noexcept { return m_demo ? &*m_demo : nullptr; }
    [[nodiscard]] bool moviePlaying() const noexcept { return m_movieActive; }
    [[nodiscard]] const std::string& lastMovie() const noexcept { return m_lastMovie; }
    [[nodiscard]] const OriginalInput& lastInput() const noexcept { return m_input; }
    [[nodiscard]] std::pair<int, int> scriptScreen() const noexcept { return {m_screenW, m_screenH}; }
    [[nodiscard]] bool scrollLocked() const noexcept { return m_scrollLock; }
    [[nodiscard]] bool stateBarShown() const noexcept { return m_stateBar; }
    [[nodiscard]] std::uint64_t ticks() const noexcept { return m_ticks; }
    [[nodiscard]] std::size_t scriptErrors() const noexcept { return m_scriptErrors; }
    [[nodiscard]] std::pair<int, int> menuOrigin() const noexcept {
        return {(m_viewW - m_screenW) / 2, (m_outH - m_screenH) / 2};
    }
    [[nodiscard]] int viewX() const noexcept { return m_viewX; }
    [[nodiscard]] int viewWidth() const noexcept { return m_viewW; }
    // Value of an int global of the running script (0 if missing).
    [[nodiscard]] std::int32_t globalInt(std::string_view name, int index = 0) const {
        if (!m_vm || !m_vm->hasGlobal(name))
            return 0;
        try {
            return m_vm->get(name, index).toInt();
        } catch (const std::exception&) {
            return 0;
        }
    }

    // ---- ScriptHost -------------------------------------------------------------------------------
    script::Value callBuiltin(const script::BuiltinInfo& b, std::span<const script::Value> args) override {
        script::Value r = dispatch(b.id, args);
        if (builtinObserver)
            builtinObserver(b.id, args, r);
        return r;
    }
    // Diagnostics (tools/game_trace): every builtin call of the script, with its result.
    std::function<void(int, std::span<const script::Value>, const script::Value&)> builtinObserver;
    // Diagnostics (tools/game_trace): the effect channels and a replacement for the mixer sink
    // (nullptr restores the mixer).
    [[nodiscard]] SoundChannels& soundChannels() noexcept { return m_sfx; }
    void setSoundSink(SoundSink* sink) noexcept { m_sink = sink ? sink : &m_sfxSink; }
    void warning(const script::SourceLocation& loc, std::string_view message) override {
        if (m_warnings++ < 50)
            log(LogLevel::Warn, std::format("script {}: {}", loc.str(), message));
    }

    // Builtin dispatch by ID (public for tests and tools).
    script::Value dispatch(int id, std::span<const script::Value> args) {
        using script::argInt;
        using script::argString;
        using script::Value;
        // Session objects passed to world builtins.
        if ((id == 67 || id == 109 || id == 110 || id == 94) && isSessionHandle(argInt(args, 0)))
            return sessionObjectBuiltin(id, args);
        if (isEngineBuiltin(id)) {
            if (const auto v = m_engine->callBuiltin(id, args))
                return *v;
            return Value{0};
        }
        switch (static_cast<script::BuiltinId>(id)) {
        // --- system ---
        case script::BuiltinId::Load: m_ops.push_back({Op::Load, argString(args, 0)}); return Value{0};
        case script::BuiltinId::PlayAVI: m_ops.push_back({Op::Movie, argString(args, 0)}); return Value{0};
        case script::BuiltinId::SetGraphMode: m_ops.push_back({Op::GraphMode, {}, argInt(args, 0)}); return Value{0};
        case script::BuiltinId::Exit: m_ops.push_back({Op::Exit, {}}); return Value{0};
        case script::BuiltinId::Print: log(LogLevel::Info, "Print: " + argString(args, 0)); return Value{0};
        case script::BuiltinId::strlen: return Value{static_cast<std::int32_t>(cstrlen(argString(args, 0)))};
        case script::BuiltinId::SetGraphDetail: setGraphDetail(argInt(args, 0) != 0); return Value{0};
        case script::BuiltinId::SaveDemo: m_recordPath = argString(args, 0); return Value{0};
        case script::BuiltinId::E_InsertTerrain: return Value{0}; // map editor only
        case script::BuiltinId::GetReg: return Value{std::string{}}; // no registry
        // --- input ---
        case script::BuiltinId::GetKey: return Value{std::int32_t{m_input.key}};
        case script::BuiltinId::GetInputState: return Value{std::int32_t{m_input.bits}};
        case script::BuiltinId::SetCursor: m_cursorType = argInt(args, 0); return Value{0};
        case script::BuiltinId::ScrollLock:
            m_scrollLock = argInt(args, 0) != 0;
            m_engine->setScrollLock(m_scrollLock);
            return Value{0};
        // --- menus ---
        case script::BuiltinId::LoadMenu: loadMenu(argString(args, 0), UiList::Menu); return Value{0};
        case script::BuiltinId::FreeMenu: m_ui.freeList(UiList::Menu); return Value{0};
        case script::BuiltinId::MenuCreate: return Value{menuCreate(argInt(args, 0), argInt(args, 1), argInt(args, 2), argInt(args, 3))};
        case script::BuiltinId::MenuAction: m_ui.menuAction(argInt(args, 0), argInt(args, 1), argInt(args, 2)); return Value{0};
        case script::BuiltinId::MenuVid: return Value{m_ui.menuState().menuVid()};
        case script::BuiltinId::MenuDir: return Value{m_ui.menuState().dir};
        case script::BuiltinId::MenuSelectVid: return Value{m_ui.menuState().selected};
        // --- texts ---
        case script::BuiltinId::PrintText:
            return Value{m_ui.print(argInt(args, 0), argInt(args, 1), argString(args, 2), argInt(args, 3))};
        case script::BuiltinId::PrintFile: return Value{printFile(argInt(args, 0), argInt(args, 1), argString(args, 2), argInt(args, 3))};
        case script::BuiltinId::DeleteText: m_ui.deleteText(argInt(args, 0)); return Value{0};
        case script::BuiltinId::Message:
            if (m_stateBar) { // 0x416cf0
                m_ui.setMessage(argString(args, 0));
                m_messageClock = static_cast<std::uint32_t>(m_engine->clock());
            }
            return Value{0};
        case script::BuiltinId::StateBar: setStateBar(argInt(args, 0) != 0); return Value{0};
        // --- audio ---
        case script::BuiltinId::PlaySFX: queueSfx(argInt(args, 0)); return Value{0};
        case script::BuiltinId::StopSFX:
            m_sfx.stopAll(*m_sink);
            return Value{0};
        case script::BuiltinId::StopSFXOne: m_sfx.stop(*m_sink, argInt(args, 0)); return Value{0};
        case script::BuiltinId::PlayAudio: playAudio({argInt(args, 0), argInt(args, 1), argInt(args, 2), argInt(args, 3)}); return Value{0};
        case script::BuiltinId::StopMusic: // also stops the PlayWAV slot (0x412e00 -> 0x408d80)
            stopMusic();
            m_sfx.stop(*m_sink, playWavSlot());
            return Value{0};
        case script::BuiltinId::SetSound: m_scriptSfx = std::clamp(argInt(args, 0), 0, 100); applyVolumes(); return Value{0};
        case script::BuiltinId::SetMusic: setMusic(std::clamp(argInt(args, 0), 0, 100)); return Value{0};
        case script::BuiltinId::PlayWAV: return Value{playWav(argString(args, 0))};
        case script::BuiltinId::PlayMidi:
        case script::BuiltinId::mciSendString:
            log(LogLevel::Debug, std::format("builtin {} ignored ({})", id, argString(args, 0)));
            return Value{0};
        // --- files ---
        case script::BuiltinId::FOpen: return Value{m_demo ? 0 : m_files.open(argString(args, 0))};
        case script::BuiltinId::FCreate: return Value{m_demo ? 0 : m_files.create(argString(args, 0))};
        case script::BuiltinId::FRead: return Value{fread(argInt(args, 0))};
        case script::BuiltinId::FWrite: m_files.writeLine(argInt(args, 0), argString(args, 1)); return Value{0};
        case script::BuiltinId::FClose: m_files.close(argInt(args, 0)); return Value{0};
        case script::BuiltinId::FileCopy: m_files.copy(argString(args, 0), argString(args, 1)); return Value{0};
        // --- network (single player) ---
        case script::BuiltinId::GetNetInfo: return netInfo(argInt(args, 0));
        case script::BuiltinId::Send: return Value{0};
        case script::BuiltinId::Receive: return Value{0};
        case script::BuiltinId::SetNet: m_net = argInt(args, 0); return Value{0};
        case script::BuiltinId::GetNetKey: return Value{std::int32_t{m_input.key}};
        case script::BuiltinId::GetNetInputState: return Value{std::int32_t{m_input.bits}};
        default:
            if (m_unknown++ < 20)
                log(LogLevel::Warn, std::format("unknown builtin {}", id));
            return Value{0};
        }
    }

    // Session builtins implemented (all IDs 64..139 not in EngineBuiltinIds).
    static constexpr int SessionBuiltinCount = 76 - static_cast<int>(EngineBuiltinIds.size());

private:
    struct Op {
        enum Kind : std::uint8_t { Load, Movie, GraphMode, Exit } kind;
        std::string arg;
        int value = 0;
    };

    void log(LogLevel level, std::string_view text) const {
        if (m_config.log)
            m_config.log(level, text);
    }

    static std::size_t cstrlen(const std::string& s) {
        const auto nul = s.find('\0');
        return nul == std::string::npos ? s.size() : nul;
    }

    static bool anyPress(const input::PcState& pc) {
        if (pc.left.pressed || pc.right.pressed || !pc.text.empty())
            return true;
        return std::ranges::any_of(pc.keys, [](const input::ButtonState& b) { return b.pressed; });
    }

    // The input mapper gives buttons a different meaning in each context (Start = Enter while
    // typing a name, Esc in menus; A types a letter or clicks): a button still held when the
    // context changes would arrive as a fresh key or click. When the context changes while
    // something was held in the previous frame, what is down now is ignored until released.
    const input::PcState& filterStale(const input::PcState& pc) {
        const input::Context ctx = inputContext();
        if (ctx != m_lastContext) {
            m_lastContext = ctx;
            if (m_prevAnyDown) {
                for (std::size_t i = 0; i < input::KeyCount; ++i)
                    m_staleKey[i] = pc.keys[i].down;
                m_staleLeft = pc.left.down;
                m_staleRight = pc.right.down;
            }
        }
        m_prevAnyDown = pc.left.down || pc.right.down ||
                        std::ranges::any_of(pc.keys, [](const input::ButtonState& b) { return b.down; });
        m_filtered = pc;
        auto filter = [](input::ButtonState& b, bool& stale) {
            if (stale && !b.down) stale = false;
            if (stale) b = {};
        };
        for (std::size_t i = 0; i < input::KeyCount; ++i)
            filter(m_filtered.keys[i], m_staleKey[i]);
        filter(m_filtered.left, m_staleLeft);
        filter(m_filtered.right, m_staleRight);
        return m_filtered;
    }

    // Cursor in view coordinates.
    [[nodiscard]] std::pair<int, int> viewPointer(const input::PcState& pc) const {
        return {pc.cursorX() - m_viewX, pc.cursorY()};
    }

    // ---- the frame ---------------------------------------------------------------------------
    void tick(OriginalInput rec, int elapsedMs, double dtSeconds) {
        if (!m_mapLoaded)
            return;
        ++m_ticks;
        auto& cam = m_engine->camera();
        cam.width = m_viewW;
        cam.height = m_outH;

        m_engine->beginFrame(elapsedMs);
        m_engine->setInput(rec);
        m_input = rec;
        m_ui.setClock(static_cast<std::uint32_t>(m_engine->clock()));
        if (m_recorder)
            m_recorder->tick(static_cast<std::uint32_t>(m_engine->clock()), rec);

        runScript();

        FramebufferRef fb{m_view.data(), m_outH, m_viewW};
        if (m_config.render) {
            m_engine->draw(fb);
            drawUi(fb);
        }
        // Positional sounds are computed while the sprites tick (0x409a20, inside World::Tact):
        // with the camera of that moment, before the scroll and the clamp of the frame's end.
        m_soundCam = m_engine->camera();
        m_engine->tactWorld();
        // Menus and texts are sprites of the world in the original: they tick in World::Tact,
        // before the menu update (0x40c1d0) of the frame.
        for (const int s : m_ui.update(static_cast<float>(dtSeconds), static_cast<std::uint32_t>(m_engine->clock())))
            queueSfx(s);
        m_screenSounds = m_ui.takeScreenSounds();

        // Menu update: screen coordinates of the script's screen.
        const auto [ox, oy] = menuOrigin();
        const int mx = m_demo ? rec.mouseScreenX : rec.mouseScreenX - ox;
        const int my = m_demo ? rec.mouseScreenY : rec.mouseScreenY - oy;
        std::uint16_t bits = rec.bits;
        if (m_ui.updateMenu(mx, my, bits)) {
            rec.bits = bits;
            m_engine->setInput(rec);
        }
        m_engine->tactPlayers();
        stateBarTact(m_config.render ? &fb : nullptr);
        m_engine->tactGroupAi();
        followCamera(rec, dtSeconds);
        m_engine->updateCamera();
        playSounds();

        if (m_config.render) {
            m_engine->drawScreenEffects(fb);
            drawTextEntryCandidate(fb);
            drawWeaponHud(fb, dtSeconds);
            present();
        }
    }

    void runScript() {
        if (!m_vm)
            return;
        try {
            m_vm->tick();
        } catch (const std::exception& e) {
            if (m_scriptErrors++ < 20)
                log(LogLevel::Error, std::format("script {}: {}", m_map, e.what()));
        }
    }

    // Posted operations, executed after the frame in the order they were posted.
    void runOps() {
        while (!m_ops.empty() && !m_movieActive) {
            Op op = std::move(m_ops.front());
            m_ops.pop_front();
            switch (op.kind) {
            case Op::Load:
                if (m_demo && !op.arg.ends_with(".dem")) {
                    log(LogLevel::Info, "demo: the script loads a map, back to the trailer map");
                    endDemo();
                } else {
                    loadMap(op.arg);
                }
                break;
            case Op::Movie: startMovie(op.arg); break;
            case Op::GraphMode:
                m_screenW = op.value == 2 ? 800 : op.value == 3 ? 1024 : 640;
                m_screenH = op.value == 2 ? 600 : op.value == 3 ? 768 : 480;
                break;
            case Op::Exit: m_exit = true; m_ops.clear(); return;
            }
        }
    }

    // ---- maps and scripts ---------------------------------------------------------------------
    static bool iequalsSuffix(std::string_view s, std::string_view suffix) {
        if (s.size() < suffix.size())
            return false;
        return std::ranges::equal(s.substr(s.size() - suffix.size()), suffix, [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
        });
    }

    void loadMap(std::string name) {
        if (iequalsSuffix(name, ".dem")) {
            startDemo(name);
            return;
        }
        loadMapAndScript(name, nextSeed(), nullptr);
    }

    std::uint32_t nextSeed() const {
        // timeGetTime() in the original: milliseconds since the session started.
        return static_cast<std::uint32_t>(m_realMs) + 0x10000u;
    }

    bool loadMapAndScript(const std::string& name, std::uint32_t seed, const demo::DemoFile* demo) {
        const auto mapPath = script::resolveCaseInsensitive(m_config.gameDir, name);
        if (!mapPath) {
            log(LogLevel::Error, std::format("Load({}): map not found", name));
            return false;
        }
        finishRecording();
        // The previous script's cells survive the load (docs/re/runtime.md §1.3), also across
        // maps without a script.
        if (m_vm)
            m_carry = m_vm->snapshot();
        m_ui.clear();
        m_stateBar = false;
        m_sfx.clearQueue();
        m_engine->unloadMap();
        m_mapLoaded = false;
        m_vm.reset();
        try {
            m_engine->loadMap(*mapPath, seed);
            if (demo) // demos start from the recorded clock, not the map's startTimer (0x414e8b)
                m_engine->setClock(demo->startClock);
            m_ui.resetClock(static_cast<std::uint32_t>(m_engine->clock()));
        } catch (const std::exception& e) {
            log(LogLevel::Error, std::format("Load({}): {}", name, e.what()));
            return false;
        }
        m_mapLoaded = true;
        m_scrollLock = false;
        m_engine->setScrollLock(false);
        auto& cam = m_engine->camera();
        cam.width = m_viewW;
        cam.height = m_outH;
        m_map = name;
        m_history.push_back(name);
        m_inputBuilder.clear();
        m_input = {};

        std::filesystem::path scriptName{std::string{name}};
        std::string ini = name.substr(0, name.size() - std::min(name.size(), scriptName.extension().string().size())) + ".ini";
        compileScript(ini);
        if (m_vm) {
            if (m_carry)
                m_vm->inheritGlobals(*m_carry);
            if (demo && !m_vm->setIntGlobalValues(demo->intGlobals)) {
                // Recorded with a script of a different size: apply the cells both have.
                auto ints = m_vm->intGlobalValues();
                const std::size_t n = std::min(ints.size(), demo->intGlobals.size());
                std::copy_n(demo->intGlobals.begin(), n, ints.begin());
                m_vm->setIntGlobalValues(ints);
                log(LogLevel::Warn, std::format("demo: {} int cells recorded, the script has {}",
                                                demo->intGlobals.size(), ints.size()));
            }
        }
        log(LogLevel::Info, std::format("load {} (seed {:#x}, clock {}){}", name, seed, m_engine->clock(), demo ? " [demo]" : ""));

        if (!demo && !m_recordPath.empty())
            startRecording(name, seed);
        return true;
    }

    void compileScript(const std::string& ini) {
        if (!script::resolveCaseInsensitive(m_config.gameDir, ini)) {
            log(LogLevel::Warn, std::format("{}: no script, the map runs without one", ini));
            return;
        }
        try {
            // The game scripts declare their builtins themselves (#include "maps\header.ini"):
            // predeclaring them would count twice against the original's 128-function limit.
            script::CompileOptions opts;
            if (m_config.predeclareBuiltins)
                opts.predeclared = &m_builtins;
            auto compiled = script::Script::compile(m_loader, ini, opts);
            // Default ExecConfig = the original's semantics (docs/script-language.md).
            m_vm = std::make_unique<script::Interpreter>(std::move(compiled), *this, script::ExecConfig{});
        } catch (const std::exception& e) {
            log(LogLevel::Error, std::format("{}: {}", ini, e.what()));
            m_vm.reset();
        }
    }

    // Every int cell of the current script in the original order (what a .dem records).
    [[nodiscard]] std::vector<std::int32_t> intCells() const {
        std::vector<std::int32_t> out;
        if (m_vm)
            for (std::uint32_t v : m_vm->intGlobalValues())
                out.push_back(static_cast<std::int32_t>(v));
        return out;
    }

    // ---- demos ----------------------------------------------------------------------------------
    void startDemo(const std::string& name) {
        std::optional<std::filesystem::path> path = script::resolveCaseInsensitive(m_config.gameDir, name);
        if (!path && !m_config.saveDir.empty())
            path = script::resolveCaseInsensitive(m_config.saveDir, name);
        if (!path) {
            log(LogLevel::Error, std::format("demo {} not found", name));
            return;
        }
        std::ifstream in{*path, std::ios::binary};
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{in}, {}};
        auto parsed = demo::parseDemo(bytes);
        if (!parsed || parsed->ticks.empty()) {
            log(LogLevel::Error, std::format("demo {}: unreadable", name));
            return;
        }
        endDemo(false);
        DemoPlayer player{std::move(*parsed)};
        if (!loadMapAndScript(player.file().mapName, player.file().seed, &player.file()))
            return;
        if (static_cast<std::uint32_t>(m_engine->clock()) != player.file().startClock)
            log(LogLevel::Warn, std::format("demo: map clock {} differs from the recorded {}", m_engine->clock(),
                                            player.file().startClock));
        m_demo.emplace(std::move(player));
        m_demoName = name; // save states reload the demo by name
        m_demoDebtMs = 0;
        m_demoView = true;
        updateView();
        log(LogLevel::Info, std::format("demo {}: {} ticks of {}", name, m_demo->file().ticks.size(), m_demo->file().mapName));
    }

    // Ends a running demo; with `loadNext`, loads the map of the demo's trailer.
    void endDemo(bool loadNext = true) {
        if (!m_demo)
            return;
        const std::string next = m_demo->nextMap();
        m_demo.reset();
        m_demoView = false;
        updateView();
        m_ops.clear();
        if (loadNext)
            loadMapAndScript(next, nextSeed(), nullptr);
    }

    void updateView() {
        m_viewW = m_demoView ? std::min(m_outW, 640) : m_outW;
        m_viewX = (m_outW - m_viewW) / 2;
    }

    std::string fread(int handle) {
        if (m_demo) {
            if (auto line = m_demo->fread()) {
                m_lastDemoLine = *line;
                return *line;
            }
            return m_lastDemoLine;
        }
        std::string line = m_files.readLine(handle);
        if (m_recorder)
            m_recorder->fread(line);
        return line;
    }

    void startRecording(const std::string& name, std::uint32_t seed) {
        if (!m_vm)
            return;
        m_recorder.emplace(name, seed, static_cast<std::uint32_t>(m_engine->clock()), intCells());
        m_recordingTo = std::exchange(m_recordPath, {});
        log(LogLevel::Info, std::format("recording demo {} of {}", m_recordingTo, name));
    }

    void finishRecording() {
        if (!m_recorder)
            return;
        const auto bytes = m_recorder->finish();
        m_recorder.reset();
        const auto path = m_files.writePath(m_recordingTo);
        if (path.empty())
            return;
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out{path, std::ios::binary | std::ios::trunc};
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        log(LogLevel::Info, std::format("demo written to {} ({} bytes)", path.string(), bytes.size()));
    }

    // ---- builtins ---------------------------------------------------------------------------------
    script::Value sessionObjectBuiltin(int id, std::span<const script::Value> args) {
        using script::argInt;
        const int h = argInt(args, 0);
        if (id == 67)
            return script::Value{m_ui.objectAction(h, argInt(args, 1), argInt(args, 2), argInt(args, 3))};
        const auto p = m_ui.position(h);
        if (!p)
            return script::Value{0};
        return script::Value{id == 109 ? (*p)[0] : id == 110 ? (*p)[1] : (*p)[2]};
    }

    int menuCreate(int nvid, int dir, int x, int y) {
        if (nvid < 0 || nvid >= static_cast<int>(m_vids.size()))
            return 0;
        if (m_vids[nvid].type != ObjectClass::Image) {
            // Not a button: the original creates an ordinary unit (CreateUnit) at (x, y).
            const std::array<script::Value, 4> a{script::Value{nvid}, script::Value{x}, script::Value{y}, script::Value{0}};
            const auto v = m_engine->callBuiltin(68, a);
            return v ? v->toInt() : 0;
        }
        return m_ui.create(nvid, dir, x, y);
    }

    void loadMenu(const std::string& file, UiList list) {
        const auto path = script::resolveCaseInsensitive(m_config.gameDir, file);
        if (!path) {
            log(LogLevel::Warn, std::format("LoadMenu({}): not found", file));
            return;
        }
        try {
            m_ui.loadMenu(ui::loadMenuFile(*path, m_vids), list);
        } catch (const std::exception& e) {
            log(LogLevel::Warn, std::format("LoadMenu({}): {}", file, e.what()));
        }
    }

    int printFile(int x, int y, const std::string& file, int font) {
        auto path = script::resolveCaseInsensitive(m_config.gameDir, file);
        if (!path && !m_config.saveDir.empty())
            path = script::resolveCaseInsensitive(m_config.saveDir, file);
        if (!path)
            return 0;
        return m_ui.printFile(x, y, *path, font);
    }

    // StateBar(enable): statebar.men in a list of its own (stateba8.men on 800-wide screens:
    // the view is never 800 wide here) plus the Message text.
    void setStateBar(bool enable) {
        if (enable == m_stateBar)
            return;
        m_ui.freeList(UiList::StateBar);
        m_ui.setMessage({});
        m_stateBar = enable;
        if (enable)
            loadMenu("maps\\statebar.men", UiList::StateBar);
    }

    // StateBar::Tact 0x416de0, where the original runs it: inside Player::Tact of the local
    // player (so not while paused), after the world and the menus have been drawn. For each
    // object of the bar, in list order (statebar.men: 266, 61, 49):
    //  - 266 (bar, 5 directions), only with a weapon head: the head's vid name and the five
    //    ammo counters (right-aligned at +391 + 48*i) in font 127 at the Message unit's
    //    position (266.x - 299, 266.y + 225) - 15, drawn straight into the picture (putcadr);
    //    direction = weapon index from the head's nvid (0x41700f);
    //  - 61 (radar): 2x2 dots for the objects within +-464 px (offset / 16 from the radar's
    //    centre, colours of 0x416e16), direction = towards the nearest enemy (or 0);
    //  - 49 (hp bar): direction = hp * 127 / maxHP (half of its 79 directions).
    // SetDirection (0x40b610) restarts the animation, so these objects stay on their first
    // frame. Message() text goes back to empty 5 s after it was set (0x416e96).
    void stateBarTact(FramebufferRef* fb) {
        if (!m_stateBar)
            return;
        const auto info = m_engine->stateBarInfo();
        if (!info)
            return;
        const auto clock = static_cast<std::uint32_t>(m_engine->clock());
        if (clock - m_messageClock > 5000) {
            m_ui.setMessage({});
            m_messageClock = clock;
        }
        const int ox = (m_viewW - 640) / 2, oy = m_outH - 480;   // origin of the bar's screen
        const auto setDir = [&](const ScreenObject& o, int dir) {   // direction byte -> index
            const int dirs = m_vids[o.nvid].directionsCount;
            if (dirs <= 0)
                return;
            const int step = static_cast<std::uint8_t>(256 / dirs);
            m_ui.setDirection(o.handle, ((((step >> 1) + dir) & 0xff) * dirs) >> 8);
        };
        const auto glyph = [&](int c, int x, int y) {   // putcadr(127, c, x, y, 0)
            const Vid& font = m_vids[ui::vids::Font];
            const auto* g = std::get_if<Vid::Graphics>(&font.graphicsData);
            if (fb && g && *g && static_cast<std::size_t>(c) < (*g)->frames.size())
                hicolor::drawFrame((*g)->frames[static_cast<std::size_t>(c)], x, y, *fb);
            return g && *g ? static_cast<int>((*g)->width) : 7;
        };
        const auto objects = m_ui.objects();   // setDirection edits objects in place
        for (const auto& o : objects) {
            if (o.list != UiList::StateBar)
                continue;
            if (o.nvid == 266 && info->turretNvid >= 0) {
                const int tx = ox + o.x - 299, ty = oy + o.y + 225 - 15;
                int x = tx;
                for (const char ch : info->turretName)
                    x += glyph(static_cast<std::uint8_t>(ch), x, ty);
                for (std::size_t i = 0; i < info->ammo.size(); ++i) {
                    const std::string digits = std::to_string(info->ammo[i]);   // itoa(v, 10)
                    int dx = tx + 391 + 48 * static_cast<int>(i);
                    for (auto it = digits.rbegin(); it != digits.rend(); ++it)
                        dx -= glyph(static_cast<std::uint8_t>(*it), dx, ty);
                }
                const int n = info->turretNvid;
                const int index = n < 0x5c ? n - 0x58 : n < 0xcf ? n - 0xca : n == 0xcf ? 4 : n - 0xcf;
                const int dirs = std::max<int>(1, m_vids[o.nvid].directionsCount);
                setDir(o, (index * 256 + static_cast<std::uint8_t>(256 / dirs)) / dirs);
            } else if (o.nvid == 61) {
                if (fb) {
                    const int left = ox, top = oy, right = ox + 639, bottom = oy + 479;   // clip rect
                    for (const auto& d : info->radar) {
                        const int x = ox + o.x + (d.dx >> 4), y = oy + o.y + (d.dy >> 4);
                        if (x < left || y < top || x >= right || y >= bottom)
                            continue;
                        const RGBA8 c = hicolor::toPixel(d.color);
                        for (int yy = y; yy <= y + 1; ++yy)
                            for (int xx = x; xx <= x + 1; ++xx)
                                if (xx >= 0 && yy >= 0 && xx < fb->extent(1) && yy < fb->extent(0))
                                    (*fb)[yy, xx] = c;
                    }
                }
                setDir(o, info->enemyDirection >= 0 ? info->enemyDirection : 0);
            } else if (o.nvid == 49) {
                if (info->maxHp)
                    setDir(o, info->hp * 127 / info->maxHp);
                else if (o.dirIndex != 0)
                    m_ui.setDirection(o.handle, 0);
            }
        }
    }

    void setGraphDetail(bool high) {
        // 0x412e8c: extra graphic children on high detail; vid 19's children in both cases.
        auto setVid = [&](int nvid, int type, int value) {
            const std::array<script::Value, 3> a{script::Value{nvid}, script::Value{type}, script::Value{value}};
            m_engine->callBuiltin(127, a);
        };
        constexpr int VidChild = 2;
        for (const int nvid : {5, 63, 162, 193})
            setVid(nvid, VidChild + 14, high ? 199 : 0);
        setVid(74, VidChild + 15, high ? 200 : 0);
        for (const int k : {2, 4, 5})
            setVid(19, VidChild + k, 116);
        m_detail = high;
    }

    script::Value netInfo(int type) {
        switch (type) {
        case 1: return script::Value{0};                     // local player number
        case 2: case 3: case 4: case 5: return script::Value{std::string{}}; // player names
        case 6: return script::Value{0};                     // number of players: single player
        default: return script::Value{0};                    // 9 clears kills, 10..25 kills
        }
    }

    // ---- audio ------------------------------------------------------------------------------------
    // Effect numbers index fw.res sounds from 0, as the original's descriptor table
    // (0x408730 loads sound i into descriptor i): nsfx 1..N-1 are playable (nsfx 0 is rejected
    // by 0x408dd0) and nsfx N = number of fw.res sounds is the single PlayWAV slot
    // (game/sound_channels.hpp effectSound, checked against the original by sound_oracle_test).
    [[nodiscard]] int playWavSlot() const { return static_cast<int>(m_sounds.size()); }
    [[nodiscard]] const SoundData* sound(int nsfx) const {
        // older WAVs stay alive in m_wavs: voices may still point at them
        return effectSound(m_sounds, m_wavLoaded && !m_wavs.empty() ? &m_wavs.back() : nullptr, nsfx);
    }

    // PlaySFX(n) and UI sounds: full volume, centred (0x4130ca -> 0x408dd0).
    void queueSfx(int nsfx) { m_sfx.request(nsfx); }

    // Plays the effects requested this frame with the original's channel rules (looping
    // engines kept alive instead of restarted, 8 channels, 50 ms restart guard...): see
    // game/sound_channels.hpp.
    void playSounds() {
        const Camera& cam = m_soundCam;
        for (const auto& e : m_engine->takeSounds()) {
            // DirectSound pan/attenuation of 0x409a20: pan 4*dx, attenuation -2*max(|dx|,|dy|),
            // both in 1/100 dB, relative to the centre of the view.
            const int dx = e.x - (cam.x + cam.width / 2), dy = e.y - (cam.y + cam.height / 2);
            m_sfx.request(e.nsfx, 4 * dx, -2 * std::max(std::abs(dx), std::abs(dy)));
        }
        // Menus and texts are screen sprites (layers 10..14, after the world's units): their
        // sounds are relative to the centre of the script's screen (0x409ab4).
        for (const auto& e : std::exchange(m_screenSounds, {})) {
            const int dx = e.x - m_screenW / 2, dy = e.y - m_screenH / 2;
            m_sfx.request(e.nsfx, 4 * dx, -2 * std::max(std::abs(dx), std::abs(dy)));
        }
        m_sfx.tact(*m_sink, static_cast<std::uint32_t>(m_engine->clock()));
    }

    // gromada::Mixer as the SoundChannels sink: one DirectSound buffer per channel (no mixer:
    // nothing plays, nothing is "playing"). Stop pauses the voice, Play resumes it (DirectSound
    // keeps the position); a one-shot that reached its end plays again from the start.
    struct MixerSink final : SoundSink {
        struct Buffer {
            const SoundData* sound = nullptr;
            int nsfx = 0;
            std::uint32_t voice = 0;
            int pan = 0, attenuation = 0; // DirectSound units
        };
        Mixer* mixer = nullptr;
        Session* session = nullptr;
        std::array<Buffer, SoundChannels::ChannelCount> buffers{};

        bool load(int ch, int nsfx, const SoundData& s, int source) override {
            drop(ch);
            buffers[ch] = {&s, nsfx, 0, 0, 0};
            if (source >= 0) { // a duplicate starts with the parameters of its source
                buffers[ch].pan = buffers[source].pan;
                buffers[ch].attenuation = buffers[source].attenuation;
            }
            return true;
        }
        void release(int ch) override { drop(ch); buffers[ch] = {}; }
        bool playing(int ch) override { return mixer && mixer->sfxPlaying(buffers[ch].voice); }
        void setPan(int ch, int pan) override { buffers[ch].pan = pan; apply(ch); }
        void setVolume(int ch, int attenuation) override {
            if (attenuation > 0 || attenuation < -10000)
                return; // DSERR_INVALIDPARAM: the volume does not change
            buffers[ch].attenuation = attenuation;
            apply(ch);
        }
        void play(int ch, bool loop) override {
            auto& b = buffers[ch];
            if (!mixer || !b.sound || mixer->resumeSfxVoice(b.voice, loop))
                return;
            if (loop && session) // rare: a looping sound (vehicle engine) starts
                session->log(LogLevel::Debug, std::format("sfx {} loop start", b.nsfx));
            b.voice = mixer->playSfx(*b.sound, SoundChannels::volumeOf(b.attenuation), SoundChannels::panOf(b.pan), loop);
        }
        void stop(int ch) override { if (mixer) mixer->pauseSfxVoice(buffers[ch].voice); }
        void drop(int ch) {
            if (mixer && buffers[ch].voice)
                mixer->stopSfxVoice(buffers[ch].voice);
        }
        void apply(int ch) {
            const auto& b = buffers[ch];
            if (mixer && b.voice)
                mixer->setSfxVolumePan(b.voice, SoundChannels::volumeOf(b.attenuation), SoundChannels::panOf(b.pan));
        }
    };

    // PlayAudio(t1..t4) / PlayMusic: CD playlist (docs/re/runtime.md §9).
    void playAudio(std::array<int, 4> t) {
        if (t[0] < 0)
            return;
        for (int i = 1; i < 4; ++i)
            if (t[i] < 0) t[i] = t[0];
        if (m_playing && m_playlist == t)
            return;
        m_playlist = t;
        m_playing = true;
        if (m_mixer && m_scriptMusic > 0)
            m_mixer->playMusic(t);
    }

    void stopMusic() {
        m_playing = false;
        m_playlist = {};
        if (m_mixer)
            m_mixer->stopMusic();
    }

    void setMusic(int volume) {
        const int before = m_scriptMusic;
        m_scriptMusic = volume;
        applyVolumes();
        if (!m_mixer)
            return;
        if (volume == 0)
            m_mixer->stopMusic();
        else if (before == 0 && m_playing) // from 0 to >0: the playlist restarts
            m_mixer->playMusic(m_playlist);
    }

    void applyVolumes() {
        if (!m_mixer)
            return;
        m_mixer->setSfxVolume(m_coreSfx * m_scriptSfx / 100.0f);
        m_mixer->setMusicVolume(m_movieActive ? 0.0f : m_coreMusic * m_scriptMusic / 100.0f);
    }

    // PlayWAV (0x408c50): one slot (N), looping, queued at once; the same file already playing
    // is left alone (game/sound_channels.hpp). Returns N, 0 if the file cannot be opened.
    int playWav(const std::string& file) {
        using WavLoad = SoundChannels::WavLoad;
        return m_sfx.playWav(*m_sink, file, [&] {
            m_wavLoaded = false; // the original frees the previous WAV first
            auto path = script::resolveCaseInsensitive(m_config.gameDir, file);
            if (!path)
                return WavLoad::NotFound;
            try {
                std::ifstream in{*path, std::ios::binary};
                const auto size = std::filesystem::file_size(*path);
                BinaryStreamReader reader{in, static_cast<std::size_t>(size)};
                m_wavs.emplace_back(reader);
                m_wavLoaded = true;
                m_wavNames.push_back(file); // save states reload them by name
                return WavLoad::Loaded;
            } catch (const std::exception& e) {
                log(LogLevel::Warn, std::format("PlayWAV({}): {}", file, e.what()));
                return WavLoad::Invalid; // opened but not playable: slot N stays empty
            }
        });
    }

    // ---- movies -----------------------------------------------------------------------------------
    void startMovie(const std::string& file) {
        m_lastMovie = file;
        if (!m_movies)
            return;
        const auto path = script::resolveCaseInsensitive(m_config.gameDir, file);
        if (!path || !m_movies->open(*path)) {
            log(LogLevel::Warn, std::format("PlayAVI({}): cannot play, skipped", file));
            return;
        }
        m_movieActive = true;
        applyVolumes(); // the CD pauses during movies
        if (m_mixer)
            m_mixer->setStream(m_movies->audio());
        log(LogLevel::Info, std::format("movie {}", file));
    }

    void stopMovie() {
        if (!m_movieActive)
            return;
        if (m_mixer)
            m_mixer->setStream(nullptr);
        m_movies->close();
        m_movieActive = false;
        applyVolumes();
    }

    // ---- drawing ------------------------------------------------------------------------------------
    void drawUi(FramebufferRef fb) {
        const auto [ox, oy] = menuOrigin();
        m_ui.draw(fb, UiList::Menu, ox, oy);
        if (m_stateBar)
            m_ui.draw(fb, UiList::StateBar, (m_viewW - 640) / 2, m_outH - 480);
    }

    // Name entry with the pad: the letter under selection blinks at the font cursor (vid 130).
    void drawTextEntryCandidate(FramebufferRef fb) {
        if (!textEntry() || (m_frames / 15) % 2)
            return;
        const auto [ox, oy] = menuOrigin();
        for (const auto& o : m_ui.objects())
            if (o.nvid == ui::vids::FontCursor) {
                const char c[2] = {m_pc.textCandidate, 0};
                m_ui.fonts().drawText(c, ui::vids::Font, ox + o.x - 3, oy + o.y - 5, fb);
                return;
            }
    }

    // ---- controls (issue #16, session_controls.hpp) -----------------------------------------------
    WeaponSlots currentWeapons() {
        const std::array<script::Value, 1> army{script::Value{0}};
        const auto f = m_engine->callBuiltin(85, army);
        const int flagman = f ? f->toInt() : 0;
        return weaponSlots([&](std::string_view name, int i) { return globalInt(name, i); }, [&](int index) {
            const std::array<script::Value, 4> a{script::Value{flagman}, script::Value{64}, script::Value{index}, script::Value{0}};
            const auto v = flagman ? m_engine->callBuiltin(67, a) : std::nullopt; // Action(act_getammo)
            return v ? v->toInt() : 0;
        });
    }

    void followCamera(const OriginalInput& rec, double dt) {
        if (!m_followOn || m_demo || m_scrollLock || m_ui.hasMenu())
            return;
        const std::array<script::Value, 1> army{script::Value{0}};
        const auto f = m_engine->callBuiltin(85, army);
        if (!f || f->toInt() == 0)
            return;
        const std::array<script::Value, 1> unit{*f};
        const auto x = m_engine->callBuiltin(109, unit), y = m_engine->callBuiltin(110, unit);
        if (!x || !y)
            return;
        constexpr int band = 5; // the engine's edge scroll (0x415e8d)
        const bool edge = rec.mouseScreenX <= band || rec.mouseScreenY <= band || rec.mouseScreenX >= m_viewW - 1 - band ||
                          rec.mouseScreenY >= m_outH - 1 - band;
        m_follow.update(m_engine->camera(), x->toInt(), y->toInt(), static_cast<float>(dt), edge);
    }

    // The selected weapon for a moment after a change, when the status bar is hidden.
    void drawWeaponHud(FramebufferRef fb, double dt) {
        if (m_demo || !m_vm || !m_vm->hasGlobal("nweapon") || !m_vm->hasGlobal("weapon")) {
            m_weaponHud.reset();
            return;
        }
        const std::array<script::Value, 1> army{script::Value{0}};
        const auto f = m_engine->callBuiltin(85, army);
        if (!f || f->toInt() == 0) {
            m_weaponHud.reset();
            return;
        }
        const WeaponSlots w = currentWeapons();
        const int requested = m_pc.weaponPending ? m_pc.weaponSlot : w.current;
        bool poke = false;
        for (int k = 0; k < 4; ++k)
            poke |= m_pc.key(static_cast<input::Key>(static_cast<int>(input::Key::Num1) + k)).pressed;
        if (!m_weaponHud.update(w.current, requested, static_cast<float>(dt), poke) || m_stateBar || m_ui.hasMenu())
            return;
        const int head = w.nvid[static_cast<std::size_t>(std::clamp(requested, 0, 3))];
        if (!head)
            return;
        const std::array<script::Value, 1> nvid{script::Value{head}};
        const auto name = m_engine->callBuiltin(124, nvid); // GetVidName
        // ammunition index of the head as the engine counts it (act_coor_attack)
        const int index = head < 0x5d ? head - 0x58 : head < 0xcf ? head - 0xca : head == 0xcf ? 4 : head - 0xcf;
        const std::array<script::Value, 4> a{*f, script::Value{64}, script::Value{index}, script::Value{0}};
        const auto ammoValue = m_engine->callBuiltin(67, a);
        const int ammo = ammoValue ? ammoValue->toInt() : -1;
        const std::string text = WeaponHud::label(requested, name ? name->toString() : std::string{}, ammo,
                                                  requested != w.current);
        // darkened box behind the text: the game font is green, like much of the scenery
        const int x0 = 6, y0 = m_outH - 30, x1 = std::min(m_viewW, 18 + 7 * static_cast<int>(text.size())), y1 = m_outH - 8;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                auto& p = fb[y, x];
                p = RGBA8{{static_cast<std::uint8_t>(p.r >> 2), static_cast<std::uint8_t>(p.g >> 2), static_cast<std::uint8_t>(p.b >> 2)}, 255};
            }
        m_ui.fonts().drawText(text, ui::vids::Font, 12, m_outH - 24, fb);
    }

    [[nodiscard]] bool textEntry() const {
        // PLAYER.INI: reset_name == 1 while the player types the profile name.
        return m_vm && iequalsSuffix(m_map, "player.map") && globalInt("reset_name") == 1;
    }

    void present() {
        std::ranges::fill(m_xrgb, 0u);
        for (int y = 0; y < m_outH; ++y) {
            const RGBA8* src = m_view.data() + static_cast<std::size_t>(y) * m_viewW;
            std::uint32_t* dst = m_xrgb.data() + static_cast<std::size_t>(y) * m_outW + m_viewX;
            for (int x = 0; x < m_viewW; ++x)
                dst[x] = (std::uint32_t{src[x].r} << 16) | (std::uint32_t{src[x].g} << 8) | src[x].b;
        }
        drawCursor();
    }

    void drawCursor() {
        if (!m_cursors || m_cursorType < 0)
            return;
        if (m_pc.cursorHidden && !m_demo && !m_ui.hasMenu() && !m_movieActive)
            return; // direct scheme without the reticle (issue #19)
        int x = m_pc.cursorX(), y = m_pc.cursorY();
        input::CursorShape shape = input::CursorShape::Arrow;
        if (m_demo) {
            x = m_input.mouseScreenX + m_viewX;
            y = m_input.mouseScreenY;
        } else if (!m_ui.hasMenu() && m_mapLoaded) {
            switch (m_engine->cursorAt(x - m_viewX, y)) {
            case CursorKind::Arrow: shape = input::CursorShape::Arrow; break;
            case CursorKind::Move: shape = input::CursorShape::Move; break;
            case CursorKind::Fight: shape = input::CursorShape::Fight; break;
            case CursorKind::NoMove: shape = input::CursorShape::NoMove; break;
            case CursorKind::Cycle: shape = input::CursorShape::Cycle; break;
            }
        }
        input::drawCursor(m_cursors->get(shape), x, y, m_xrgb.data(), m_outW, m_outH, pitch());
    }

    // ---- state ----------------------------------------------------------------------------------------
    std::span<const Vid> m_vids;
    std::span<const SoundData> m_sounds;
    std::unique_ptr<Engine> m_engine;
    Mixer* m_mixer = nullptr;
    std::unique_ptr<MoviePlayer> m_movies;
    SessionConfig m_config;
    SaveFiles m_files;
    SessionUi m_ui;
    script::FileSystemSourceLoader m_loader;
    script::BuiltinRegistry m_builtins;
    std::unique_ptr<script::Interpreter> m_vm;
    std::optional<script::GlobalSnapshot> m_carry; // cells carried across map loads
    InputRecordBuilder m_inputBuilder;
    FollowCamera m_follow;     // issue #16
    bool m_followOn = false;
    WeaponHud m_weaponHud;
    const input::CursorSet* m_cursors = nullptr;

    std::string m_map;
    std::vector<std::string> m_history;
    bool m_mapLoaded = false;
    std::deque<Op> m_ops;
    OriginalInput m_input;
    input::PcState m_pc, m_filtered;
    input::Context m_lastContext = input::Context::Menu;
    std::array<bool, input::KeyCount> m_staleKey{};
    bool m_staleLeft = false, m_staleRight = false, m_prevAnyDown = false;

    std::vector<RGBA8> m_view;      // m_viewW x m_outH, what the engine and the menus draw on
    std::vector<std::uint32_t> m_xrgb;
    int m_outW = 640, m_outH = 480, m_viewW = 640, m_viewX = 0;
    int m_screenW = 640, m_screenH = 480; // script screen (SetGraphMode)
    bool m_demoView = false;

    double m_realMs = 0, m_msDebt = 0, m_demoDebtMs = 0;
    std::uint64_t m_frames = 0, m_ticks = 0;
    std::optional<DemoPlayer> m_demo;
    std::string m_lastDemoLine;
    std::optional<DemoRecorder> m_recorder;
    std::string m_recordPath, m_recordingTo;

    bool m_movieActive = false;
    std::string m_lastMovie;

    MixerSink m_sfxSink;
    SoundSink* m_sink = &m_sfxSink;    // replaced by tools (game_trace) through setSoundSink()
    Camera m_soundCam;                 // camera during tactWorld() (positional sounds)
    std::vector<ui::ScreenSound> m_screenSounds;   // menu and text sounds of the frame
    SoundChannels m_sfx{[this](int n) { return sound(n); },
                        [this] { return playWavSlot(); }};
    std::deque<SoundData> m_wavs;  // PlayWAV
    bool m_wavLoaded = false;      // m_wavs.back() is in slot N
    std::vector<std::string> m_wavNames; // PlayWAV file of each m_wavs entry (save states)
    std::string m_demoName;              // .dem being played (save states)
    std::array<int, 4> m_playlist{};
    bool m_playing = false;
    int m_scriptSfx = 100, m_scriptMusic = 100;
    float m_coreMusic = 0.8f, m_coreSfx = 1.0f;

    std::uint32_t m_messageClock = 0;   // statebar +0x24: time of the last Message()
    bool m_scrollLock = false, m_stateBar = false, m_detail = true, m_exit = false;
    int m_cursorType = 0, m_net = 0;
    std::size_t m_scriptErrors = 0, m_warnings = 0, m_unknown = 0;
};

} // namespace gromada::game
