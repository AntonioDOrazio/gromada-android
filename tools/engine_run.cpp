// Desktop driver of the simulation engine (issue #8): loads a level, places the player's
// Kassandra like the level scripts do (Create + SetFlagman + weapon/ammo of HEADER.INI's
// Tact), simulates N seconds with a fixed frame step and optional scripted input, writes
// screenshots (PPM) and a text trace.
//
//   engine_run --data DIR (--level NN | --map FILE) [--seconds S] [--step MS] [--seed N]
//              [--out DIR] [--shots T1,T2,...] [--trace MS] [--input EVENTS] [--no-player]
//              [--size WxH] [--quiet]
//
// EVENTS: ';'-separated "t:kind:dx,dy[:ms]" with t in ms of game time, coordinates relative
// to the flagman. kinds: move (left click), fire (right button held for ms), up/down/left/right
// (keys held for ms), jump (act_jump at the point).

#include "game/engine.hpp"

#include <chrono>
#include <cstdio>
#include <regex>

using namespace gromada;
using namespace gromada::game;

namespace {

struct InputEvent {
    int t = 0;
    std::string kind;
    int dx = 0, dy = 0;
    int duration = 0;
};

std::vector<InputEvent> parseEvents(const std::string& text) {
    std::vector<InputEvent> out;
    std::stringstream ss(text);
    std::string item;
    while (std::getline(ss, item, ';')) {
        if (item.empty()) continue;
        InputEvent e;
        std::stringstream is(item);
        std::string part;
        std::vector<std::string> parts;
        while (std::getline(is, part, ':')) parts.push_back(part);
        if (parts.size() < 2) continue;
        e.t = std::stoi(parts[0]);
        e.kind = parts[1];
        if (parts.size() > 2) {
            auto comma = parts[2].find(',');
            e.dx = std::stoi(parts[2].substr(0, comma));
            if (comma != std::string::npos) e.dy = std::stoi(parts[2].substr(comma + 1));
        }
        if (parts.size() > 3) e.duration = std::stoi(parts[3]);
        out.push_back(e);
    }
    return out;
}

void writePpm(const std::filesystem::path& path, const std::vector<RGBA8>& px, int w, int h) {
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << w << " " << h << "\n255\n";
    for (const auto& p : px) { f.put(static_cast<char>(p.r)); f.put(static_cast<char>(p.g)); f.put(static_cast<char>(p.b)); }
}

// Free place near (x, y) for the vid (spiral of AskPlace probes).
std::pair<int, int> freePlace(World& w, int nvid, int x, int y) {
    for (int r = 0; r < 800; r += 16)
        for (int k = 0; k < 16; ++k) {
            const int px = x + (fx::Sin[k * 16] * r >> 10), py = y + (fx::NegCos[k * 16] * r >> 10);
            if (px < 40 || py < 40 || px > w.mapWidth() - 40 || py > w.mapHeight() - 40) continue;
            if (!w.askPlaceVid(px, py, 0, nvid)) return {px, py};
            if (r == 0) break;
        }
    return {x, y};
}

// Where the level script puts the Kassandra: Create(kassandra,X,Y,..) or the first
// teleport of tp_x[]/tp_y[] (most levels start at tp_in = 0).
std::optional<std::pair<int, int>> scriptSpawn(const std::filesystem::path& ini) {
    std::ifstream f(ini, std::ios::binary);
    if (!f) return std::nullopt;
    const std::string text{std::istreambuf_iterator<char>(f), {}};
    std::smatch m;
    if (std::regex_search(text, m, std::regex(R"(Create\(kassandra,\s*(\d+)\s*,\s*(\d+))")))
        return std::pair{std::stoi(m[1]), std::stoi(m[2])};
    std::smatch mx, my;
    if (std::regex_search(text, mx, std::regex(R"(tp_x\[\d+\]\s*=\s*\{\s*(\d+))")) &&
        std::regex_search(text, my, std::regex(R"(tp_y\[\d+\]\s*=\s*\{\s*(\d+))")))
        return std::pair{std::stoi(mx[1]), std::stoi(my[1])};
    return std::nullopt;
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path data = std::getenv("GROMADA_DATA") ? std::getenv("GROMADA_DATA") : "";
    std::filesystem::path mapFile, outDir = "engine_out";
    int level = 0;
    double seconds = 10, step = 1000.0 / 60.0;
    std::uint32_t seed = 12345;
    std::vector<int> shots;
    int traceEvery = 1000;
    std::string events;
    bool player = true, quiet = false;
    int width = 640, height = 480;
    int spawnX = -1, spawnY = -1;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--data") data = next();
        else if (a == "--level") level = std::stoi(next());
        else if (a == "--map") mapFile = next();
        else if (a == "--seconds") seconds = std::stod(next());
        else if (a == "--step") step = std::stod(next());
        else if (a == "--seed") seed = static_cast<std::uint32_t>(std::stoul(next()));
        else if (a == "--out") outDir = next();
        else if (a == "--trace") traceEvery = std::stoi(next());
        else if (a == "--input") events = next();
        else if (a == "--no-player") player = false;
        else if (a == "--spawn") { const auto v = next(); const auto c = v.find(','); spawnX = std::stoi(v.substr(0, c)); spawnY = std::stoi(v.substr(c + 1)); }
        else if (a == "--quiet") quiet = true;
        else if (a == "--size") { const auto s = next(); const auto x = s.find('x'); width = std::stoi(s.substr(0, x)); height = std::stoi(s.substr(x + 1)); }
        else if (a == "--shots") {
            std::stringstream ss(next());
            std::string t;
            while (std::getline(ss, t, ',')) shots.push_back(std::stoi(t));
        } else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (data.empty()) { std::fprintf(stderr, "set --data or GROMADA_DATA\n"); return 77; }
    if (mapFile.empty()) {
        char name[32];
        std::snprintf(name, sizeof name, "%02d.map", level);
        mapFile = data / "maps" / "levels" / name;
    }

    GameResources res{data / "fw.res"};
    GameEngine engine{res};
    engine.camera().width = width;
    engine.camera().height = height;
    engine.loadMap(mapFile, seed);
    World& w = engine.world();

    auto call = [&](int id, std::initializer_list<script::Value> args) {
        std::vector<script::Value> v(args);
        return engine.callBuiltin(id, v).value_or(script::Value{0});
    };

    // HEADER.INI Tact(): kassandra = V_TRANSMISSION1 (81), weapon[0] = V_FLAGMANHEAD11 (88).
    int flagman = 0;
    if (player) {
        const int kassandra = 81;
        auto& cam = engine.camera();
        int px = cam.x + width / 2, py = cam.y + height / 2;
        std::filesystem::path ini = mapFile;
        ini.replace_extension(".INI");
        if (!std::filesystem::exists(ini)) ini.replace_extension(".ini");
        if (spawnX >= 0) { px = spawnX; py = spawnY; }
        else if (auto sp = scriptSpawn(ini)) { px = sp->first; py = sp->second; }
        else std::tie(px, py) = freePlace(w, kassandra, px, py);
        call(127, {kassandra, 1, 60});                         // SetVid(kassandra, VID_MAXHP, 40*3/2)
        flagman = call(68, {kassandra, px, py, 0}).toInt();     // Create
        call(119, {0, flagman});                                // SetFlagman(0, ...)
        w.players()[0].type = 1;
        call(67, {flagman, 35, 88, 0});                         // act_changeweapon
        const int noammo[8] = {20, 80, 60, 10, 10, 0, 0, 0};
        for (int k = 0; k < 8; ++k) call(67, {flagman, 63, noammo[k], k});   // act_addammo
        call(126, {px, py});                                    // SetPosition
        call(128, {1});                                         // SetAutoScroll(1)
    }
    const auto evs = parseEvents(events);
    if (std::getenv("ENGINE_TRACE_DAMAGE"))
        w.damageHook = [&](const Sprite& t, int amount, int src) {
            const Sprite* s = w.byId(src);
            std::printf("  damage t=%d: %d(%d) hp=%d <- %d from %d(%d) at (%d,%d)\n", engine.clock() - 0, t.id, t.nvid, t.hp, amount,
                        src, s ? s->nvid : -1, t.x, t.y);
        };

    std::filesystem::create_directories(outDir);
    std::vector<RGBA8> pixels(static_cast<std::size_t>(width) * height);
    FramebufferRef fb{pixels.data(), height, width};

    const int startClock = engine.clock();
    double acc = 0;
    long frames = 0;
    std::vector<double> frameTimes;
    double simMs = 0, simMax = 0, drawMs = 0;
    std::size_t shotIdx = 0;
    std::sort(shots.begin(), shots.end());
    int nextTrace = 0;
    long sounds = 0;
    int deaths = 0;
    int prevUnits = static_cast<int>(w.gridUnits().size());
    const double total = seconds * 1000.0;
    double t = 0;
    while (t < total) {
        const int dt = static_cast<int>(std::floor(t + step) - std::floor(t));
        t += step;
        acc += dt;
        const int now = engine.clock() - startClock;

        OriginalInput in;
        Sprite* f = w.byId(flagman);
        const int fx0 = f ? f->x : engine.camera().x + width / 2, fy0 = f ? f->y : engine.camera().y + height / 2;
        in.mouseScreenX = static_cast<std::int16_t>(width / 2);
        in.mouseScreenY = static_cast<std::int16_t>(height / 2);
        in.mouseWorldX = static_cast<std::int16_t>(fx0 + 80);
        in.mouseWorldY = static_cast<std::int16_t>(fy0);
        for (const auto& e : evs) {
            const bool starts = now >= e.t && now - dt < e.t + 0;
            const bool held = now >= e.t && now < e.t + std::max(e.duration, 1);
            if (!held && !starts) continue;
            in.mouseWorldX = static_cast<std::int16_t>(fx0 + e.dx);
            in.mouseWorldY = static_cast<std::int16_t>(fy0 + e.dy);
            in.mouseScreenX = static_cast<std::int16_t>(in.mouseWorldX - engine.camera().x);
            in.mouseScreenY = static_cast<std::int16_t>(in.mouseWorldY - engine.camera().y);
            if (in.mouseScreenX < 10 || in.mouseScreenX > width - 10) in.mouseScreenX = static_cast<std::int16_t>(width / 2);
            if (in.mouseScreenY < 10 || in.mouseScreenY > height - 10) in.mouseScreenY = static_cast<std::int16_t>(height / 2);
            if (e.kind == "move" && starts) in.bits |= OriginalInput::LClick | OriginalInput::LDown;
            else if (e.kind == "fire") in.bits |= OriginalInput::RDown | (starts ? OriginalInput::RClick : 0);
            else if (e.kind == "up") in.bits |= OriginalInput::Up;
            else if (e.kind == "down") in.bits |= OriginalInput::Down;
            else if (e.kind == "left") in.bits |= OriginalInput::Left;
            else if (e.kind == "right") in.bits |= OriginalInput::Right;
            else if (e.kind == "jump" && starts && f) w.action(*f, act::jump, in.mouseWorldX, in.mouseWorldY);
        }

        const auto t0 = std::chrono::steady_clock::now();
        engine.beginFrame(dt);
        engine.setInput(in);
        const auto tw0 = std::chrono::steady_clock::now();
        engine.tactWorld();
        const double twMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tw0).count();
        engine.tactPlayers();
        engine.tactGroupAi();
        engine.updateCamera();
        sounds += static_cast<long>(engine.takeSounds().size());
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        simMs += ms;
        simMax = std::max(simMax, ms);
        frameTimes.push_back(ms);
        if (ms > 3.0 && !quiet) std::printf("  slow frame %ld at t=%d: %.3fms (world %.3fms) sprites=%zu\n", frames, now, ms, twMs, w.liveCount());
        ++frames;

        const int units = static_cast<int>(w.gridUnits().size());
        if (units < prevUnits) deaths += prevUnits - units;
        prevUnits = units;

        if (shotIdx < shots.size() && now >= shots[shotIdx]) {
            const auto d0 = std::chrono::steady_clock::now();
            engine.draw(fb);
            drawMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - d0).count();
            char name[64];
            std::snprintf(name, sizeof name, "shot_%06d.ppm", now);
            writePpm(outDir / name, pixels, width, height);
            ++shotIdx;
        }
        if (!quiet && now >= nextTrace) {
            nextTrace += traceEvery;
            int moving = 0, attacking = 0, army[4] = {};
            for (Sprite* u : w.gridUnits()) {
                if (u->state & st::Move) ++moving;
                if (u->state & st::Attack) ++attacking;
                const int a = w.vidOf(*u).army;
                if (a >= 0 && a < 4) ++army[a];
            }
            Sprite* fl = w.byId(flagman);
            std::printf("t=%6d clock=%d sprites=%zu units=%zu [a0=%d a1=%d a2=%d] moving=%d attacking=%d sounds=%ld", now,
                        engine.clock(), w.liveCount(), w.gridUnits().size(), army[0], army[1], army[2], moving, attacking, sounds);
            if (fl) std::printf(" flagman=(%d,%d,%d) hp=%d act=%d", fl->x, fl->y, fl->z, fl->hp, fl->action);
            else if (player) std::printf(" flagman=dead");
            std::printf(" cam=(%d,%d)\n", engine.camera().x, engine.camera().y);
        }
    }
    // final frame drawn for the timing figure
    const auto d0 = std::chrono::steady_clock::now();
    engine.draw(fb);
    const double lastDraw = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - d0).count();
    std::printf("frames=%ld step=%.2fms sim avg=%.3fms max=%.3fms draw(last)=%.3fms sprites=%zu units=%zu unit-losses=%d sounds=%ld\n",
                frames, step, simMs / std::max(1L, frames), simMax, lastDraw, w.liveCount(), w.gridUnits().size(), deaths, sounds);
    (void)drawMs;
    if (!frameTimes.empty()) {
        auto sorted = frameTimes;
        std::ranges::sort(sorted);
        const auto worst = std::ranges::max_element(frameTimes) - frameTimes.begin();
        std::printf("frame time p50=%.3fms p99=%.3fms p99.9=%.3fms max=%.3fms (frame %ld)\n", sorted[sorted.size() / 2],
                    sorted[sorted.size() * 99 / 100], sorted[sorted.size() * 999 / 1000], sorted.back(), static_cast<long>(worst));
    }
    return 0;
}
