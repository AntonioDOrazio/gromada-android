// Minimal headless libretro frontend for testing the core without RetroArch.
// usage: harness <core.so> <content> <frames> <out_prefix> [key=value ...]
//   options:  gromada_fps=120 gromada_aspect=16:9 gromada_level=5
//   input:    hold=right|left|up|down|y   (held for the whole run)
//             press_r_at=<frame>          (pulse R: next weapon; the viewer's next level is now R2)
//             shots=<f1,f2,...>           (frames to dump as PPM)
//   timed input (repeatable; "@from-to" or "@frame", default = whole run):
//             btn=<a|b|x|y|l|r|l2|r2|l3|r3|start|select|up|down|left|right>@10-20
//             lstick=<x>,<y>@...  rstick=<x>,<y>@...   (-1..1, y down)
//             l2a=<p>@...  r2a=<p>@...                 (analog trigger pressure 0..1)
//             touch=<px>,<py>[;<px>,<py>]@...         (fingers, in game-image pixels)
//             mouse=<dx>,<dy>@...  mouse_btn=<left|right>@...
//   savedir=<dir>   libretro save directory (default <out_prefix>_save; "none" = no save dir)
//   save states (issue #14): frames are "logical" frames, the ones the timed input refers to.
//             savestate=<frame>       save (retro_serialize) before running that frame
//             loadstate=<frame>       when that frame is reached the first time, load the state
//                                     and go back to the saved frame (its input is replayed)
//             statefile=<path>        write the saved state there (savestate) / load it from
//                                     there (loadstate without savestate)
//             runahead=<n>            every frame: run it, save, run n more frames, load (the
//                                     single-instance run-ahead of RetroArch)
//             hashes=<path>           one line per real frame: "<frame> <video fnv> <audio fnv>",
//                                     plus "ahead <frame> <video fnv>" for run-ahead frames
//             statefuzz=<n>           after the run: n corrupted/truncated copies of a fresh state
//                                     must be rejected by retro_unserialize without crashing
//   Exit() from the game (RETRO_ENVIRONMENT_SHUTDOWN) stops the run.
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
static std::string outPrefix, hold, saveDir;
static bool shutdownRequested = false;
static int frame = 0, pressRAt = -1, lastW = 0, lastH = 0;
static double fps = 0;
static size_t audioFrames = 0;
static std::vector<int16_t> audioCapture;
static std::set<int> pressA;
static unsigned imageW = 640, imageH = 480;
static uint64_t videoHash = 0, audioHash = 0;
static bool hashing = false;
static uint64_t fnv(const void* p, size_t n, uint64_t h = 0xcbf29ce484222325ull) {
    const auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}

struct Timed {
    std::string kind;        // btn, lstick, rstick, touch, mouse, mouse_btn
    unsigned id = 0;         // button id or mouse button id
    std::vector<std::pair<float, float>> xy;
    int from = 0, to = 1 << 30;
    bool active() const { return frame >= from && frame <= to; }
};
static std::vector<Timed> timed;

static bool parseTimed(const std::string& k, const std::string& v) {
    static const std::map<std::string, unsigned> buttons = {
        {"b", RETRO_DEVICE_ID_JOYPAD_B}, {"y", RETRO_DEVICE_ID_JOYPAD_Y}, {"select", RETRO_DEVICE_ID_JOYPAD_SELECT},
        {"start", RETRO_DEVICE_ID_JOYPAD_START}, {"up", RETRO_DEVICE_ID_JOYPAD_UP}, {"down", RETRO_DEVICE_ID_JOYPAD_DOWN},
        {"left", RETRO_DEVICE_ID_JOYPAD_LEFT}, {"right", RETRO_DEVICE_ID_JOYPAD_RIGHT}, {"a", RETRO_DEVICE_ID_JOYPAD_A},
        {"x", RETRO_DEVICE_ID_JOYPAD_X}, {"l", RETRO_DEVICE_ID_JOYPAD_L}, {"r", RETRO_DEVICE_ID_JOYPAD_R},
        {"l2", RETRO_DEVICE_ID_JOYPAD_L2}, {"r2", RETRO_DEVICE_ID_JOYPAD_R2}, {"l3", RETRO_DEVICE_ID_JOYPAD_L3},
        {"r3", RETRO_DEVICE_ID_JOYPAD_R3}};
    if (k != "btn" && k != "lstick" && k != "rstick" && k != "touch" && k != "mouse" && k != "mouse_btn" && k != "l2a" &&
        k != "r2a")
        return false;
    Timed t;
    t.kind = k;
    const auto at = v.find('@');
    const std::string what = v.substr(0, at);
    if (at != std::string::npos) {
        const std::string range = v.substr(at + 1);
        const auto dash = range.find('-');
        t.from = std::atoi(range.c_str());
        t.to = dash == std::string::npos ? t.from : std::atoi(range.c_str() + dash + 1);
    }
    if (k == "btn") {
        auto it = buttons.find(what);
        if (it == buttons.end()) { std::fprintf(stderr, "unknown button %s\n", what.c_str()); std::exit(1); }
        t.id = it->second;
    } else if (k == "mouse_btn") {
        t.id = what == "right" ? RETRO_DEVICE_ID_MOUSE_RIGHT : RETRO_DEVICE_ID_MOUSE_LEFT;
    } else if (k == "l2a" || k == "r2a") {
        t.id = k == "l2a" ? RETRO_DEVICE_ID_JOYPAD_L2 : RETRO_DEVICE_ID_JOYPAD_R2;
        t.xy.emplace_back(std::atof(what.c_str()), 0.0f);
    } else {
        for (size_t p = 0; p < what.size();) {
            size_t q = what.find(';', p); if (q == std::string::npos) q = what.size();
            const std::string pair = what.substr(p, q - p);
            t.xy.emplace_back(std::atof(pair.c_str()), std::atof(pair.c_str() + pair.find(',') + 1));
            p = q + 1;
        }
    }
    timed.push_back(t);
    return true;
}

