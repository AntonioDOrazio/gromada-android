// Gromada libretro core: runs the original game (game session, issue #10: logo, movies,
// menus, briefings, levels, demos driven by the original scripts) on the original data.
// The level viewer of milestones M1/M2 stays reachable with the debug core option
// "Modalità visualizzatore". 60/120 fps and 4:3/16:9 are core options.
// Audio: sound effects from fw.res and the CD soundtrack (music/trackNN.ogg).
//
// This is the ONLY translation unit of the core (libgromada is header-implemented).

#include "core/std.hpp"
#include "engine/audio.hpp"
#include "engine/level.hpp"
#include "engine/renderer.hpp"
#include "game/session_movie.hpp"
#include "game/engine.hpp"
#include "game/savestate.hpp"
#include "input/input.hpp"
#include "libretro/libretro.h"
#include "video/movie.hpp"

#include <cstdarg>

namespace {

constexpr int AudioRate = 44100;
constexpr int LevelCount = 30;

retro_environment_t        env_cb;
retro_video_refresh_t      video_cb;
retro_audio_sample_batch_t audio_batch_cb;
retro_input_poll_t         input_poll_cb;
retro_input_state_t        input_state_cb;
retro_log_printf_t         log_cb;

void log(retro_log_level level, const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    if (log_cb)
        log_cb(level, "[Gromada] %s\n", buf);
    else
        std::fprintf(stderr, "[Gromada] %s\n", buf);
}

struct Options {
    int fps = 60;
    bool widescreen = false;
    int startLevel = 1;
    int musicVolume = 80;
    int sfxVolume = 100;
    bool viewer = false;   // debug: level viewer instead of the game
    int demo = 0;          // debug: play maps/demoN.dem (0 = none)
    // presentation (issue #20, docs/rendering.md)
    int renderScale = 1;   // internal resolution: the picture is k x 640 x 480 (k x 854 x 480)
    bool smooth = true;    // positions extrapolated between the ticks of the simulation
    float zoom = 1;        // zoom of the view when the option changes (then L2/R2, A/B)
    int renderThreads = 0; // threads drawing the picture (0 = automatic)
};

struct Core {
    std::unique_ptr<GameResources> resources;
    std::unique_ptr<gromada::Level> level;
    gromada::Renderer renderer;
    Options options;
    int levelIndex = 1;
    float camX = 0, camY = 0;
    bool prevL = false, prevR = false, prevA = false, prevB = false;
    int soundTest = -1;
    double audioDebt = 0;
    std::vector<std::int16_t> audioBuf;
    gromada::Mixer mixer{AudioRate};
    // --- input (issue #4) ---
    gromada::input::Mapper input;
    gromada::input::CursorSet cursors;
    float cursorBusy = 0; // viewer: CYCLE cursor for a moment after a level load
    // --- game session (issue #10) ---
    std::unique_ptr<gromada::game::Session> session;
    std::filesystem::path gameDir, saveDir;
    // --- save states (issue #14, docs/savestates.md) ---
    gromada::game::SaveStateManager saveStates;
    std::vector<std::uint8_t> stateBuffer;
};

std::unique_ptr<Core> core;

// --- options ---------------------------------------------------------------

std::string levelValues() {
    std::string values = "Livello (visualizzatore); ";
    for (int i = 1; i <= LevelCount; ++i) {
        values += std::to_string(i);
        if (i != LevelCount) values += '|';
    }
    return values;
}

void declareOptions() {
    static const std::string levels = levelValues();
    static const retro_variable vars[] = {
        {"gromada_fps", "Frame rate; 60|120"},
        {"gromada_aspect", "Formato; 4:3|16:9"},
        {"gromada_viewer", "Modalità visualizzatore (debug); disattivata|attivata"},
        {"gromada_level", levels.c_str()},
        {"gromada_demo", "Demo (debug); nessuna|1|2|3"},
        {"gromada_music_volume", "Volume musica; 80|90|100|0|10|20|30|40|50|60|70"},
        {"gromada_sfx_volume", "Volume effetti; 100|0|10|20|30|40|50|60|70|80|90"},
        // input (issue #4): scheme, cursor speed/acceleration/precision, touch/mouse, keyboard
        gromada::input::coreOptions[0], gromada::input::coreOptions[1], gromada::input::coreOptions[2],
        gromada::input::coreOptions[3], gromada::input::coreOptions[4], gromada::input::coreOptions[5],
        gromada::input::coreOptions[6], gromada::input::coreOptions[7], // camera, aim assist (issue #16)
        gromada::input::coreOptions[8], gromada::input::coreOptions[9], // reticle, lock corridor (issue #19)
        {"gromada_movie", "Riproduci filmato (Start/A/B per saltare); nessuno|0|1|2|3"},
        // presentation (issue #20)
        {"gromada_render_scale", "Risoluzione interna; 1x|2x|3x|4x"},
        {"gromada_smooth", "Movimento fluido (interpolazione); attivato|disattivato"},
        {"gromada_zoom", "Zoom della vista; 1x|0.5x|0.75x|1.5x|2x"},
        {"gromada_render_threads", "Thread di disegno; automatico|1|2|3|4"},
        {nullptr, nullptr},
    };
    static_assert(std::size(gromada::input::coreOptions) == 10, "list every input option above");
    env_cb(RETRO_ENVIRONMENT_SET_VARIABLES, const_cast<retro_variable*>(vars));
}

const char* getVar(const char* key) {
    retro_variable var{key, nullptr};
    if (env_cb && env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        return var.value;
    return nullptr;
}

Options readOptions() {
    Options o;
    if (const char* v = getVar("gromada_fps")) o.fps = std::strcmp(v, "120") == 0 ? 120 : 60;
    if (const char* v = getVar("gromada_aspect")) o.widescreen = std::strcmp(v, "16:9") == 0;
    if (const char* v = getVar("gromada_level")) o.startLevel = std::clamp(std::atoi(v), 1, LevelCount);
    if (const char* v = getVar("gromada_music_volume")) o.musicVolume = std::clamp(std::atoi(v), 0, 100);
    if (const char* v = getVar("gromada_sfx_volume")) o.sfxVolume = std::clamp(std::atoi(v), 0, 100);
    if (const char* v = getVar("gromada_viewer")) o.viewer = std::strcmp(v, "attivata") == 0;
    if (const char* v = getVar("gromada_demo")) o.demo = std::clamp(std::atoi(v), 0, 3);
    if (const char* v = getVar("gromada_render_scale")) o.renderScale = std::clamp(std::atoi(v), 1, 4);
    if (const char* v = getVar("gromada_smooth")) o.smooth = std::strcmp(v, "disattivato") != 0;
    if (const char* v = getVar("gromada_zoom")) o.zoom = std::clamp(static_cast<float>(std::atof(v)), 0.5f, 2.0f);
    if (const char* v = getVar("gromada_render_threads")) o.renderThreads = std::clamp(std::atoi(v), 0, 4);
    return o;
}

retro_system_av_info makeAvInfo(const Options& o) {
    retro_system_av_info info{};
    const int width = o.widescreen ? gromada::Renderer::MaxWidth : 640;
    // The viewer draws at 1x; the game at k x (internal resolution, issue #20). The maximum
    // covers both aspects, so switching the aspect only needs SET_GEOMETRY.
    const int k = o.viewer ? 1 : o.renderScale;
    info.geometry.base_width = width * k;
    info.geometry.base_height = gromada::Renderer::Height * k;
    info.geometry.max_width = gromada::Renderer::MaxWidth * k;
    info.geometry.max_height = gromada::Renderer::Height * k;
    info.geometry.aspect_ratio = o.widescreen ? 16.0f / 9.0f : 4.0f / 3.0f;
    info.timing.fps = o.fps;
    info.timing.sample_rate = AudioRate;
    return info;
}

// --- level handling --------------------------------------------------------

void notify(const std::string& text) {
    retro_message msg{text.c_str(), 180};
    env_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
}

bool loadLevel(int index) {
    const auto path = core->resources->mapsPath() / "levels" / std::format("{:02}.map", index);
    try {
        auto map = loadMap(core->resources->vids(), path);
        core->level = std::make_unique<gromada::Level>(*core->resources, map);
        core->levelIndex = index;
        const auto& h = core->level->header();
        core->camX = static_cast<float>(h.observerX);
        core->camY = static_cast<float>(h.observerY);
        core->cursorBusy = 1.0f;
        log(RETRO_LOG_INFO, "level %02d: %ux%u, %zu entities", index, h.width, h.height, core->level->entities().size());
        const auto script = core->resources->mapsPath() / "levels" / std::format("{:02}.INI", index);
        if (const auto playlist = gromada::findLevelPlaylist(script)) {
            core->mixer.playMusic(*playlist);
            notify(std::format("Livello {:02} - musica: traccia {}", index, (*playlist)[0]));
        } else {
            core->mixer.stopMusic();
            notify(std::format("Livello {:02}", index));
        }
        return true;
    } catch (const std::exception& e) {
        log(RETRO_LOG_ERROR, "cannot load %s: %s", path.string().c_str(), e.what());
        return false;
    }
}

// --- input (issue #4) ------------------------------------------------------
// Gameplay mapping comes from src/input (see docs/controls.md). Viewer-only extras sit on
// buttons the game does not use: L2/R2 = level, L3/R3 = sound test, Y = fast scroll.

void applyInputOptions() {
    core->input.configure(gromada::input::readSettings(getVar));
    static std::vector<retro_input_descriptor> desc;
    const auto game = gromada::input::inputDescriptors(core->input.settings().scheme);
    desc.assign(game.begin(), game.end());
    if (core->options.viewer)
        desc.insert(desc.end(), {
            {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Anteprima: scorrimento veloce"},
            {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "Anteprima: livello precedente"},
            {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "Anteprima: livello successivo"},
            {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "Anteprima: effetto sonoro precedente"},
            {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3, "Anteprima: effetto sonoro successivo"},
        });
    desc.push_back({0, 0, 0, 0, nullptr});
    env_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc.data());
}

const gromada::input::PcState& updateInput(float dt) {
    using namespace gromada::input;
    FrameInfo f;
    f.dt = dt;
    f.width = core->renderer.width();
    f.height = core->renderer.height();
    f.player = nullptr; // the viewer has no player yet: aim from the screen center
    const PcState& pc = core->input.update(pollRaw(input_state_cb, core->input.settings()), f);
    for (int k = 0; k < 4; ++k)
        if (pc.key(static_cast<Key>(static_cast<int>(Key::Num1) + k)).pressed)
            notify(std::format("Arma {}", k + 1));
    return pc;
}

// Viewer stand-in for the engine's choice (which will depend on what is under the cursor).
gromada::input::CursorShape viewerCursorShape(const gromada::input::PcState& pc, bool blockedScroll) {
    using enum gromada::input::CursorShape;
    if (core->cursorBusy > 0) return Cycle;
    if (pc.right.down || pc.aiming) return Fight;
    if (pc.left.down) return Move;
    if (blockedScroll) return NoMove;
    return Arrow;
}

void drawCursor(const gromada::input::PcState& pc, bool blockedScroll) {
    auto& r = core->renderer;
    // The renderer owns a mutable buffer; it is redrawn from scratch every frame.
    gromada::input::drawCursor(core->cursors.get(viewerCursorShape(pc, blockedScroll)), pc.cursorX(), pc.cursorY(),
                               const_cast<std::uint32_t*>(r.xrgb()), r.width(), r.height(), r.pitch());
}
void checkMovieOption(); // movies, see below
void stopMovie();
void startSession();      // game session, see below

void applyOptions(bool initial) {
    const Options next = readOptions();
    const Options prev = core->options;
    core->options = next;
    core->renderer.setWidth(next.widescreen ? gromada::Renderer::MaxWidth : 640);
    core->mixer.setMusicVolume(next.musicVolume / 100.0f);
    core->mixer.setSfxVolume(next.sfxVolume / 100.0f);
    applyInputOptions();
    if (next.viewer)
        checkMovieOption();
    if (core->session) {
        core->session->setVolumes(next.musicVolume / 100.0f, next.sfxVolume / 100.0f);
        core->session->setRenderScale(next.renderScale);
        core->session->setSmoothMotion(next.smooth);
        core->session->setRenderThreads(next.renderThreads);
        if (initial || next.zoom != prev.zoom)
            core->session->setZoom(next.zoom);
        core->session->setOutputSize(core->renderer.width(), core->renderer.height());
    }
    if (initial)
        return;

    auto av = makeAvInfo(next);
    if (next.fps != prev.fps || next.renderScale != prev.renderScale || next.viewer != prev.viewer)
        env_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av);
    else if (next.widescreen != prev.widescreen)
        env_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &av.geometry);

