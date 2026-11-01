// Minimal headless libretro frontend for testing the core without RetroArch.
// usage: harness <core.so> <content> <frames> <out_prefix> [key=value ...]
//   options:  gromada_fps=120 gromada_aspect=16:9 gromada_level=5
//   input:    hold=right|left|up|down|y   (held for the whole run)
//             press_r_at=<frame>          (pulse R / next level)
//             shots=<f1,f2,...>           (frames to dump as PPM)
#include <dlfcn.h>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "../src/libretro/libretro.h"

static std::map<std::string, std::string> opts;
static std::set<int> shots;
static std::string outPrefix, hold;
static int frame = 0, pressRAt = -1, lastW = 0, lastH = 0;
static double fps = 0;
static size_t audioFrames = 0;
static std::vector<int16_t> audioCapture;
static std::set<int> pressA;

static void logcb(enum retro_log_level, const char* fmt, ...) {
    va_list a; va_start(a, fmt); std::vfprintf(stderr, fmt, a); va_end(a);
}

static bool env(unsigned cmd, void* data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: static_cast<retro_log_callback*>(data)->log = logcb; return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: return *static_cast<retro_pixel_format*>(data) == RETRO_PIXEL_FORMAT_XRGB8888;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        auto* v = static_cast<retro_variable*>(data);
        auto it = opts.find(v->key);
        v->value = it == opts.end() ? nullptr : it->second.c_str();
        return it != opts.end();
    }
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_GEOMETRY: return true;
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: fps = static_cast<retro_system_av_info*>(data)->timing.fps; return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *static_cast<bool*>(data) = false; return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE: std::fprintf(stderr, "[msg] %s\n", static_cast<retro_message*>(data)->msg); return true;
    default: return false;
    }
}

static void video(const void* data, unsigned w, unsigned h, size_t pitch) {
    lastW = w; lastH = h;
    if (!data || !shots.count(frame)) return;
    std::string path = outPrefix + "_" + std::to_string(frame) + ".ppm";
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (unsigned y = 0; y < h; ++y) {
        auto* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(data) + y * pitch);
        for (unsigned x = 0; x < w; ++x) {
            uint8_t rgb[3] = {uint8_t(row[x] >> 16), uint8_t(row[x] >> 8), uint8_t(row[x])};
            std::fwrite(rgb, 1, 3, f);
        }
    }
    std::fclose(f);
    std::fprintf(stderr, "wrote %s\n", path.c_str());
}

static size_t audio(const int16_t* d, size_t frames) { audioFrames += frames; audioCapture.insert(audioCapture.end(), d, d + frames * 2); return frames; }
static void poll() {}
static int16_t input(unsigned port, unsigned device, unsigned, unsigned id) {
    if (port != 0 || device != RETRO_DEVICE_JOYPAD) return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_R && frame == pressRAt) return 1;
    if (id == RETRO_DEVICE_ID_JOYPAD_A && pressA.count(frame)) return 1;
    static const std::map<std::string, unsigned> names = {
        {"right", RETRO_DEVICE_ID_JOYPAD_RIGHT}, {"left", RETRO_DEVICE_ID_JOYPAD_LEFT},
        {"up", RETRO_DEVICE_ID_JOYPAD_UP}, {"down", RETRO_DEVICE_ID_JOYPAD_DOWN}, {"y", RETRO_DEVICE_ID_JOYPAD_Y}};
    for (size_t p = 0; p < hold.size();) {
        size_t q = hold.find('+', p); if (q == std::string::npos) q = hold.size();
        auto it = names.find(hold.substr(p, q - p));
        if (it != names.end() && it->second == id) return 1;
        p = q + 1;
    }
    return 0;
}

#define SYM(name) auto name = reinterpret_cast<decltype(&::name)>(dlsym(lib, #name)); if (!name) { std::fprintf(stderr, "missing %s\n", #name); return 1; }

int main(int argc, char** argv) {
    if (argc < 5) { std::fprintf(stderr, "usage: harness core.so content frames out_prefix [k=v...]\n"); return 1; }
    void* lib = dlopen(argv[1], RTLD_NOW);
    if (!lib) { std::fprintf(stderr, "%s\n", dlerror()); return 1; }
    const int frames = std::atoi(argv[3]);
    outPrefix = argv[4];
    for (int i = 5; i < argc; ++i) {
        std::string kv = argv[i]; auto eq = kv.find('=');
        std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
        if (k == "hold") hold = v;
        else if (k == "press_r_at") pressRAt = std::atoi(v.c_str());
        else if (k == "press_a_at") { for (char* t = std::strtok(v.data(), ","); t; t = std::strtok(nullptr, ",")) pressA.insert(std::atoi(t)); }
        else if (k == "shots") { for (char* t = std::strtok(v.data(), ","); t; t = std::strtok(nullptr, ",")) shots.insert(std::atoi(t)); }
        else opts[k] = v;
    }

    SYM(retro_set_environment) SYM(retro_set_video_refresh) SYM(retro_set_audio_sample_batch)
    SYM(retro_set_input_poll) SYM(retro_set_input_state) SYM(retro_init) SYM(retro_load_game)
    SYM(retro_get_system_av_info) SYM(retro_run) SYM(retro_unload_game) SYM(retro_deinit)

    retro_set_environment(env); retro_set_video_refresh(video); retro_set_audio_sample_batch(audio);
    retro_set_input_poll(poll); retro_set_input_state(input);
    retro_init();
    retro_game_info game{argv[2], nullptr, 0, nullptr};
    if (!retro_load_game(&game)) { std::fprintf(stderr, "load_game failed\n"); return 2; }
    retro_system_av_info av{}; retro_get_system_av_info(&av); fps = av.timing.fps;
    std::fprintf(stderr, "av: %ux%u aspect=%.3f fps=%.0f rate=%.0f\n", av.geometry.base_width, av.geometry.base_height,
                 av.geometry.aspect_ratio, av.timing.fps, av.timing.sample_rate);

    auto t0 = std::chrono::steady_clock::now();
    for (frame = 0; frame < frames; ++frame) retro_run();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "ran %d frames (%ux%u) in %.1f ms -> %.2f ms/frame; audio frames %zu (expected %.0f)\n",
                 frames, lastW, lastH, ms, ms / frames, audioFrames, frames * av.timing.sample_rate / fps);
    {   // dump captured audio as WAV
        std::string path = outPrefix + ".wav"; FILE* f = std::fopen(path.c_str(), "wb");
        uint32_t rate = uint32_t(av.timing.sample_rate), bytes = uint32_t(audioCapture.size() * 2), v32; uint16_t v16;
        std::fwrite("RIFF", 1, 4, f); v32 = 36 + bytes; std::fwrite(&v32, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
        v32 = 16; std::fwrite(&v32, 4, 1, f); v16 = 1; std::fwrite(&v16, 2, 1, f); v16 = 2; std::fwrite(&v16, 2, 1, f);
        std::fwrite(&rate, 4, 1, f); v32 = rate * 4; std::fwrite(&v32, 4, 1, f); v16 = 4; std::fwrite(&v16, 2, 1, f);
        v16 = 16; std::fwrite(&v16, 2, 1, f); std::fwrite("data", 1, 4, f); std::fwrite(&bytes, 4, 1, f);
        std::fwrite(audioCapture.data(), 2, audioCapture.size(), f); std::fclose(f);
    }
    retro_unload_game(); retro_deinit();
    return 0;
}
