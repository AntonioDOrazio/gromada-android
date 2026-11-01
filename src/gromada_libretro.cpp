// Gromada libretro core — milestones M1 ("viewer") + M2 (audio):
// loads the original game data (fw.res + maps), renders a level with the software
// renderer and scrolls it with the gamepad. 60/120 fps and 4:3/16:9 are core options.
// Audio: sound effects from fw.res and the CD soundtrack (music/trackNN.ogg).
//
// This is the ONLY translation unit of the core (libgromada is header-implemented).

#include "core/std.hpp"
#include "engine/audio.hpp"
#include "engine/level.hpp"
#include "engine/renderer.hpp"
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
};

std::unique_ptr<Core> core;

// --- options ---------------------------------------------------------------

std::string levelValues() {
    std::string values = "Livello (anteprima); ";
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
        {"gromada_level", levels.c_str()},
        {"gromada_music_volume", "Volume musica; 80|90|100|0|10|20|30|40|50|60|70"},
        {"gromada_sfx_volume", "Volume effetti; 100|0|10|20|30|40|50|60|70|80|90"},
        {"gromada_movie", "Riproduci filmato (Start/A/B per saltare); nessuno|0|1|2|3"},
        {nullptr, nullptr},
    };
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
    return o;
}

retro_system_av_info makeAvInfo(const Options& o) {
    retro_system_av_info info{};
    const int width = o.widescreen ? gromada::Renderer::MaxWidth : 640;
    info.geometry.base_width = width;
    info.geometry.base_height = gromada::Renderer::Height;
    info.geometry.max_width = gromada::Renderer::MaxWidth;
    info.geometry.max_height = gromada::Renderer::Height;
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

void checkMovieOption(); // movies, see below

void applyOptions(bool initial) {
    const Options next = readOptions();
    const Options prev = core->options;
    core->options = next;
    core->renderer.setWidth(next.widescreen ? gromada::Renderer::MaxWidth : 640);
    core->mixer.setMusicVolume(next.musicVolume / 100.0f);
    core->mixer.setSfxVolume(next.sfxVolume / 100.0f);
    checkMovieOption();
    if (initial)
        return;

    auto av = makeAvInfo(next);
    if (next.fps != prev.fps)
        env_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av);
    else if (next.widescreen != prev.widescreen)
        env_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &av.geometry);

    if (next.startLevel != prev.startLevel)
        loadLevel(next.startLevel);
}

// --- per-frame ---------------------------------------------------------------

float axis(unsigned id) {
    const int v = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, id);
    const float f = v / 32767.0f;
    return std::abs(f) < 0.15f ? 0.0f : f; // deadzone
}

bool button(unsigned id) {
    return input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, id) != 0;
}

void updateCamera(float dt) {
    float dx = axis(RETRO_DEVICE_ID_ANALOG_X);
    float dy = axis(RETRO_DEVICE_ID_ANALOG_Y);
    if (button(RETRO_DEVICE_ID_JOYPAD_LEFT))  dx = -1;
    if (button(RETRO_DEVICE_ID_JOYPAD_RIGHT)) dx = 1;
    if (button(RETRO_DEVICE_ID_JOYPAD_UP))    dy = -1;
    if (button(RETRO_DEVICE_ID_JOYPAD_DOWN))  dy = 1;

    const float speed = button(RETRO_DEVICE_ID_JOYPAD_Y) ? 1200.0f : 400.0f; // px/s
    core->camX += dx * speed * dt;
    core->camY += dy * speed * dt;

    const auto& h = core->level->header();
    const float maxX = std::max(0.0f, static_cast<float>(h.width) - core->renderer.width());
    const float maxY = std::max(0.0f, static_cast<float>(h.height) - core->renderer.height());
    core->camX = std::clamp(core->camX, 0.0f, maxX);
    core->camY = std::clamp(core->camY, 0.0f, maxY);
}

void handleLevelSwitch() {
    const bool l = button(RETRO_DEVICE_ID_JOYPAD_L);
    const bool r = button(RETRO_DEVICE_ID_JOYPAD_R);
    if (l && !core->prevL) loadLevel(core->levelIndex == 1 ? LevelCount : core->levelIndex - 1);
    if (r && !core->prevR) loadLevel(core->levelIndex == LevelCount ? 1 : core->levelIndex + 1);
    core->prevL = l;
    core->prevR = r;
}

// Sound test (viewer only, until gameplay triggers effects): A = next effect, B = previous.
void handleSoundTest() {
    const bool a = button(RETRO_DEVICE_ID_JOYPAD_A);
    const bool b = button(RETRO_DEVICE_ID_JOYPAD_B);
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
    info->library_version = "0.2.0-m2";
    info->valid_extensions = "exe|res";
    info->need_fullpath = true;
    info->block_extract = true;
}

RETRO_API void retro_get_system_av_info(retro_system_av_info* info) {
    *info = makeAvInfo(core ? core->options : readOptions());
}

RETRO_API void retro_init() { core = std::make_unique<Core>(); }
RETRO_API void retro_deinit() { stopMovie(); core.reset(); }
RETRO_API void retro_reset() { if (core && core->resources) loadLevel(core->levelIndex); }

RETRO_API bool retro_load_game(const retro_game_info* game) {
    if (!game || !game->path)
        return false;

    auto fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
        log(RETRO_LOG_ERROR, "XRGB8888 not supported by frontend");
        return false;
    }

    static const retro_input_descriptor desc[] = {
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Scorri a sinistra"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Scorri a destra"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Scorri in alto"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Scorri in basso"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Scorrimento veloce"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Prova effetto successivo"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Prova effetto precedente"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "Livello precedente"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "Livello successivo"},
        {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Scorri (orizzontale)"},
        {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Scorri (verticale)"},
        {0, 0, 0, 0, nullptr},
    };
    env_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, const_cast<retro_input_descriptor*>(desc));

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
    applyOptions(true);
    return loadLevel(core->options.startLevel);
}

RETRO_API void retro_unload_game() {
    stopMovie();
    movieState.option.clear(); // replay the chosen movie at the next load
    core->level.reset();
    core->resources.reset();
}

RETRO_API void retro_run() {
    bool updated = false;
    if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
        applyOptions(false);

    input_poll_cb();
    if (runMovie())
        return;
    handleLevelSwitch();
    handleSoundTest();

    const float dt = 1.0f / core->options.fps;
    updateCamera(dt);
    core->level->animate(dt);

    auto& r = core->renderer;
    r.draw(*core->level, static_cast<int>(core->camX), static_cast<int>(core->camY));
    video_cb(r.xrgb(), r.width(), r.height(), r.pitch());
    pushAudio();
}

RETRO_API unsigned retro_get_region() { return RETRO_REGION_NTSC; }
RETRO_API bool retro_load_game_special(unsigned, const retro_game_info*, size_t) { return false; }

// Save states arrive with the gameplay milestone.
RETRO_API size_t retro_serialize_size() { return 0; }
RETRO_API bool retro_serialize(void*, size_t) { return false; }
RETRO_API bool retro_unserialize(const void*, size_t) { return false; }
RETRO_API void* retro_get_memory_data(unsigned) { return nullptr; }
RETRO_API size_t retro_get_memory_size(unsigned) { return 0; }
RETRO_API void retro_cheat_reset() {}
RETRO_API void retro_cheat_set(unsigned, bool, const char*) {}