    if (next.viewer != prev.viewer) {
        if (next.viewer) {
            core->session.reset();
            loadLevel(core->levelIndex);
        } else {
            stopMovie();
            core->level.reset();
            startSession();
        }
    } else if (next.viewer && next.startLevel != prev.startLevel) {
        loadLevel(next.startLevel);
    }
    if (!next.viewer && core->session && next.demo != prev.demo && next.demo)
        core->session->playDemo(next.demo);
}

// --- per-frame ---------------------------------------------------------------

bool button(unsigned id) {
    return input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, id) != 0;
}

// Viewer camera: arrow keys (d-pad; left stick in the direct scheme) stand in for the
// player's movement, and the cursor scrolls RTS-style at the screen edges (not with touch:
// taps near the border would scroll). Returns true when an edge push is blocked by the map
// border (NOMOVE cursor).
bool updateCamera(float dt, const gromada::input::PcState& pc) {
    gromada::input::Vec2 dir = pc.move;
    gromada::input::Vec2 edge;
    if (pc.source != gromada::input::PointerSource::Touch)
        edge = gromada::input::edgeScroll(pc.cursor, core->renderer.width(), core->renderer.height());
    if (edge.x) dir.x = edge.x;
    if (edge.y) dir.y = edge.y;

    const float speed = button(RETRO_DEVICE_ID_JOYPAD_Y) ? 1200.0f : 400.0f; // px/s
    const float prevX = core->camX, prevY = core->camY;
    core->camX += dir.x * speed * dt;
    core->camY += dir.y * speed * dt;

    const auto& h = core->level->header();
    const float maxX = std::max(0.0f, static_cast<float>(h.width) - core->renderer.width());
    const float maxY = std::max(0.0f, static_cast<float>(h.height) - core->renderer.height());
    core->camX = std::clamp(core->camX, 0.0f, maxX);
    core->camY = std::clamp(core->camY, 0.0f, maxY);
    return (edge.x && core->camX == prevX) || (edge.y && core->camY == prevY);
}

