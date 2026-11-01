// Desktop preview of the UI module: renders menu screens, a mission briefing and the credits
// from the original data into PPM screenshots (convert with: ffmpeg -i x.ppm x.png).
//
// usage: ui_preview <game_dir> <out_dir> [level=1] [res=640|800]
//
// The screens are rebuilt statically with the calls the scripts make when they open them
// (the script interpreter is a separate module): every recipe below cites its script.
// Never commit the output: it contains the game's copyrighted graphics and texts.

#include "core/std.hpp"
#include "engine/level.hpp"
#include "engine/renderer.hpp"
#include "ui/ui.hpp"

using namespace gromada::ui;
namespace fs = std::filesystem;

namespace {

struct Canvas {
    int width, height;
    std::vector<RGBA8> pixels;
    Canvas(int w, int h) : width{w}, height{h}, pixels(static_cast<std::size_t>(w) * h, RGBA8{{0, 0, 0}, 255}) {}
    FramebufferRef fb() { return FramebufferRef{pixels.data(), height, width}; }
};

void writePpm(const Canvas& c, const fs::path& path) {
    std::ofstream out{path, std::ios::binary};
    out << "P6\n" << c.width << ' ' << c.height << "\n255\n";
    for (const auto& p : c.pixels) out.put(static_cast<char>(p.r)).put(static_cast<char>(p.g)).put(static_cast<char>(p.b));
    std::printf("wrote %s\n", path.string().c_str());
}

struct Context {
    fs::path root;
    GameResources& res;
    FontSet fonts;
    Locale locale;
    bool r800 = false;
    int cx() const { return r800 ? 400 : 320; } // centr_x
    int cy() const { return r800 ? 300 : 240; } // centr_y
    int width() const { return r800 ? 800 : 640; }
    int height() const { return r800 ? 600 : 480; }
    MenuFile men(std::string_view name) const { // LoadMenu("maps\\<name>.men") / "<name>8.men"
        const std::string file = std::format("maps\\{}{}.men", name, r800 ? "8" : "");
        return loadMenuFile(*resolveGamePath(root, file), res.vids());
    }
    int printFile(TextManager& t, int x, int y, std::string_view scriptPath) const {
        const auto p = resolveGamePath(root, scriptPath);
        return p ? t.printFile(x, y, *p, vids::Font) : 0;
    }
};

struct Screen {
    MenuScreen menu;
    TextManager texts;
    explicit Screen(const Context& ctx) : menu{ctx.res.vids()}, texts{ctx.fonts} {}
};

void render(const Context& ctx, const Screen& s, Canvas& canvas) {
    drawUi(s.menu, s.texts, ctx.fonts, canvas.fb());
}

// mainmenu.ini: MenuCreate(136,0,320,240) (the main menu runs before SetGraphMode).
void mainMenu(const Context& ctx, const fs::path& out) {
    Screen s{ctx};
    s.menu.create(vids::Logo, 0, 320, 240);
    Canvas c{640, 480};
    render(ctx, s, c);
    writePpm(c, out / "mainmenu.ppm");
}

// credits.ini: MenuCreate(137,0,centr_x,centr_y); if(opt_resolution) MenuCreate(83,...).
void credits(const Context& ctx, const fs::path& out) {
    Screen s{ctx};
    s.menu.create(vids::Credits, 0, ctx.cx(), ctx.cy());
    if (ctx.r800)
        s.menu.create(vids::BlackFrame800, 0, ctx.cx(), ctx.cy());
    Canvas c{ctx.width(), ctx.height()};
    render(ctx, s, c);
    writePpm(c, out / std::format("credits{}.ppm", ctx.r800 ? "8" : ""));
}

// HEADER.INI GameMenuOn(), first menu of a single player game (nmenu=27, service centre
// closed), drawn over the paused level; levels/NN.ini then types the briefing with
// Action(gamemenu,act_settextcount,0,0).
void briefing(const Context& ctx, const fs::path& out, int level, float typedSeconds) {
    Canvas c{640, 480};
    {
        const auto map = loadMap(ctx.res.vids(), ctx.root / "maps" / "levels" / std::format("{:02}.map", level));
        gromada::Level lvl{ctx.res, map};
        gromada::Renderer renderer;
        renderer.setWidth(640);
        lvl.animate(0.0f); // normalises the entities' frame indices
        renderer.draw(lvl, lvl.header().observerX, lvl.header().observerY);
        // GameMenuOn() calls SetGamma(gammamenu = 96 = 0x060) before drawing the menu.
        // Hypothesis (docs/ui.md): SetGamma takes 0xRGB with 3-bit channel gains (0x777 =
        // neutral) applied to the vids without the IgnoreGlobalGamma flag, i.e. the level.
        constexpr int gamma = 0x060;
        auto gain = [](std::uint32_t v, int g) { return static_cast<std::uint8_t>((v & 0xFF) * g / 7); };
        for (int i = 0; i < 640 * 480; ++i) {
            const std::uint32_t p = renderer.xrgb()[i];
            c.pixels[i] = RGBA8{{gain(p >> 16, (gamma >> 8) & 7), gain(p >> 8, (gamma >> 4) & 7), gain(p, gamma & 7)}, 255};
        }
    }
    Screen s{ctx};
    s.menu.create(vids::TextMenu, 27, 320, 240);
    s.menu.create(vids::ClosedButton, 0, 320 + 195, 240 + 116);
    const int text = ctx.printFile(s.texts, 320 - 230, 240 - 140, briefingPath(level));
    s.menu.load(Context{ctx.root, ctx.res, ctx.fonts, ctx.locale, false}.men("gamemenu"));
    if (typedSeconds >= 0) {
        s.texts.setVisibleCount(text, 0);
        for (float t = 0; t < typedSeconds; t += 1.0f / 60)
            s.texts.update(1.0f / 60);
    }
    render(ctx, s, c);
    writePpm(c, out / (typedSeconds >= 0 ? std::format("briefing{:02}_typing.ppm", level) : std::format("briefing{:02}.ppm", level)));
}

// MAPS.INI MapsCreate() for a new player (map = 0 plus the bits the script forces).
void missionChoice(const Context& ctx, const fs::path& out) {
    Screen s{ctx};
    s.menu.load(ctx.men("maps"));
    std::uint32_t map = 0;
    map |= (1u << 6) | (1u << 8) | (1u << 19) | (1u << 20) | (1u << 21) | (1u << 22) | (1u << 23) | (1u << 24) |
           (1u << 25) | (1u << 27) | (1u << 28) | (1u << 30);
    const int cx = ctx.cx() - 320, cy = ctx.cy() - 240;
    auto marker = [&](int n, int x, int y) {
        s.menu.create(map & (1u << n) ? vids::WinnedMap : vids::ChoiceMap, n, cx + x, cy + y);
    };
    auto has = [&](int n) { return (map & (1u << n)) != 0; };
    marker(0, 202, 111);
    if (has(0) || has(1)) marker(1, 166, 142);
    if (has(1) || has(2)) marker(2, 165, 291);
    if (has(2) || has(9)) marker(9, 255, 200);
    if (has(9) || has(3)) marker(3, 112, 203);
    if (has(9) || has(4)) marker(4, 149, 222);
    if (has(3) || has(5)) marker(5, 137, 263);
    if (has(4) || has(7)) marker(7, 189, 249);
    if (has(5) || has(7) || has(10)) marker(10, 327, 214);
    if (has(10) || has(11)) marker(11, 283, 274);
    if (has(10) || has(12)) marker(12, 304, 248);
    if (has(10) || has(13)) marker(13, 362, 89);
    if (has(11) || has(12) || has(14)) marker(14, 242, 233);
    if (has(12) || has(13) || has(15)) marker(15, 365, 203);
    if (has(14) || has(15) || has(16)) marker(16, 287, 112);
    if (has(16) || has(17)) marker(17, 309, 84);
    if (has(17) || has(18)) marker(18, 377, 170);
    if ((map & 523967) == 523967 || has(26)) marker(26, 365, 312);
    if (has(29)) s.menu.create(vids::WinnedMap, 29, cx + 493, cy + 261);
    s.menu.create(vids::TextMenu, 27, ctx.cx(), ctx.cy());
    const int selected = 0; // first map not won
    ctx.printFile(s.texts, ctx.cx() - 80, ctx.cy() + 117, mapSummaryPath(selected + 1));
    if (!has(1)) s.menu.create(vids::ClosedButton, 0, ctx.cx() + 195, ctx.cy() + 116);
    s.menu.setAction(vids::ChoiceMap, selected, 1); // act_menustandup
    Canvas c{ctx.width(), ctx.height()};
    render(ctx, s, c);
    writePpm(c, out / std::format("maps{}.ppm", ctx.r800 ? "8" : ""));
}

// PLAYER.INI main(): five player slots read from save1..save5 (first value = name).
void playerChoice(const Context& ctx, const fs::path& out) {
    Screen s{ctx};
    s.menu.load(ctx.men("player"));
    s.menu.create(vids::TextMenu, 2, ctx.cx(), ctx.cy());
    for (int i = 0; i < 5; ++i) {
        std::string name = ctx.locale.get("STR_NONAME", "NONAME");
        if (const auto save = readGameText(ctx.root, std::format("save{}", i + 1)))
            name = save->substr(0, save->find_first_of("\r\n"));
        s.texts.print(ctx.cx() - 59, ctx.cy() - 64 + i * 19, name, vids::Font);
        s.menu.create(vids::Rectangle, i, ctx.cx() - 30, ctx.cy() - 60 + i * 19);
    }
    Canvas c{ctx.width(), ctx.height()};
    render(ctx, s, c);
    writePpm(c, out / std::format("player{}.ppm", ctx.r800 ? "8" : ""));
}

// HEADER.INI OptionsDraw() with the values of fw.ini (resolution shown as the preview's one).
void options(const Context& ctx, const fs::path& out) {
    Screen s{ctx};
    s.menu.load(ctx.men("options"));
    s.menu.create(vids::TextMenu, 12, ctx.cx(), ctx.cy());
    for (int i = 0; i < 10; ++i)
        s.menu.create(vids::Rectangle, i, ctx.cx() - 70 + (i & 1) * 200, ctx.cy() - 60 + (i / 2) * 19);
    int detail = 1, sound = 50, music = 50, scrolling = 0;
    if (const auto ini = readGameText(ctx.root, "fw.ini")) {
        std::istringstream in{*ini};
        int resolution = 0;
        in >> resolution >> detail >> sound >> music >> scrolling;
    }
    const auto& L = ctx.locale;
    s.texts.print(ctx.cx() - 20, ctx.cy() - 64, ctx.r800 ? "800x600" : "640x480", vids::Font);
    s.texts.print(ctx.cx() - 20, ctx.cy() - 45, detail ? L.get("STR_HIGH") : L.get("STR_LOW"), vids::Font);
    s.menu.create(vids::Volume, sound / 10, ctx.cx() + 63, ctx.cy() - 22);
    s.menu.create(vids::Volume, music / 10, ctx.cx() + 63, ctx.cy() - 3);
    s.texts.print(ctx.cx() - 20, ctx.cy() + 12, scrolling ? L.get("STR_ON") : L.get("STR_OFF"), vids::Font);
    Canvas c{ctx.width(), ctx.height()};
    render(ctx, s, c);
    writePpm(c, out / std::format("options{}.ppm", ctx.r800 ? "8" : ""));
}

// HEADER.INI DesignMenuOn() + first DesignMenuTact() for a new player
// (weapon[0]=V_FLAGMANHEAD11, kassandra=V_TRANSMISSION2, noammo[0]=20).
void design(const Context& ctx, const fs::path& out) {
    Screen s{ctx};
    s.menu.load(ctx.men("design"));
    s.menu.create(vids::TextMenu, 20, ctx.cx(), ctx.cy());
    s.menu.create(vids::WeaponScreen, 0, ctx.cx() + 64, ctx.cy() - 84);
    const int weapons[4] = {88, 0, 0, 0};
    for (int i = 0; i < 4; ++i) {
        const int y = ctx.cy() - 37 + i * 19;
        if (weapons[i]) {
            s.texts.print(ctx.cx() - 86, y, std::string{ctx.res.vids()[weapons[i]].name.data()}, vids::Font);
            s.texts.print(ctx.cx() + 127, y, "20", vids::Font);
        } else {
            s.texts.print(ctx.cx() - 86, y, ctx.locale.get("STR_NONE"), vids::Font);
        }
    }
    s.texts.print(ctx.cx() - 86, ctx.cy() - 37 + 76, std::string{ctx.res.vids()[132].name.data()}, vids::Font);
    for (int i = 0; i < 10; ++i)
        s.menu.create(vids::Rectangle, i, ctx.cx() - 100 + 200 * (i & 1), ctx.cy() - 33 + (i / 2) * 19);
    ctx.printFile(s.texts, ctx.cx() - 80, ctx.cy() + 117, weaponTextPath(1));
    Canvas c{ctx.width(), ctx.height()};
    render(ctx, s, c);
    writePpm(c, out / std::format("design{}.ppm", ctx.r800 ? "8" : ""));
}

// All 256 glyphs of every font, magnified x3, with the layout/measure helpers exercised.
void fontSheet(const Context& ctx, const fs::path& out) {
    for (int nvid : ctx.fonts.fontNvids()) {
        Canvas c{16 * 12 + 8, 16 * 14 + 40};
        for (auto& p : c.pixels) p = RGBA8{{24, 24, 32}, 255};
        for (int code = 0; code < 256; ++code)
            ctx.fonts.drawGlyph(nvid, static_cast<std::uint8_t>(code), 4 + (code % 16) * 12, 4 + (code / 16) * 14, c.fb());
        const std::string sample = utf8ToGame("Unità: Ok\r\nАРЕНА");
        ctx.fonts.drawText(sample, nvid, 4, 16 * 14 + 8, c.fb(), {.align = Align::Center, .boxWidth = c.width - 8});
        Canvas big{c.width * 3, c.height * 3};
        for (int y = 0; y < big.height; ++y)
            for (int x = 0; x < big.width; ++x)
                big.pixels[y * big.width + x] = c.pixels[(y / 3) * c.width + x / 3];
        writePpm(big, out / std::format("font{}.ppm", nvid));
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <game_dir|--smoke> <out_dir> [level=N] [res=640|800]\n"
                             "  --smoke: game folder from $GROMADA_DATA, exit 77 when unset (ctest)\n", argv[0]);
        return 2;
    }
    fs::path root = argv[1];
    if (root == "--smoke") {
        const char* data = std::getenv("GROMADA_DATA");
        if (!data || !*data) {
            std::fprintf(stderr, "SKIP (set GROMADA_DATA to the game folder)\n");
            return 77;
        }
        root = data;
    }
    int level = 1;
    bool r800 = false;
    for (int i = 3; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a.starts_with("level=")) level = std::clamp(std::atoi(argv[i] + 6), 1, 30);
        if (a == "res=800") r800 = true;
    }
    try {
        const fs::path out = argv[2];
        fs::create_directories(out);
        GameResources res{root / "fw.res"};
        Context ctx{root, res, FontSet{res.vids()}, Locale::load(root), r800};
        mainMenu(ctx, out);
        credits(ctx, out);
        briefing(ctx, out, level, -1.0f);
        briefing(ctx, out, level, 2.0f);
        missionChoice(ctx, out);
        playerChoice(ctx, out);
        options(ctx, out);
        design(ctx, out);
        fontSheet(ctx, out);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ui_preview: %s\n", e.what());
        return 1;
    }
    return 0;
}
