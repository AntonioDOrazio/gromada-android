// Game session on the original data (skipped without GROMADA_DATA), with the provisional
// engine: the whole flow of the game driven by the original scripts and the three demos.
//   boot -> logo (0.avi) -> main menu (1.avi) -> 2.avi -> player choice -> new player (name,
//   difficulty, save file) -> mission map -> level 1 briefing (typewriter) -> level 1 ->
//   Esc (game menu) -> logoff -> mission map; then each demo runs to its end.
#include "core/std.hpp"
#include "libgromada/libgromada_all.hpp"
#include "game/session.hpp"
#include "game/engine.hpp"
#include "test.hpp"

#include <unistd.h>

using namespace gromada;
using namespace gromada::game;

namespace {

// Fake movie: lasts `frames` steps (the real decoder is tested by video_*_test).
struct FakeMovies final : MoviePlayer {
    int left = 0;
    std::vector<std::string> opened;
    bool open(const std::filesystem::path& file) override {
        opened.push_back(file.filename().string());
        left = 600;
        return true;
    }
    bool step(double) override { return --left > 0; }
    void blit(std::uint32_t* xrgb, int w, int h, std::size_t pitch) override {
        for (int y = 0; y < h; ++y) std::fill_n(xrgb + y * pitch, w, 0x00203040u);
    }
    AudioStream* audio() override { return nullptr; }
    void close() override { left = 0; }
};

struct Driver {
    Session& s;
    input::PcState pc;
    int frame = 0;