void handleLevelSwitch() {
    const bool l = button(RETRO_DEVICE_ID_JOYPAD_L2);
    const bool r = button(RETRO_DEVICE_ID_JOYPAD_R2);
    if (l && !core->prevL) loadLevel(core->levelIndex == 1 ? LevelCount : core->levelIndex - 1);
    if (r && !core->prevR) loadLevel(core->levelIndex == LevelCount ? 1 : core->levelIndex + 1);
    core->prevL = l;
    core->prevR = r;
}

// Sound test (viewer only, until gameplay triggers effects): R3 = next effect, L3 = previous.
void handleSoundTest() {
    const bool a = button(RETRO_DEVICE_ID_JOYPAD_R3);
    const bool b = button(RETRO_DEVICE_ID_JOYPAD_L3);
    const int count = static_cast<int>(core->resources->sounds().size());
    if (count > 0 && ((a && !core->prevA) || (b && !core->prevB))) {
        core->soundTest = a && !core->prevA ? (core->soundTest + 1) % count
                                            : (core->soundTest <= 0 ? count - 1 : core->soundTest - 1);
        core->mixer.playSfx(core->resources->sounds()[core->soundTest]);
        notify(std::format("Effetto {}/{}", core->soundTest, count - 1));
    }
    core->prevA = a;
    core->prevB = b;
}

