// UI module against the original data (needs GROMADA_DATA): every font vid, every .men file,
// the menu maps, the lens, LOCALE.INI and glyph coverage of all the game texts.
#include "ui/ui.hpp"
#include "test.hpp"

#include <set>

using namespace gromada::ui;
namespace fs = std::filesystem;

namespace {

std::vector<fs::path> filesWithExtension(const fs::path& dir, std::string_view ext, bool recursive) {
    std::vector<fs::path> out;
    auto visit = [&](const fs::directory_entry& e) {
        auto x = e.path().extension().string();
        std::ranges::transform(x, x.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (e.is_regular_file() && x == ext)
            out.push_back(e.path());
    };
    if (recursive)
        for (const auto& e : fs::recursive_directory_iterator{dir}) visit(e);
    else
        for (const auto& e : fs::directory_iterator{dir}) visit(e);
    std::ranges::sort(out);
    return out;
}

std::string readAll(const fs::path& p) {
    std::ifstream f{p, std::ios::binary};
    return {std::istreambuf_iterator<char>{f}, std::istreambuf_iterator<char>{}};
}

} // namespace

int main() {
    SKIP_WITHOUT_DATA();
    const auto root = *gromada_test::dataDir();
    GameResources res{root / "fw.res"};
    const FontSet fonts{res.vids()};

    // --- fonts --------------------------------------------------------------------------
    const auto fontNvids = fonts.fontNvids();
    CHECK_EQ(fontNvids, std::vector<int>{vids::Font});
    for (int nvid : fontNvids) {
        const Vid& v = res.vids()[nvid];
        const auto& g = v.graphics();
        CHECK_EQ(int{g.numOfFrames}, 256);
        CHECK_EQ(int{g.dataFormat}, 2);
        const auto m = fonts.metrics(nvid);
        CHECK(m.has_value());
        CHECK_EQ(m->advance, 7);
        CHECK_EQ(m->lineHeight, 10);
        CHECK_EQ(m->glyphWidth, 7);
        CHECK_EQ(m->glyphHeight, 8);
        CHECK_EQ(m->originX(), 0);
        CHECK_EQ(m->originY(), 1);
        CHECK_EQ(int{v.z_layer}, 14);
        // Every glyph decodes inside its 7x8 box.
        std::vector<RGBA8> pixels(7 * 8);
        for (int c = 0; c < 256; ++c)
            DrawSprite(g.frames[c], 0, 0, FramebufferRef{pixels.data(), 8, 7});
        for (int c = '0'; c <= '9'; ++c) CHECK(fonts.hasGlyph(nvid, c));
        for (int c = 'A'; c <= 'Z'; ++c) CHECK(fonts.hasGlyph(nvid, c));
        for (int c = 'a'; c <= 'z'; ++c) CHECK(fonts.hasGlyph(nvid, c));
        for (int c = 0x80; c <= 0x9F; ++c) CHECK(fonts.hasGlyph(nvid, c)); // Cyrillic capitals
        for (char c : std::string_view{"'.,:;!?-()/%+"}) CHECK(fonts.hasGlyph(nvid, c));
        CHECK(!fonts.hasGlyph(nvid, ' '));
        CHECK(!fonts.hasGlyph(nvid, 0));
        // Typewriter data of V_FONT: 71 ms frames, sfx 23 (char) and 24 (line break).
        const auto tw = typewriterTimingOf(v);
        CHECK_EQ(tw.charSfx, 23);
        CHECK_EQ(tw.lineSfx, 24);
        CHECK(std::abs(tw.stepSeconds - 0.071f) < 1e-6f);
    }

    // --- every byte of every game text has a glyph (or is white space) ---------------------
    std::vector<fs::path> texts = filesWithExtension(root / "maps", ".txt", true);
    CHECK_EQ(texts.size(), std::size_t{15 + 30 * 2 + 2}); // W*, levels NN/NNS, net NNS
    std::set<int> missing;
    auto checkCoverage = [&](std::string_view bytes, const std::string& where) {
        for (unsigned char c : bytes) {
            if (c == '\r' || c == '\n' || c == ' ' || c == '\t')
                continue;
            if (!fonts.hasGlyph(vids::Font, c) && missing.insert(c).second)
                std::fprintf(stderr, "no glyph for byte 0x%02X (%s)\n", c, where.c_str());
        }
    };
    for (const auto& p : texts)
        checkCoverage(readAll(p), p.filename().string());
    const Locale locale = Locale::load(root);
    for (const auto& [key, value] : locale.strings())
        checkCoverage(value, key);
    for (const Vid& v : res.vids())
        checkCoverage(std::string_view{v.name.data()}, "vid name");
    CHECK(missing.empty());

    // --- localized strings ----------------------------------------------------------------
    CHECK_EQ(locale.strings().size(), std::size_t{6});
    CHECK_EQ(locale.get("STR_NONAME"), std::string{"SENZANOME"});
    CHECK_EQ(locale.get("STR_NONE"), std::string{"NESSUNO"});
    CHECK_EQ(locale.get("STR_HIGH"), std::string{"ALTO"});
    CHECK_EQ(locale.get("STR_LOW"), std::string{"BASSO"});
    CHECK_EQ(locale.get("STR_ON"), std::string{"ON"});
    CHECK_EQ(locale.get("STR_OFF"), std::string{"OFF"});
    const auto header = readGameText(root, "maps\\header.ini");
    CHECK(header.has_value());
    if (header) {
        const auto defines = parseDefines(*header);
        CHECK_EQ(defines.at("V_FONT"), std::to_string(vids::Font));
        CHECK_EQ(defines.at("V_TEXTMENU"), std::to_string(vids::TextMenu));
        CHECK_EQ(defines.at("V_4BUTTON"), std::to_string(vids::FourButton));
        CHECK_EQ(defines.at("V_RECTANGLE"), std::to_string(vids::Rectangle));
        CHECK_EQ(defines.at("V_LOGO"), std::to_string(vids::Logo));
        CHECK_EQ(defines.at("V_CREDITS"), std::to_string(vids::Credits));
        CHECK_EQ(defines.at("V_SCREEN"), std::to_string(vids::Screen));
        CHECK_EQ(defines.at("V_CHOICEMAP"), std::to_string(vids::ChoiceMap));
        CHECK_EQ(defines.at("V_WINNEDMAP"), std::to_string(vids::WinnedMap));
        CHECK_EQ(defines.at("V_YOURLOSE"), std::to_string(vids::YourLose));
        CHECK_EQ(defines.at("V_YOURWIN"), std::to_string(vids::YourWin));
        CHECK_EQ(defines.at("act_settextcount"), std::string{"32"});
    }
    // Script paths resolve case-insensitively (the files are NN.TXT, scripts say nn.txt).
    for (int level = 1; level <= 30; ++level) {
        const auto brief = readGameText(root, briefingPath(level));
        const auto summary = readGameText(root, mapSummaryPath(level));
        CHECK(brief.has_value() && !brief->empty());
        CHECK(summary.has_value() && !summary->empty());
        if (brief) {
            const auto extent = measureText(*brief, vids::Font, fonts.lookup());
            CHECK(extent.width <= 490);  // 69 cells at most (trailing spaces included)
            CHECK(extent.height <= 180);
        }
    }
    for (int w = 1; w <= 15; ++w)
        CHECK(readGameText(root, weaponTextPath(w)).has_value());

    // --- .men files -------------------------------------------------------------------------
    const auto menFiles = filesWithExtension(root / "maps", ".men", false);
    CHECK_EQ(menFiles.size(), std::size_t{14});
    for (const auto& p : menFiles) {
        try {
            const auto file = loadMenuFile(p, res.vids());
            CHECK_EQ(file.header, std::uint32_t{0});
            CHECK(!file.objects.empty());
            const bool is800 = p.stem().string().back() == '8';
            for (const auto& o : file.objects) {
                CHECK_EQ(int{o.layer}, int{res.vids()[o.nvid].z_layer});
                CHECK_EQ(int{o.action}, 0);
                CHECK(o.x >= 0 && o.x < (is800 ? 800 : 640));
                CHECK(o.y >= 0 && o.y < (is800 ? 600 : 480));
                CHECK(!o.hp.has_value());
            }
            MenuScreen menu{res.vids()};
            menu.load(file);
            for (const auto& o : menu.objects())
                CHECK(menu.frameOf(o).has_value());
            std::vector<RGBA8> pixels(800 * 600);
            menu.draw(FramebufferRef{pixels.data(), 600, 800});
        } catch (const std::exception& e) {
            CHECK(!"cannot load .men file");
            std::fprintf(stderr, "%s: %s\n", p.string().c_str(), e.what());
        }
    }
    const auto gamemenu = loadMenuFile(root / "maps" / "gamemenu.men", res.vids());
    CHECK_EQ(gamemenu.objects.size(), std::size_t{6});
    CHECK_EQ(int{gamemenu.objects[0].nvid}, vids::Lens);
    CHECK_EQ(int{gamemenu.objects[1].nvid}, vids::BackScreen);

    // --- menu maps: ordinary maps with one marker object, the screen comes from the script ---
    for (const char* name : {"mainmenu", "logo", "credits", "maps", "net", "player", "win", "new"}) {
        try {
            const auto map = loadMap(res.vids(), root / "maps" / std::format("{}.map", name));
            CHECK(map.header.width > 0 && !map.objects.empty());
        } catch (const std::exception& e) {
            CHECK(!"cannot load menu map");
            std::fprintf(stderr, "%s.map: %s\n", name, e.what());
        }
    }

    // --- the lens (format 7) consumes its data exactly ------------------------------------------
    {
        const auto& g = res.vids()[vids::Lens].graphics();
        CHECK_EQ(int{g.dataFormat}, 7);
        SpanStreamReader r = g.frames[0].read();
        r.read<std::uint16_t>();
        const int rows = r.read<std::uint16_t>();
        for (int i = 0; i < rows; ++i) {
            r.read<std::int16_t>();
            const int n = r.read<std::int16_t>();
            r.skip(n + 1);
        }
        CHECK_EQ(r.bytesRemaining(), std::size_t{0});
        std::vector<RGBA8> pixels(640 * 480);
        CHECK(applyLensFrame(g.frames[0], 0, 0, FramebufferRef{pixels.data(), 480, 640}));
        std::vector<RGBA8> big(800 * 600);
        CHECK(applyLensFrame(g.frames[0], 80, 60, FramebufferRef{big.data(), 600, 800}));
    }

    // --- texts: PrintFile of every briefing through the manager ---------------------------------
    TextManager manager{fonts};
    for (int level = 1; level <= 30; ++level) {
        const auto path = resolveGamePath(root, briefingPath(level));
        CHECK(path && manager.printFile(90, 100, *path, vids::Font) > 0);
    }
    CHECK_EQ(manager.texts().size(), std::size_t{30});

    return gromada_test::finish();
}