    void run(int frames, double dt = 1.0 / 60) {
        for (int i = 0; i < frames; ++i) {
            s.runFrame(pc, dt);
            clearEdges();
            ++frame;
        }
    }
    void clearEdges() {
        pc.left.pressed = pc.left.released = pc.right.pressed = pc.right.released = false;
        for (auto& k : pc.keys) k.pressed = k.released = false;
        pc.text.clear();
    }
    // A key/click held for `hold` frames, then released for `after` frames.
    void press(input::ButtonState& b, int hold = 3, int after = 3) {
        b = {true, true, false};
        run(1);
        b.down = true;
        run(hold - 1);
        b = {false, false, true};
        run(after);
    }
    void key(input::Key k, int hold = 3, int after = 3) { press(pc.keys[static_cast<std::size_t>(k)], hold, after); }
    void click(int x, int y) {
        pc.cursor = {static_cast<float>(x), static_cast<float>(y)};
        run(1);
        press(pc.left);
    }
    void type(char c) {
        pc.text = std::string(1, c);
        run(1);
    }
    // Runs until the session loads `map` (case-insensitive suffix); false on timeout.
    bool until(std::string_view map, int maxFrames = 4000) {
        for (int i = 0; i < maxFrames; ++i) {
            if (s.currentMap().ends_with(map)) return true;
            run(1);
        }
        std::fprintf(stderr, "timeout waiting for %.*s (current %s)\n", int(map.size()), map.data(), s.currentMap().c_str());
        return false;
    }
};

std::filesystem::path tempDir(const char* name) {
    auto p = std::filesystem::temp_directory_path() / std::format("gromada_{}_{}", name, ::getpid());
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p;
}

void flow(const GameResources& res, const std::filesystem::path& game) {
    const auto save = tempDir("session_flow");
    auto movies = std::make_unique<FakeMovies>();
    FakeMovies* mv = movies.get();
    Mixer mixer{44100};
    SessionConfig cfg;
    cfg.gameDir = game;
    cfg.saveDir = save;
    cfg.log = [](LogLevel l, std::string_view t) { if (l >= LogLevel::Warn) std::fprintf(stderr, "  [session] %.*s\n", int(t.size()), t.data()); };
    Session s{res.vids(), res.sounds(), std::make_unique<GameEngine>(res), &mixer, std::move(movies), cfg};
    // The original files are seeded into the save directory, never written in the game folder.
    CHECK(std::filesystem::exists(save / "save1"));
    CHECK(std::filesystem::exists(save / "fw.ini"));
    s.start();
    Driver d{s};
    CHECK_EQ(s.currentMap(), std::string{"maps\\logo.map"});

    // logo: 0.avi, skipped with a click
    d.run(2);
    CHECK(s.moviePlaying());
    CHECK_EQ(s.lastMovie(), std::string{"avi\\0.avi"});
    d.press(d.pc.left);
    CHECK(d.until("mainmenu.map"));
    d.run(3);
    CHECK(s.moviePlaying()); // 1.avi, the intro
    CHECK_EQ(s.lastMovie(), std::string{"avi\\1.avi"});
    d.key(input::Key::Escape); // a key skips it too
    d.run(5);
    CHECK(!s.moviePlaying());
    CHECK(s.ui().hasMenu());   // V_LOGO
    CHECK(s.scrollLocked());
    CHECK_EQ(s.globalInt("centr_x"), 400); // fw.ini of the data: 800x600
    CHECK_EQ(s.inputContext(), input::Context::Menu);

    // Enter: player.map after the fade (700 ms) and 2.avi
    d.key(input::Key::Enter);
    d.run(60);
    CHECK(s.moviePlaying());
    CHECK_EQ(s.lastMovie(), std::string{"avi\\2.avi"});
    d.press(d.pc.left);
    CHECK(d.until("player.map"));
    CHECK_EQ(s.scriptScreen(), (std::pair{800, 600})); // SetGraphMode(opt_resolution+1)
    d.run(5);
    CHECK_EQ(s.ui().texts().texts().size(), std::size_t{5}); // the five profiles

    // New player in slot 2: hover the row (V_RECTANGLE), Enter on a SENZANOME profile.
    const auto [ox, oy] = s.menuOrigin();
    d.pc.cursor = {static_cast<float>(400 - 30 + ox), static_cast<float>(300 - 60 + 19 + oy)};
    d.run(3);
    CHECK_EQ(s.globalInt("nmenu"), 3);
    d.key(input::Key::Enter);
    CHECK_EQ(s.globalInt("reset_name"), 1);
    CHECK_EQ(s.inputContext(), input::Context::TextEntry);
    for (const char c : std::string{"zeta 9"}) d.type(c);
    d.run(2);
    CHECK_EQ(s.interpreter()->get("player_name").toString(), std::string{"ZETA 9"});
    d.key(input::Key::Enter, 6); // held over the context change: must not become Esc
    CHECK_EQ(s.globalInt("reset_name"), 2);
    d.key(input::Key::Enter);
    CHECK(d.until("maps.map"));
    {
        const auto profile = s.files().read("save2");
        CHECK(profile && profile->starts_with("ZETA 9\r\n0\r\n"));
        std::ifstream original{game / "save2", std::ios::binary};
        std::string orig{std::istreambuf_iterator<char>{original}, {}};
        CHECK(orig.starts_with("SENZANOME")); // game folder untouched
    }
    d.run(10);
    CHECK_EQ(s.globalInt("nmenu"), 27);

    // Start mission 1: briefing over the paused level, typed.
    d.key(input::Key::Enter);
    CHECK(d.until("levels\\01.map"));
    d.run(30);
    CHECK(s.globalInt("gamemenu") != 0);
    const auto* brief = s.ui().text(s.globalInt("gamemenu"));
    CHECK(brief != nullptr);
    if (brief) {
        CHECK(brief->typing());
        const auto shown = brief->visible;
        d.run(60);
        CHECK(brief->visible > shown);
    }
    // Enter on "continue": the level runs, the flagman exists.
    d.key(input::Key::Enter);
    d.run(30);
    CHECK_EQ(s.globalInt("gamemenu"), 0);
    CHECK(!s.ui().hasMenu());
    CHECK_EQ(s.inputContext(), input::Context::Gameplay);
    const std::array<script::Value, 1> army{script::Value{0}};
    CHECK(s.engine().callBuiltin(85, army)->toInt() != 0);
    CHECK(s.playerScreenPosition().has_value());

    // Esc: game menu; Up to "logoff", Enter: back to the mission map.
    d.key(input::Key::Escape);
    d.run(5);
    CHECK(s.globalInt("gamemenu") != 0);
    CHECK_EQ(s.globalInt("nmenu"), 31);
    d.key(input::Key::Up);
    CHECK_EQ(s.globalInt("nmenu"), 30);
    d.key(input::Key::Enter);
    CHECK(d.until("maps.map", 120));
    CHECK_EQ(s.mapHistory().size(), std::size_t{6});
    CHECK_EQ(s.scriptErrors(), std::size_t{0});
    CHECK_EQ(mv->opened.size(), std::size_t{3});

    // Mission 1 again, won with the original cheat code ("cheat" + 'w' -> Win()): the profile
    // records the mission, "YOU WIN" appears, a click goes back to the mission map.
    d.run(10);
    d.key(input::Key::Enter);
    CHECK(d.until("levels\\01.map"));
    d.run(10);
    d.key(input::Key::Enter); // close the briefing
    d.run(10);
    for (const char c : std::string{"cheatw"}) { d.type(c); d.run(1); }
    d.run(5);
    CHECK_EQ(s.globalInt("win"), 1);
    CHECK_EQ(s.globalInt("map") & 1, 1);
    {
        const auto profile = s.files().read("save2");
        CHECK(profile && profile->starts_with("ZETA 9\r\n1\r\n"));
    }
    d.run(150); // 2.5 s: V_YOURWIN
    bool yourWin = false;
    for (const auto& o : s.ui().objects()) yourWin |= o.nvid == ui::vids::YourWin;
    CHECK(yourWin);
    d.run(150); // clickable after 4 s (Tact returns early before) and the birth animation
    d.press(d.pc.left);
    d.run(2);
    CHECK(d.until("maps.map", 120));
    d.run(5);
    CHECK_EQ(s.globalInt("map") & 1, 1); // LoadPlayer() of MAPS.INI reads the saved profile

    // Options from the mission map: music volume +10, Esc writes fw.ini (save directory only).
    d.pc.cursor = {0, 0}; // the options rows follow the mouse: keep it away
    d.run(30);
    d.key(input::Key::Down);
    CHECK_EQ(s.globalInt("nmenu"), 28);
    d.key(input::Key::Enter);
    d.run(60);
    CHECK_EQ(s.globalInt("gamestate"), 1);
    CHECK_EQ(s.globalInt("nmenu"), 12);
    for (int i = 0; i < 3; ++i) d.key(input::Key::Down);
    CHECK_EQ(s.globalInt("nmenu"), 15);
    const int music = s.globalInt("opt_music");
    d.key(input::Key::Right);
    CHECK_EQ(s.globalInt("opt_music"), music + 10);
    d.key(input::Key::Escape);
    d.run(60);
    CHECK_EQ(s.globalInt("gamestate"), 0);
    {
        const auto fw = s.files().read("fw.ini");
        CHECK(fw.has_value());
        if (fw) {
            std::string expected = "1\r\n1\r\n50\r\n" + std::to_string(music + 10) + "\r\n1\r\n";
            CHECK_EQ(*fw, expected);
        }
        std::ifstream original{game / "fw.ini", std::ios::binary};
        CHECK_EQ(std::string(std::istreambuf_iterator<char>{original}, {}), std::string{"1\r\n1\r\n50\r\n0\r\n1\r\n"});
    }
    CHECK_EQ(s.scriptErrors(), std::size_t{0});
    std::filesystem::remove_all(save);
}

// Main menu -> Esc -> credits -> any key -> Exit().
void credits(const GameResources& res, const std::filesystem::path& game) {
    SessionConfig cfg;
    cfg.gameDir = game;
    Session s{res.vids(), res.sounds(), std::make_unique<GameEngine>(res), nullptr, nullptr, cfg};
    s.start("maps\\mainmenu.map"); // without movies
    Driver d{s};
    d.run(10);
    d.key(input::Key::Escape);
    CHECK(d.until("credits.map", 120));
    d.run(10);
    bool creditsShown = false;
    for (const auto& o : s.ui().objects()) creditsShown |= o.nvid == ui::vids::Credits;
    CHECK(creditsShown);
    d.key(input::Key::Enter);
    d.run(60);
    CHECK(s.exitRequested());
}

void runDemo(const GameResources& res, const std::filesystem::path& game, int n) {
    SessionConfig cfg;
    cfg.gameDir = game;
    cfg.render = false;
    std::string ending;
    cfg.log = [&](LogLevel, std::string_view t) { if (t.starts_with("demo")) ending = t; };
    Session s{res.vids(), res.sounds(), std::make_unique<GameEngine>(res), nullptr, nullptr, cfg};
    s.start("maps\\mainmenu.map");
    s.playDemo(n);
    CHECK(s.demoPlaying());
    const std::size_t ticks = s.demo()->file().ticks.size();
    input::PcState pc;
    std::size_t frames = 0;
    // 40 ms per frame: about one recorded tick per frame.
    while (s.demoPlaying() && frames < ticks * 4) {
        s.runFrame(pc, 0.040);
        ++frames;
    }
    CHECK(!s.demoPlaying());
    CHECK(s.currentMap().ends_with("mainmenu.map")); // the trailer
    std::fprintf(stderr, "demo%d: %s\n", n, ending.c_str());
    CHECK_EQ(s.scriptErrors(), std::size_t{0});
    std::fprintf(stderr, "demo%d: %zu ticks replayed in %zu frames\n", n, ticks, frames);
}

} // namespace

int main() {
    SKIP_WITHOUT_DATA();
    const auto game = *gromada_test::dataDir();
    GameResources res{game / "fw.res"};
    flow(res, game);
    credits(res, game);
    for (int n = 1; n <= 3; ++n)
        runDemo(res, game, n);
    return gromada_test::finish();
}