void pushAudio() {
    // 44100 / 120 is not an integer: carry the fractional sample over to the next frame.
    core->audioDebt += static_cast<double>(AudioRate) / core->options.fps;
    const auto frames = static_cast<std::size_t>(core->audioDebt);
    core->audioDebt -= frames;
    if (!frames)
        return;
    core->audioBuf.resize(frames * 2);
    core->mixer.render(core->audioBuf.data(), frames);
    audio_batch_cb(core->audioBuf.data(), frames);
}

// --- movies (avi/N.avi) ------------------------------------------------------
// Played on top of the viewer: picture scaled into the framebuffer (pillarboxed in 16:9),
// soundtrack through the mixer's streaming source, music muted meanwhile.

struct MovieState {
    std::unique_ptr<gromada::video::Movie> movie;
    std::vector<std::uint32_t> fb;
    std::string option = "nessuno"; // last value of gromada_movie
    long outputFrames = 0;
    bool prevSkip = true;
} movieState;

void stopMovie() {
    auto& m = movieState;
    if (!m.movie)
        return;
    log(RETRO_LOG_INFO, "movie stopped after %ld output frames (%.2f s at %d fps), %zu decode errors",
        m.outputFrames, m.outputFrames / double(core->options.fps), core->options.fps, m.movie->decodeErrors());
    core->mixer.setStream(nullptr);
    core->mixer.setMusicVolume(core->options.musicVolume / 100.0f);
    core->prevA = button(RETRO_DEVICE_ID_JOYPAD_A); // the skip press must not reach the viewer
    core->prevB = button(RETRO_DEVICE_ID_JOYPAD_B);
    m.movie.reset();
}

