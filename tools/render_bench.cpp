// Rendering pipeline benchmark and screenshots (issue #20, docs/rendering.md): runs a level of
// the original data in a game Session (GameEngine, no audio) with scripted input, at a given
// internal resolution, zoom, aspect, frame rate and smooth motion; prints the time of a frame
// (Session::runFrame, picture included) with and without drawing, so the cost of the picture
// is the difference; optionally writes PPM screenshots. Times are the CPU time of the thread
// (CLOCK_THREAD_CPUTIME_ID): other processes on the machine do not count. With --threads > 1
// the other threads' work is not in it: use --wall (elapsed time) to see the speed-up.
//
// usage: render_bench --data DIR [--level 19] [--frames 600] [--warmup 300] [--fps 120]
//                     [--aspect 4:3|16:9] [--scale 1..4] [--zoom 1] [--smooth 1] [--repeat 1]
//                     [--shots f1,f2,...] [--out prefix] [--no-baseline] [--threads N] [--wall]
#include "core/std.hpp"
#include "libgromada/libgromada_all.hpp"
#include "game/engine.hpp"
#include "game/session.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <set>
#include <time.h>

using namespace gromada;
using namespace gromada::game;

namespace {

bool g_wall = false;
double cpuMs() {
    timespec t{};
    clock_gettime(g_wall ? CLOCK_MONOTONIC : CLOCK_THREAD_CPUTIME_ID, &t);
    return static_cast<double>(t.tv_sec) * 1e3 + static_cast<double>(t.tv_nsec) * 1e-6;
}

struct Options {
    std::filesystem::path data;
    int level = 19, frames = 600, warmup = 300, fps = 120, scale = 1, repeat = 1, threads = 0;
    bool wide = false, smooth = true, baseline = true;
    float zoom = 1;
    std::set<int> shots;
    std::string out = "render_bench";
};

input::PcState inputAt(int f, int fps) {
    const int t = f * 60 / fps;   // the same input in time at 60 and 120 fps
    input::PcState pc;
    pc.cursor = {static_cast<float>(320 + 250 * std::sin(t * 0.021)), static_cast<float>(240 + 180 * std::cos(t * 0.013))};
    const auto button = [](input::ButtonState& b, bool down, bool before) { b = {down, down && !before, !down && before}; };
    const auto at = [&](auto&& pred, int k) { return pred(k * 60 / fps); };
    const auto leftAt = [](int k) { return k % 120 < 3; };
    const auto rightAt = [](int k) { return k % 200 > 120; };
    const auto enterAt = [](int k) { return k < 400 && k % 40 < 2; };   // briefing, dialogs
    button(pc.left, at(leftAt, f), at(leftAt, f - 1));
    button(pc.right, at(rightAt, f), at(rightAt, f - 1));
    button(pc.keys[static_cast<std::size_t>(input::Key::Enter)], at(enterAt, f), at(enterAt, f - 1));
    return pc;
}

struct Result {
    std::vector<double> ms;
    std::size_t sprites = 0;
};

Result run(const GameResources& res, const Options& o, bool render) {
    SessionConfig cfg;
    cfg.gameDir = o.data;
    cfg.outputWidth = o.wide ? 854 : 640;
    cfg.outputHeight = 480;
    cfg.render = render;
    cfg.renderScale = o.scale;
    cfg.smoothMotion = o.smooth;
    cfg.renderThreads = o.threads;
    Session s{res.vids(), res.sounds(), std::make_unique<GameEngine>(res), nullptr, nullptr, cfg};
    s.setZoom(o.zoom);
    s.start(std::format("maps\\levels\\{:02}.map", o.level));
    Result r;
    const double dt = 1.0 / o.fps;
    for (int f = 0; f < o.warmup + o.frames; ++f) {
        const double t0 = cpuMs();
        s.runFrame(inputAt(f, o.fps), dt);
        const double ms = cpuMs() - t0;
        if (f >= o.warmup) r.ms.push_back(ms);
        if (render && o.shots.count(f)) {
            const std::string path = std::format("{}_{}.ppm", o.out, f);
            FILE* out = std::fopen(path.c_str(), "wb");
            std::fprintf(out, "P6\n%d %d\n255\n", s.width(), s.height());
            for (int i = 0; i < s.width() * s.height(); ++i) {
                const std::uint32_t p = s.xrgb()[i];
                const std::uint8_t rgb[3] = {static_cast<std::uint8_t>(p >> 16), static_cast<std::uint8_t>(p >> 8), static_cast<std::uint8_t>(p)};
                std::fwrite(rgb, 1, 3, out);
            }
            std::fclose(out);
            std::fprintf(stderr, "wrote %s (zoom %.2f, map %s)\n", path.c_str(), s.effectiveZoom(), s.currentMap().c_str());
        }
    }
    r.sprites = dynamic_cast<GameEngine&>(s.engine()).world().liveCount();
    return r;
}

struct Stats { double mean, median, p95, max; };
Stats stats(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (const double x : v) sum += x;
    return {sum / static_cast<double>(v.size()), v[v.size() / 2], v[std::min(v.size() - 1, v.size() * 95 / 100)], v.back()};
}

} // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc;) {
        const std::string k = argv[i];
        if (k == "--no-baseline") { o.baseline = false; ++i; continue; }
        if (k == "--wall") { g_wall = true; ++i; continue; }
        if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", k.c_str()); return 1; }
        const std::string v = argv[i + 1];
        i += 2;
        if (k == "--data") o.data = v;
        else if (k == "--level") o.level = std::atoi(v.c_str());
        else if (k == "--frames") o.frames = std::atoi(v.c_str());
        else if (k == "--warmup") o.warmup = std::atoi(v.c_str());
        else if (k == "--fps") o.fps = std::atoi(v.c_str());
        else if (k == "--aspect") o.wide = v == "16:9";
        else if (k == "--scale") o.scale = std::atoi(v.c_str());
        else if (k == "--zoom") o.zoom = static_cast<float>(std::atof(v.c_str()));
        else if (k == "--smooth") o.smooth = v != "0";
        else if (k == "--repeat") o.repeat = std::max(1, std::atoi(v.c_str()));
        else if (k == "--threads") o.threads = std::atoi(v.c_str());
        else if (k == "--out") o.out = v;
        else if (k == "--shots") {
            for (std::size_t p = 0; p < v.size();) {
                const std::size_t q = std::min(v.find(',', p), v.size());
                o.shots.insert(std::atoi(v.substr(p, q - p).c_str()));
                p = q + 1;
            }
        } else { std::fprintf(stderr, "unknown option %s\n", k.c_str()); return 1; }
    }
    if (o.data.empty()) { std::fprintf(stderr, "usage: render_bench --data DIR [options]\n"); return 1; }
    const GameResources res{o.data / "fw.res"};
    for (int rep = 0; rep < o.repeat; ++rep) {
        const Result drawn = run(res, o, true);
        const Stats a = stats(drawn.ms);
        std::printf("level %02d %s %d fps scale %d zoom %.2f smooth %d threads %d %s: %zu sprites, %zux%zu: frame mean %.3f ms median %.3f p95 %.3f max %.3f",
                    o.level, o.wide ? "16:9" : "4:3", o.fps, o.scale, o.zoom, o.smooth, o.threads, g_wall ? "wall" : "cpu", drawn.sprites,
                    static_cast<std::size_t>((o.wide ? 854 : 640) * o.scale), static_cast<std::size_t>(480 * o.scale), a.mean, a.median,
                    a.p95, a.max);
        if (o.baseline) {
            const Stats b = stats(run(res, o, false).ms);
            std::printf(" | without picture mean %.3f ms | picture %.3f ms", b.mean, a.mean - b.mean);
        }
        std::printf("\n");
        std::fflush(stdout);
    }
    return 0;
}