static void logcb(enum retro_log_level, const char* fmt, ...) {
    std::fprintf(stderr, "[f%d] ", frame);
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
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        if (saveDir == "none") return false;
        *static_cast<const char**>(data) = saveDir.c_str();
        return true;
    case RETRO_ENVIRONMENT_SHUTDOWN: shutdownRequested = true; std::fprintf(stderr, "[f%d] shutdown requested\n", frame); return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE: std::fprintf(stderr, "[msg] %s\n", static_cast<retro_message*>(data)->msg); return true;
    default: return false;
    }
}

static void video(const void* data, unsigned w, unsigned h, size_t pitch) {
    lastW = w; lastH = h; imageW = w; imageH = h;
    if (hashing && data)
        for (unsigned y = 0; y < h; ++y) videoHash = fnv(static_cast<const uint8_t*>(data) + y * pitch, w * 4, videoHash);
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

static bool muteCapture = false;
static size_t audio(const int16_t* d, size_t frames) {
    if (hashing) audioHash = fnv(d, frames * 4, audioHash);
    if (muteCapture) return frames;
    audioFrames += frames; audioCapture.insert(audioCapture.end(), d, d + frames * 2); return frames;
}
static void poll() {}
static int16_t timedInput(unsigned device, unsigned index, unsigned id) {
    auto clamp16 = [](float v) { return int16_t(v > 32767 ? 32767 : v < -32767 ? -32767 : v); };
    if (device == RETRO_DEVICE_ANALOG && index == RETRO_DEVICE_INDEX_ANALOG_BUTTON) {
        for (const Timed& t : timed)
            if ((t.kind == "l2a" || t.kind == "r2a") && t.id == id && t.active()) return clamp16(t.xy[0].first * 32767);
    } else if (device == RETRO_DEVICE_ANALOG) {
        const char* kind = index == RETRO_DEVICE_INDEX_ANALOG_LEFT ? "lstick" : "rstick";
        for (const Timed& t : timed)
            if (t.kind == kind && t.active())
                return clamp16((id == RETRO_DEVICE_ID_ANALOG_X ? t.xy[0].first : t.xy[0].second) * 32767);
    } else if (device == RETRO_DEVICE_POINTER) {
        for (const Timed& t : timed)
            if (t.kind == "touch" && t.active()) {
                if (index >= t.xy.size()) return 0;
                if (id == RETRO_DEVICE_ID_POINTER_PRESSED) return 1;
                if (id == RETRO_DEVICE_ID_POINTER_X) return clamp16(t.xy[index].first / imageW * 2 * 0x7fff - 0x7fff);
                if (id == RETRO_DEVICE_ID_POINTER_Y) return clamp16(t.xy[index].second / imageH * 2 * 0x7fff - 0x7fff);
            }
    } else if (device == RETRO_DEVICE_MOUSE) {
        for (const Timed& t : timed) {
            if (!t.active()) continue;
            if (t.kind == "mouse" && id == RETRO_DEVICE_ID_MOUSE_X) return clamp16(t.xy[0].first);
            if (t.kind == "mouse" && id == RETRO_DEVICE_ID_MOUSE_Y) return clamp16(t.xy[0].second);
            if (t.kind == "mouse_btn" && t.id == id) return 1;
        }
    } else if (device == RETRO_DEVICE_JOYPAD) {
        for (const Timed& t : timed)
            if (t.kind == "btn" && t.id == id && t.active()) return 1;
    }
    return 0;
}

static int16_t input(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port != 0) return 0;
    if (int16_t v = timedInput(device, index, id)) return v;
    if (device != RETRO_DEVICE_JOYPAD) return 0;
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

static int saveAt = -1, loadAt = -1, runAhead = 0, stateFuzz = 0;
static std::string stateFile, hashFile;

int main(int argc, char** argv) {
    if (argc < 5) { std::fprintf(stderr, "usage: harness core.so content frames out_prefix [k=v...]\n"); return 1; }
    void* lib = dlopen(argv[1], RTLD_NOW);
    if (!lib) { std::fprintf(stderr, "%s\n", dlerror()); return 1; }
    const int frames = std::atoi(argv[3]);
    outPrefix = argv[4];
    for (int i = 5; i < argc; ++i) {
        std::string kv = argv[i]; auto eq = kv.find('=');
        std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
        if (parseTimed(k, v)) continue;
        if (k == "hold") hold = v;
        else if (k == "press_r_at") pressRAt = std::atoi(v.c_str());
        else if (k == "press_a_at") { for (char* t = std::strtok(v.data(), ","); t; t = std::strtok(nullptr, ",")) pressA.insert(std::atoi(t)); }
        else if (k == "savedir") saveDir = v;
        else if (k == "savestate") saveAt = std::atoi(v.c_str());
        else if (k == "loadstate") loadAt = std::atoi(v.c_str());
        else if (k == "statefile") stateFile = v;
        else if (k == "runahead") runAhead = std::atoi(v.c_str());
        else if (k == "hashes") hashFile = v;
        else if (k == "statefuzz") stateFuzz = std::atoi(v.c_str());
        else if (k == "shots") { for (char* t = std::strtok(v.data(), ","); t; t = std::strtok(nullptr, ",")) shots.insert(std::atoi(t)); }
        else opts[k] = v;
    }

    if (saveDir.empty()) saveDir = outPrefix + "_save";
    SYM(retro_set_environment) SYM(retro_set_video_refresh) SYM(retro_set_audio_sample_batch)
    SYM(retro_set_input_poll) SYM(retro_set_input_state) SYM(retro_init) SYM(retro_load_game)
    SYM(retro_get_system_av_info) SYM(retro_run) SYM(retro_unload_game) SYM(retro_deinit)
    SYM(retro_serialize_size) SYM(retro_serialize) SYM(retro_unserialize)

    retro_set_environment(env); retro_set_video_refresh(video); retro_set_audio_sample_batch(audio);
    retro_set_input_poll(poll); retro_set_input_state(input);
    retro_init();
    retro_game_info game{argv[2], nullptr, 0, nullptr};
    if (!retro_load_game(&game)) { std::fprintf(stderr, "load_game failed\n"); return 2; }
    retro_system_av_info av{}; retro_get_system_av_info(&av); fps = av.timing.fps;
    imageW = av.geometry.base_width; imageH = av.geometry.base_height;
    std::fprintf(stderr, "av: %ux%u aspect=%.3f fps=%.0f rate=%.0f\n", av.geometry.base_width, av.geometry.base_height,
                 av.geometry.aspect_ratio, av.timing.fps, av.timing.sample_rate);

    const size_t stateSize = retro_serialize_size();
    std::vector<uint8_t> state, scratch(stateSize);
    FILE* hashes = hashFile.empty() ? nullptr : std::fopen(hashFile.c_str(), "w");
    hashing = hashes != nullptr;
    double serializeMs = 0, unserializeMs = 0, maxSerializeMs = 0, maxUnserializeMs = 0;
    long serializeCount = 0, unserializeCount = 0;
    size_t maxUsed = 0;
    auto timed = [](auto&& fn) {
        auto a = std::chrono::steady_clock::now();
        const bool ok = fn();
        return std::pair{ok, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count()};
    };
    auto doSave = [&](std::vector<uint8_t>& out) {
        out.assign(stateSize, 0xAA);
        auto [ok, ms] = timed([&] { return retro_serialize(out.data(), out.size()); });
        serializeMs += ms; maxSerializeMs = std::max(maxSerializeMs, ms); ++serializeCount;
        size_t used = 0;
        if (ok && out.size() >= 24) { uint64_t payload; std::memcpy(&payload, out.data() + 16, 8); used = size_t(payload) + 64; }
        maxUsed = std::max(maxUsed, used);
        return ok;
    };
    auto doLoad = [&](const std::vector<uint8_t>& in) {
        auto [ok, ms] = timed([&] { return retro_unserialize(in.data(), in.size()); });
        unserializeMs += ms; maxUnserializeMs = std::max(maxUnserializeMs, ms); ++unserializeCount;
        return ok;
    };
    bool saved = false, loaded = false;
    int savedFrame = -1, failures = 0;
    if (saveAt < 0 && loadAt >= 0 && !stateFile.empty()) {   // load a state written by an earlier run
        FILE* f = std::fopen(stateFile.c_str(), "rb");
        if (!f) { std::fprintf(stderr, "cannot read %s\n", stateFile.c_str()); return 3; }
        state.assign(stateSize, 0);
        state.resize(std::fread(state.data(), 1, state.size(), f));
        std::fclose(f);
        saved = true;
        if (state.size() >= 56) { uint64_t fr; std::memcpy(&fr, state.data() + 48, 8); savedFrame = int(fr); } // header: session frames
    }

    auto t0 = std::chrono::steady_clock::now();
    for (int run = 0; run < frames && !shutdownRequested; ++run) {
        if (frame == saveAt && !saved) {
            if (!doSave(state)) { std::fprintf(stderr, "[f%d] retro_serialize failed\n", frame); ++failures; }
            else {
                saved = true;
                savedFrame = frame;
                std::fprintf(stderr, "[f%d] state saved (%zu-byte buffer)\n", frame, state.size());
                if (!stateFile.empty()) { FILE* f = std::fopen(stateFile.c_str(), "wb"); std::fwrite(state.data(), 1, state.size(), f); std::fclose(f); }
            }
        }
        if (frame == loadAt && saved && !loaded) {
            loaded = true;
            if (!doLoad(state)) { std::fprintf(stderr, "[f%d] retro_unserialize failed\n", frame); ++failures; }
            else {
                std::fprintf(stderr, "[f%d] state loaded, back to frame %d\n", frame, savedFrame);
                if (savedFrame >= 0) frame = savedFrame;
            }
        }
        videoHash = audioHash = 0xcbf29ce484222325ull;
        retro_run();
        if (hashes) std::fprintf(hashes, "%d %016llx %016llx\n", frame, (unsigned long long)videoHash, (unsigned long long)audioHash);
        if (runAhead > 0 && !shutdownRequested) {
            if (!doSave(scratch)) { std::fprintf(stderr, "[f%d] run-ahead: retro_serialize failed\n", frame); ++failures; }
            else {
                const int real = frame;
                muteCapture = true;
                for (int k = 1; k <= runAhead; ++k) {
                    ++frame;
                    videoHash = audioHash = 0xcbf29ce484222325ull;
                    retro_run();
                }
                if (hashes) std::fprintf(hashes, "ahead %d %016llx\n", frame, (unsigned long long)videoHash);
                muteCapture = false;
                frame = real;
                if (!doLoad(scratch)) { std::fprintf(stderr, "[f%d] run-ahead: retro_unserialize failed\n", frame); ++failures; }
            }
        }
        ++frame;
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (hashes) std::fclose(hashes);
    if (serializeCount)
        std::fprintf(stderr, "savestates: size %zu (max used %zu), serialize %ld x avg %.3f ms max %.3f ms, "
                     "unserialize %ld x avg %.3f ms max %.3f ms\n", stateSize, maxUsed, serializeCount,
                     serializeMs / serializeCount, maxSerializeMs, unserializeCount,
                     unserializeCount ? unserializeMs / unserializeCount : 0.0, maxUnserializeMs);
    if (stateFuzz > 0 && !shutdownRequested) {
        // Corrupted states: truncations, bit flips, garbage. All must be refused, and the game
        // must go on exactly as before (the state is checked before anything changes).
        std::vector<uint8_t> good, bad, before, after;
        if (!doSave(good)) { std::fprintf(stderr, "statefuzz: cannot save\n"); return 4; }
        doSave(before);
        uint64_t x = 0x9E3779B97F4A7C15ull;
        auto rnd = [&] { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
        uint64_t used; std::memcpy(&used, good.data() + 16, 8); used += 64;
        int accepted = 0;
        for (int i = 0; i < stateFuzz; ++i) {
            bad = good;
            switch (i % 4) {
            case 0: bad.resize(size_t(rnd() % used)); break;                         // truncated
            case 1: for (int k = 0; k < 1 + int(rnd() % 8); ++k) bad[rnd() % used] ^= uint8_t(1u << (rnd() % 8)); break;
            case 2: for (size_t k = 64 + rnd() % (used - 64), e = std::min(used, k + 64); k < e; ++k) bad[k] = uint8_t(rnd()); break;
            case 3: bad[rnd() % 64] ^= uint8_t(1 + rnd() % 255); break;                // header
            }
            if (bad == good) continue;
            if (retro_unserialize(bad.data(), bad.size())) ++accepted;
        }
        doSave(after);
        const bool same = before == after;
        std::fprintf(stderr, "statefuzz: %d corrupted states, %d accepted, state %s\n", stateFuzz, accepted,
                     same ? "unchanged" : "CHANGED");
        if (accepted || !same) ++failures;
    }
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
    return failures ? 5 : 0;
}