void startMovie(int index) {
    stopMovie();
    if (!core->resources)
        return;
    const auto dir = core->resources->mapsPath().parent_path();
    for (const char* fmt : {"avi/{}.avi", "AVI/{}.AVI", "avi/{}.AVI", "AVI/{}.avi"}) {
        const auto path = dir / std::vformat(fmt, std::make_format_args(index));
        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
            continue;
        try {
            auto& m = movieState;
            m.movie = std::make_unique<gromada::video::Movie>(path);
            m.fb.assign(gromada::Renderer::MaxWidth * gromada::Renderer::Height, 0);
            m.outputFrames = 0;
            m.prevSkip = true; // a button held when the movie starts does not skip it
            core->mixer.setStream(m.movie->audio());
            log(RETRO_LOG_INFO, "movie %s: %dx%d, %zu frames at %.3f fps (%.2f s), audio %.2f s", path.string().c_str(),
                m.movie->width(), m.movie->height(), m.movie->frameCount(), m.movie->frameRate(), m.movie->videoDuration(),
                m.movie->audioDuration());
        } catch (const std::exception& e) {
            log(RETRO_LOG_ERROR, "cannot play %s: %s", path.string().c_str(), e.what());
        }
        return;
    }
    log(RETRO_LOG_WARN, "movie %d not found in %s/avi", index, dir.string().c_str());
}

// Starts the movie chosen in the core option, when the option changes (or at load).
void checkMovieOption() {
    const char* v = getVar("gromada_movie");
    const std::string value = v ? v : "nessuno";
    if (value == movieState.option)
        return;
    movieState.option = value;
    if (value.size() == 1 && value[0] >= '0' && value[0] <= '9')
        startMovie(value[0] - '0');
}

// Runs one output frame of the movie; false when no movie is playing.
bool runMovie() {
    auto& m = movieState;
    if (!m.movie)
        return false;
    const bool skip = button(RETRO_DEVICE_ID_JOYPAD_START) || button(RETRO_DEVICE_ID_JOYPAD_A) || button(RETRO_DEVICE_ID_JOYPAD_B);
    const bool skipPressed = skip && !m.prevSkip;
    m.prevSkip = skip;
    if (skipPressed || m.movie->finished()) {
        stopMovie();
        return false;
    }
    core->mixer.setMusicVolume(0.0f);
    m.movie->step(1.0 / core->options.fps);
    const int w = core->renderer.width(), h = core->renderer.height();
    m.movie->blit(m.fb.data(), w, h, static_cast<std::size_t>(w));
    video_cb(m.fb.data(), w, h, static_cast<std::size_t>(w) * 4);
    pushAudio();
    ++m.outputFrames;
    return true;
}

// --- game session (issue #10) ----------------------------------------------------------
// The session runs the original game on an implementation of game::Engine.

// THE construction point of the simulation engine (issue #8). The provisional ViewerEngine
// (src/game/viewer_engine.hpp) stays available for tests and debugging.
std::unique_ptr<gromada::game::Engine> makeGameEngine(const GameResources& resources) {
    return std::make_unique<gromada::game::GameEngine>(resources);
}

// Save files (save1..5, fw.ini, recorded demos) live in <frontend save dir>/gromada.
std::filesystem::path findSaveDir() {
    const char* dir = nullptr;
    if (env_cb && env_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir && *dir)
        return std::filesystem::path{dir} / "gromada";
    log(RETRO_LOG_WARN, "no save directory from the frontend: profiles and options will not be saved");
    return {};
}

