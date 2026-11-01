// Gromada libretro core — milestone M1 ("viewer"):
// loads the original game data (fw.res + maps), renders a level with the software
// renderer and scrolls it with the gamepad. 60/120 fps and 4:3/16:9 are core options.
//
// This is the ONLY translation unit of the core (libgromada is header-implemented).

#include "core/std.hpp"
#include "engine/level.hpp"
#include "engine/renderer.hpp"
#include "libretro/libretro.h"

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
};

struct Core {
    std::unique_ptr<GameResources> resources;
    std::unique_ptr<gromada::Level> level;
    gromada::Renderer renderer;
    Options options;
    int levelIndex = 1;
    float camX = 0, camY = 0;
    bool prevL = false, prevR = false;
    double audioDebt = 0;
    std::vector<std::int16_t> silence;
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
        notify(std::format("Livello {:02}", index));
        return true;
    } catch (const std::exception& e) {
        log(RETRO_LOG_ERROR, "cannot load %s: %s", path.string().c_str(), e.what());
        return false;
    }
}

void applyOptions(bool initial) {
    const Options next = readOptions();
    const Options prev = core->options;
    core->options = next;
    core->renderer.setWidth(next.widescreen ? gromada::Renderer::MaxWidth : 640);
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

void pushSilence() {
    // 44100 / 120 is not an integer: carry the fractional sample over to the next frame.
    core->audioDebt += static_cast<double>(AudioRate) / core->options.fps;
    const auto frames = static_cast<std::size_t>(core->audioDebt);
    core->audioDebt -= frames;
    core->silence.assign(frames * 2, 0);
    if (frames)
        audio_batch_cb(core->silence.data(), frames);
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
    info->library_version = "0.1.0-m1";
    info->valid_extensions = "exe|res";
    info->need_fullpath = true;
    info->block_extract = true;
}

RETRO_API void retro_get_system_av_info(retro_system_av_info* info) {
    *info = makeAvInfo(core ? core->options : readOptions());
}

RETRO_API void retro_init() { core = std::make_unique<Core>(); }
RETRO_API void retro_deinit() { core.reset(); }
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

    applyOptions(true);
    return loadLevel(core->options.startLevel);
}

RETRO_API void retro_unload_game() {
    core->level.reset();
    core->resources.reset();
}

RETRO_API void retro_run() {
    bool updated = false;
    if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
        applyOptions(false);

    input_poll_cb();
    handleLevelSwitch();

    const float dt = 1.0f / core->options.fps;
    updateCamera(dt);
    core->level->animate(dt);

    auto& r = core->renderer;
    r.draw(*core->level, static_cast<int>(core->camX), static_cast<int>(core->camY));
    video_cb(r.xrgb(), r.width(), r.height(), r.pitch());
    pushSilence();
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