// GROMADA_TRACE_VIEW=<file> (environment): one line per drawn frame with the drawn position
// of the flagman and of one moving enemy vehicle (world and output pixels) and the view's
// origin and zoom, to measure how regular the motion on screen is (issue #20, docs/rendering.md).
void installViewTrace(gromada::game::Session& s) {
    static FILE* out = [] {
        const char* path = std::getenv("GROMADA_TRACE_VIEW");
        return path && *path ? std::fopen(path, "w") : nullptr;
    }();
    if (!out)
        return;
    static int tracked = 0;
    static std::map<int, std::pair<float, float>> last;
    s.setWorldPass([&s](const gromada::game::WorldPass& p) {
        static long frame = 0;
        const float k = static_cast<float>(p.renderScale);
        const gromada::game::DrawnSprite* f = nullptr;
        const gromada::game::DrawnSprite* v = nullptr;
        const gromada::game::DrawnSprite* m = nullptr;
        static int missile = 0;
        bool missileSeen = false;
        for (const auto& d : p.sprites)
            if (!d.child && d.id == missile) missileSeen = true;
        if (!missileSeen) missile = 0;
        for (const auto& d : p.sprites) {
            if (d.child || d.screen) continue;
            if (d.cls == 17 && !f) f = &d;   // Kassandra
            if (d.cls == 5 || d.cls == 6 || d.cls == 14 || d.cls == 15) {   // projectiles: follow one that moves
                static std::map<int, std::pair<float, float>> seen;
                const auto it = seen.find(d.id);
                if (!missile && it != seen.end() && (it->second.first != d.worldX || it->second.second != d.worldY)) missile = d.id;
                seen[d.id] = {d.worldX, d.worldY - d.worldZ};
                if (d.id == missile) m = &d;
            }
            if (d.cls == 2) {                // Vehicle: follow the first one seen moving
                const auto it = last.find(d.id);
                if (!tracked && it != last.end() && (it->second.first != d.worldX || it->second.second != d.worldY)) tracked = d.id;
                last[d.id] = {d.worldX, d.worldY};
                if (d.id == tracked) v = &d;
            }
        }
        std::fprintf(out, "%ld %.3f %.3f %.3f %.4f %s %.3f %.3f %.3f %.3f %d %.3f %.3f %.3f %.3f %d %.3f %.3f %.3f\n", frame++,
                     p.renderClock, p.origin.x, p.origin.y, p.zoom, s.currentMap().c_str(), f ? f->fbX / k : -1.f,
                     f ? f->fbY / k : -1.f, f ? f->worldX : -1.f, f ? f->worldY : -1.f, v ? v->id : 0, v ? v->fbX / k : -1.f,
                     v ? v->fbY / k : -1.f, v ? v->worldX : -1.f, v ? v->worldY : -1.f, m ? m->id : 0, m ? m->worldX : -1.f,
                     m ? m->worldY : -1.f, m ? m->worldZ : -1.f);
    });
}

void startSession() {
    using namespace gromada::game;
    SessionConfig config;
    config.gameDir = core->gameDir;
    config.saveDir = core->saveDir;
    config.outputWidth = core->renderer.width();
    config.outputHeight = core->renderer.height();
    config.renderScale = core->options.renderScale;
    config.smoothMotion = core->options.smooth;
    config.renderThreads = core->options.renderThreads;
    config.log = [](LogLevel level, std::string_view text) {
        static constexpr retro_log_level levels[] = {RETRO_LOG_DEBUG, RETRO_LOG_INFO, RETRO_LOG_WARN, RETRO_LOG_ERROR};
        log(levels[static_cast<int>(level)], "%.*s", static_cast<int>(text.size()), text.data());
    };
    core->session.reset();
    core->mixer.stopMusic();
    core->session = std::make_unique<Session>(core->resources->vids(), core->resources->sounds(),
                                              makeGameEngine(*core->resources), &core->mixer,
                                              std::make_unique<VideoMoviePlayer>(), std::move(config));
    core->session->setCursors(&core->cursors);
    core->session->setZoom(core->options.zoom);
    installViewTrace(*core->session);
    core->session->setVolumes(core->options.musicVolume / 100.0f, core->options.sfxVolume / 100.0f);
    core->session->start();
    if (core->options.demo)
        core->session->playDemo(core->options.demo);
}

// The game as the controls see it (issue #16): player and targets on screen, weapons.
struct SessionPlayer final : gromada::input::PlayerPosition {
    std::optional<gromada::input::Vec2> position;
    [[nodiscard]] std::optional<gromada::input::Vec2> screenPosition() const override { return position; }
} sessionPlayer;

// GROMADA_TRACE=<n> (environment): one log line every n frames with the player, camera,
// weapons and targets, for the automated gameplay tests (tests/controls_e2e.sh).
int traceEvery() {
    static const int every = [] {
        const char* v = std::getenv("GROMADA_TRACE");
        return v ? std::max(0, std::atoi(v)) : 0;
    }();
    return every;
}

void runSession(float dt) {
    using namespace gromada::input;
    auto& s = *core->session;
    const gromada::game::ControlsFrame controls = s.controlsFrame();
    sessionPlayer.position = controls.player;
    FrameInfo f;
    f.dt = dt;
    f.width = s.logicalWidth();   // the input works on the logical picture (640/854 x 480)
    f.height = s.logicalHeight();
    f.context = s.inputContext();
    f.player = &sessionPlayer;
    f.weaponMask = controls.weaponMask;
    f.currentWeapon = controls.currentWeapon;
    f.aimDistance = controls.weaponRange;
    f.weaponKind = controls.weaponKind;
    f.aimLift = controls.aimLift;
    f.zoom = static_cast<float>(s.mapping().zoom);   // screen px per world px of the last picture (issue #20)
    f.targets = controls.targets;
    const Settings& set = core->input.settings();
    const PcState& pc = core->input.update(pollRaw(input_state_cb, set), f);
    s.setCameraFollow(set.camera == CameraFollow::Always ||
                      (set.camera == CameraFollow::Auto && (set.scheme == Scheme::Direct || pc.source == PointerSource::Touch)));
    s.runFrame(pc, dt);
    static long frame = 0;
    if (const int every = traceEvery(); every > 0 && frame++ % every == 0) {
        const auto& cam = s.engine().camera();
        Vec2 nearest{-9999, -9999}; // closest target, ground point relative to the player, and its hp
        int nearestHp = -1;
        if (controls.player)
            for (std::size_t i = 0; i < controls.targets.size(); ++i)
                if ((controls.targets[i].ground - *controls.player).length() < (nearest).length()) {
                    nearest = controls.targets[i].ground - *controls.player;
                    nearestHp = i < controls.targetHps.size() ? controls.targetHps[i] : -1;
                }
        const auto& rec = s.lastInput(); // this frame's input record (mouse point in the world, buttons)
        log(RETRO_LOG_INFO, "trace map=%s ctx=%d flagman=%d,%d cam=%d,%d cursor=%d,%d weapon=%d mask=%u pending=%d "
            "targets=%d hp=%d order=%d locked=%d nearest=%d,%d nhp=%d mouse=%d,%d bits=%u kind=%d range=%d zoom=%d hidden=%d",
            s.currentMap().c_str(), static_cast<int>(f.context),
            controls.player ? controls.flagmanX : -1, controls.player ? controls.flagmanY : -1, cam.x, cam.y,
            pc.cursorX(), pc.cursorY(), controls.currentWeapon, static_cast<unsigned>(controls.weaponMask),
            pc.weaponPending ? pc.weaponSlot : -1, controls.targetCount, controls.targetHp, pc.moveOrder ? 1 : 0,
            pc.aimLocked ? 1 : 0, static_cast<int>(nearest.x), static_cast<int>(nearest.y), nearestHp, rec.mouseWorldX,
            rec.mouseWorldY, static_cast<unsigned>(rec.bits), static_cast<int>(controls.weaponKind),
            static_cast<int>(controls.weaponRange), static_cast<int>(std::lround(pc.zoomRate * 100)), pc.cursorHidden ? 1 : 0);
    }
    if (s.exitRequested()) {
        // Exit() from the scripts (credits): close the content; without that, start over.
        if (!env_cb(RETRO_ENVIRONMENT_SHUTDOWN, nullptr)) {
            log(RETRO_LOG_INFO, "Exit(): the frontend cannot shut down, restarting the game");
            startSession();
        } else {
            s.clearExit();
        }
    }
    video_cb(core->session->xrgb(), core->session->width(), core->session->height(), core->session->pitch());
    pushAudio();
}

} // namespace

// --- libretro API ------------------------------------------------------------

RETRO_API unsigned retro_api_version() { return RETRO_API_VERSION; }

RETRO_API void retro_set_environment(retro_environment_t cb) {
    env_cb = cb;
    retro_log_callback logging{};
    if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
        log_cb = logging.log;
    declareOptions();
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t) {}
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }
RETRO_API void retro_set_controller_port_device(unsigned, unsigned) {}

RETRO_API void retro_get_system_info(retro_system_info* info) {
    *info = {};
    info->library_name = "Gromada";
    info->library_version = "0.3.0-session";
    info->valid_extensions = "exe|res";
    info->need_fullpath = true;
    info->block_extract = true;
}

RETRO_API void retro_get_system_av_info(retro_system_av_info* info) {
    *info = makeAvInfo(core ? core->options : readOptions());
}

RETRO_API void retro_init() { core = std::make_unique<Core>(); }
RETRO_API void retro_deinit() { stopMovie(); if (core) core->session.reset(); core.reset(); }
RETRO_API void retro_reset() {
    if (!core || !core->resources)
        return;
    if (core->options.viewer) loadLevel(core->levelIndex);
    else startSession();
}

RETRO_API bool retro_load_game(const retro_game_info* game) {
    if (!game || !game->path)
        return false;

    auto fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
        log(RETRO_LOG_ERROR, "XRGB8888 not supported by frontend");
        return false;
    }

    // Input descriptors: applyInputOptions() (they depend on the control scheme).

    // Content can be gromada.exe or fw.res: both live in the game root folder.
    const auto gameDir = std::filesystem::path{game->path}.parent_path();
    const auto resPath = gameDir / "fw.res";
    try {
        core->resources = std::make_unique<GameResources>(resPath);
    } catch (const std::exception& e) {
        log(RETRO_LOG_ERROR, "cannot read %s: %s", resPath.string().c_str(), e.what());
        return false;
    }
    log(RETRO_LOG_INFO, "fw.res: %zu vids, %zu sounds, %zu weapons",
        core->resources->vids().size(), core->resources->sounds().size(), core->resources->weapons().size());

    core->mixer.setMusicDirectories({gameDir / "music", gameDir / "MUSIC", gameDir});
    log(RETRO_LOG_INFO, "cursors: %d/5 original cursors loaded", core->cursors.load(gameDir));
    core->gameDir = gameDir;
    core->saveDir = findSaveDir();
    applyOptions(true);
    if (core->options.viewer)
        return loadLevel(core->options.startLevel);
    startSession();
    return true;
}

RETRO_API void retro_unload_game() {
    stopMovie();
    movieState.option.clear(); // replay the chosen movie at the next load
    core->session.reset();
    core->level.reset();
    core->resources.reset();
}

RETRO_API void retro_run() {
    bool updated = false;
    if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
        applyOptions(false);

    input_poll_cb();
    const float dt = 1.0f / core->options.fps;
    if (core->session) {
        runSession(dt);
        return;
    }
    if (!core->level)
        return;
    // The input layer keeps tracking buttons while a movie plays, so the press that skips
    // a movie is consumed there and does not reach the game as a fresh click.
    const auto& pc = updateInput(dt);
    if (runMovie())
        return;
    handleLevelSwitch();
    handleSoundTest();

    const bool blockedScroll = updateCamera(dt, pc);
    core->cursorBusy = std::max(0.0f, core->cursorBusy - dt);
    core->level->animate(dt);

    auto& r = core->renderer;
    r.draw(*core->level, static_cast<int>(core->camX), static_cast<int>(core->camY));
    drawCursor(pc, blockedScroll);
    video_cb(r.xrgb(), r.width(), r.height(), r.pitch());
    pushAudio();
}

RETRO_API unsigned retro_get_region() { return RETRO_REGION_NTSC; }
RETRO_API bool retro_load_game_special(unsigned, const retro_game_info*, size_t) { return false; }

// Save states (issue #14, docs/savestates.md): the whole session, engine, script VM, UI,
// mixer and input mapper. The size is a fixed upper bound for the whole session (libretro
// forbids it to grow); the unused tail is zero-filled.
RETRO_API size_t retro_serialize_size() {
    return core && core->resources ? gromada::game::SaveStateMaxSize : 0;
}
RETRO_API bool retro_serialize(void* data, size_t size) {
    if (!core || !core->session || !data)
        return false;
    const gromada::game::SaveStateExtras extras{&core->input, &core->audioDebt};
    if (!core->saveStates.save(*core->session, core->stateBuffer, extras)) {
        log(RETRO_LOG_WARN, "save state: %s", core->saveStates.error().c_str());
        return false;
    }
    if (core->stateBuffer.size() > size) {
        log(RETRO_LOG_ERROR, "save state: %zu bytes, the frontend buffer has %zu", core->stateBuffer.size(), size);
        return false;
    }
    std::memcpy(data, core->stateBuffer.data(), core->stateBuffer.size());
    std::memset(static_cast<std::uint8_t*>(data) + core->stateBuffer.size(), 0, size - core->stateBuffer.size());
    return true;
}
RETRO_API bool retro_unserialize(const void* data, size_t size) {
    if (!core || !core->session || !data)
        return false;
    const gromada::game::SaveStateExtras extras{&core->input, &core->audioDebt};
    if (!core->saveStates.load(*core->session, {static_cast<const std::uint8_t*>(data), size}, extras)) {
        log(RETRO_LOG_WARN, "load state: %s", core->saveStates.error().c_str());
        return false;
    }
    return true;
}
RETRO_API void* retro_get_memory_data(unsigned) { return nullptr; }
RETRO_API size_t retro_get_memory_size(unsigned) { return 0; }
RETRO_API void retro_cheat_reset() {}
RETRO_API void retro_cheat_set(unsigned, bool, const char*) {}
