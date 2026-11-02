// Synthetic tests of the UI module (no game data): layout, measure, wrapping, alignment,
// encodings, text handles/typewriter, .men parsing, format-7 lens, #define parsing, paths.
#include "ui/ui.hpp"
#include "test.hpp"

using namespace gromada::ui;

namespace {

// Two fake fonts: 1 = V_FONT-like (7x10 cell, 7x8 glyph), 2 = bigger (10x20 cell, 10x16 glyph).
const FontMetricsLookup lookup = [](int nvid) -> std::optional<FontMetrics> {
    if (nvid == 1) return FontMetrics{.advance = 7, .lineHeight = 10, .glyphWidth = 7, .glyphHeight = 8};
    if (nvid == 2) return FontMetrics{.advance = 10, .lineHeight = 20, .glyphWidth = 10, .glyphHeight = 16};
    return std::nullopt;
};

bool sameGlyph(const PlacedGlyph& g, int font, char code, int x, int y) {
    return g.font == font && g.code == static_cast<std::uint8_t>(code) && g.x == x && g.y == y;
}

void testLayout() {
    // Pen starts at (x + 7/2 - 7/2, y + 10/2 - 8/2) = (x, y + 1).
    auto g = layoutText("AB\r\nC", 1, 100, 50, lookup);
    CHECK_EQ(g.size(), std::size_t{3});
    CHECK(sameGlyph(g[0], 1, 'A', 100, 51));
    CHECK(sameGlyph(g[1], 1, 'B', 107, 51));
    CHECK(sameGlyph(g[2], 1, 'C', 100, 61));

    // Control bytes draw nothing but take a cell (the space is a glyph, an empty one);
    // LF alone also returns.
    g = layoutText("A \tB\nC", 1, 0, 0, lookup);
    CHECK_EQ(g.size(), std::size_t{4});
    CHECK(sameGlyph(g[1], 1, ' ', 7, 1));
    CHECK(sameGlyph(g[2], 1, 'B', 21, 1));
    CHECK(sameGlyph(g[3], 1, 'C', 0, 11));

    // CR alone overprints the line.
    g = layoutText("AB\rC", 1, 0, 0, lookup);
    CHECK(sameGlyph(g[2], 1, 'C', 0, 1));

    // Bytes >= 0x80 are glyphs too (CP866 Cyrillic in the font).
    g = layoutText("\x80\xFF", 1, 0, 0, lookup);
    CHECK_EQ(g.size(), std::size_t{2});
    CHECK(sameGlyph(g[0], 1, '\x80', 0, 1));

    // ESC + byte switches font and takes one cell of the NEW font; the line start after a
    // newline is x0 - advance(first font) + advance(current font) (exe quirk).
    g = layoutText("A\x1b\x02" "BC\nD", 1, 100, 50, lookup);
    CHECK_EQ(g.size(), std::size_t{4});
    CHECK(sameGlyph(g[0], 1, 'A', 100, 51));
    CHECK(sameGlyph(g[1], 2, 'B', 117, 51));
    CHECK(sameGlyph(g[2], 2, 'C', 127, 51));
    CHECK(sameGlyph(g[3], 2, 'D', 103, 71));

    // A trailing ESC is ignored; ESC to an unknown font hides glyphs, keeps the metrics.
    CHECK_EQ(layoutText("A\x1b", 1, 0, 0, lookup).size(), std::size_t{1});
    g = layoutText("A\x1b\x09" "BC\x1b\x01" "D", 1, 0, 0, lookup);
    CHECK_EQ(g.size(), std::size_t{2});
    CHECK(sameGlyph(g[1], 1, 'D', 35, 1));

    // Unknown starting font: nothing drawn. Visible prefix (typewriter).
    CHECK(layoutText("ABC", 9, 0, 0, lookup).empty());
    CHECK_EQ(layoutText("ABCD", 1, 0, 0, lookup, {.maxBytes = 2}).size(), std::size_t{2});
    // The ESC check uses the visible length: "A ESC" with 2 visible bytes is a trailing ESC.
    CHECK_EQ(layoutText("A\x1b\x02" "B", 1, 0, 0, lookup, {.maxBytes = 2}).size(), std::size_t{1});
}

void testMeasure() {
    auto e = measureText("ABC", 1, lookup);
    CHECK_EQ(e.width, 21);
    CHECK_EQ(e.height, 10);
    CHECK_EQ(e.lines, 1);

    e = measureText("AB\r\nCDE  \r\nF", 1, lookup);
    CHECK_EQ(e.width, 35); // trailing spaces count
    CHECK_EQ(e.lines, 3);
    CHECK_EQ(e.height, 30);

    e = measureText("A\n", 1, lookup);
    CHECK_EQ(e.lines, 2);
    CHECK_EQ(e.width, 7);

    e = measureText("", 1, lookup);
    CHECK_EQ(e.width, 0);
    CHECK_EQ(e.lines, 1);

    e = measureText("ABCD", 1, lookup, 3);
    CHECK_EQ(e.width, 21);

    // Mixed fonts: 7 + ESC cell (10) + 10.
    e = measureText("A\x1b\x02" "B", 1, lookup);
    CHECK_EQ(e.width, 27);
}

void testAlign() {
    // Line widths 14 and 28 in a 70 px box.
    auto g = layoutText("AB\nABCD", 1, 100, 0, lookup, {.align = Align::Center, .boxWidth = 70});
    CHECK_EQ(g.size(), std::size_t{6});
    CHECK_EQ(g[0].x, 128);
    CHECK_EQ(g[2].x, 121);
    g = layoutText("AB\r\nABCD", 1, 100, 0, lookup, {.align = Align::Right, .boxWidth = 70});
    CHECK_EQ(g[0].x, 156);
    CHECK_EQ(g[1].x, 163);
    CHECK_EQ(g[2].x, 142);
    CHECK_EQ(g[2].y, 11);
}

void testWrap() {
    CHECK_EQ(wrapText("THE QUICK BROWN FOX", 10), std::string{"THE QUICK\nBROWN FOX"});
    CHECK_EQ(wrapText("ABCDEFGHIJKL", 5), std::string{"ABCDE\nFGHIJ\nKL"});
    CHECK_EQ(wrapText("AB ABCDEFGHIJKL", 5), std::string{"AB\nABCDE\nFGHIJ\nKL"});
    CHECK_EQ(wrapText("AB\r\nCD", 10), std::string{"AB\r\nCD"});
    CHECK_EQ(wrapText("A  B", 10), std::string{"A  B"});
    CHECK_EQ(wrapText("ONE TWO", 3), std::string{"ONE\nTWO"});
    CHECK_EQ(wrapText("ABC", 0), std::string{"ABC"});
    CHECK_EQ(wrapText("", 4), std::string{});
    // ESC pairs take one cell.
    CHECK_EQ(wrapText("A\x1b\x02" "B C", 5), std::string{"A\x1b\x02" "B C"});
    CHECK_EQ(wrapText("A\x1b\x02" "B C", 4), std::string{"A\x1b\x02" "B\nC"});
    // Every wrapped line fits.
    const std::string text = "OBIETTIVO: ABBANDONARE L'ARENA ATTRAVERSO UN TELETRASPORTATORE CHE APPARE";
    const auto wrapped = wrapTextToWidth(text, 140, *lookup(1));
    CHECK(measureText(wrapped, 1, lookup).width <= 140);
    CHECK_EQ(std::ranges::count(wrapped, '\n') > 0, true);
}

void testEncoding() {
    CHECK_EQ(utf8ToGame("UNITA'"), std::string{"UNITA'"});
    CHECK_EQ(utf8ToGame("Unità è Ò"), std::string{"Unita' e' O'"});
    CHECK_EQ(utf8ToGame("ç ñ ü"), std::string{"c n u"});
    CHECK_EQ(utf8ToGame("АРЕНА"), std::string{"\x80\x90\x85\x8D\x80"});
    CHECK_EQ(utf8ToGame("ая"), std::string{"\xA0\xEF"});
    CHECK_EQ(utf8ToGame("€"), std::string{"?"});
    CHECK_EQ(utf8ToGame("€", '_'), std::string{"_"});
    CHECK_EQ(utf8ToGame("°"), std::string{"\xF8"});
    CHECK_EQ(utf8ToGame("\xFF"), std::string{"?"}); // invalid UTF-8
    CHECK_EQ(gameToUtf8("\x80\x90\x85\x8D\x80 : NEEDLE"), std::string{"АРЕНА : NEEDLE"});
    // Round trip of every CP866 byte.
    std::string all;
    for (int c = 0x20; c < 0x100; ++c) all += static_cast<char>(c);
    CHECK_EQ(utf8ToGame(gameToUtf8(all)), all);
}

void testTextManager() {
    TextManager texts{[](int) { return TypewriterTiming{.stepSeconds = 0.125f, .lineBreakSeconds = 0.25f, .charSfx = 23, .lineSfx = 24}; }};
    const int a = texts.print(10, 20, "HELLO", 1);
    const int b = texts.print(10, 30, "AB  \r\nCD", 1);
    CHECK_EQ(a, 1);
    CHECK_EQ(b, 2);
    CHECK_EQ(texts.print(0, 0, "", 1), 0);
    CHECK_EQ(texts.print(0, 0, std::string{"\0X", 2}, 1), 0);
    CHECK_EQ(texts.find(a)->visible, std::size_t{5});
    CHECK_EQ(texts.find(b)->bytes, std::string{"AB  \r\nCD"});
    CHECK(!texts.find(0));
    CHECK(texts.print(0, 0, std::string{"AB\0C", 4}, 1) > 0); // C string semantics
    CHECK_EQ(texts.texts().back().bytes, std::string{"AB"});

    // Typewriter (act_settextcount 0).
    CHECK(texts.setVisibleCount(b, 0));
    CHECK(texts.find(b)->typing());
    texts.update(0.125f);
    CHECK_EQ(texts.find(b)->visible, std::size_t{1});
    texts.update(0.125f);
    CHECK_EQ(texts.find(b)->visible, std::size_t{2});
    CHECK_EQ(texts.takeSounds(), (std::vector<int>{23, 23}));
    // White space run with a line break: revealed at once, pause before 'C'.
    texts.update(0.125f);
    CHECK_EQ(texts.find(b)->visible, std::size_t{6});
    CHECK_EQ(texts.takeSounds(), (std::vector<int>{24}));
    texts.update(0.125f);
    texts.update(0.125f);
    CHECK_EQ(texts.find(b)->visible, std::size_t{6});
    texts.update(0.125f);
    CHECK_EQ(texts.find(b)->visible, std::size_t{7});
    texts.update(0.125f);
    CHECK(!texts.find(b)->typing());
    texts.update(1.0f); // no-op once complete
    CHECK_EQ(texts.find(b)->visible, std::size_t{8});

    // Big dt catches up; revealAll.
    texts.setVisibleCount(a, 0);
    texts.update(0.25f);
    CHECK_EQ(texts.find(a)->visible, std::size_t{2});
    texts.revealAll();
    CHECK_EQ(texts.find(a)->visible, std::size_t{5});

    CHECK(texts.erase(a));
    CHECK(!texts.erase(a));
    CHECK(!texts.erase(0));
    CHECK(!texts.find(a));
    CHECK(!texts.setVisibleCount(a, 1));
    const int c = texts.print(0, 0, "X", 1);
    CHECK(c > b); // handles are never reused
    texts.clear();
    CHECK(texts.texts().empty());
    CHECK_EQ(texts.printFile(0, 0, "/nonexistent/file.txt", 1), 0);
}

// TextManager::tick: the original's STEXT sprite (issue #13, checked against the emulated
// original on the demos): one step per sprite tick (frameDuration, once per frame, never in
// the creation frame), the step's sound one tick later at the text position, 150 ms pause
// after a line break without repeating its sound.
void testTypewriterTicks() {
    TextManager texts{[](int) { return TypewriterTiming{.stepSeconds = 0.071f, .lineBreakSeconds = 0.150f, .charSfx = 23, .lineSfx = 24}; }};
    const int t = texts.print(90, 100, "AB\r\nCD", 1);
    texts.setVisibleCount(t, 0);
    using V = std::vector<std::pair<int, std::size_t>>;   // (sound or 0, visible) per frame
    V got;
    std::uint32_t clock = 1000;
    for (int frame = 0; frame < 14; ++frame, clock += 47) {
        texts.tick(clock);
        const auto s = texts.takeScreenSounds();
        CHECK(s.size() <= 1);
        if (!s.empty()) { CHECK_EQ(s[0].x, 90); CHECK_EQ(s[0].y, 100); }
        got.push_back({s.empty() ? 0 : s[0].nsfx, texts.find(t)->visible});
    }
    // ticks every 2 frames (94 ms >= 71); the creation frame does not tick
    const V want{{0, 0}, {0, 0}, {0, 1}, {0, 1}, {23, 2}, {0, 2},   // 'A', 'B'
                 {23, 4}, {0, 4},                                // line break (and its run) revealed
                 {24, 4}, {0, 4},                                // its sound; 150 ms timer running
                 {0, 5}, {0, 5}, {23, 6}, {0, 6}};               // 'C' (no repeat of 24), 'D'
    CHECK(got == want);
    CHECK(texts.takeSounds().empty());   // tick() does not use the non-positional queue
}

void testDefines() {
    const auto d = parseDefines("#define STR_NONAME \"SENZANOME\"\r\n  #define STR_ON  \"ON\" // c\r\n"
                                "#define V_FONT         127 //font\r\n#define ESC \"a\\\\b\\\"c\"\r\nint x;\r\n#define\r\n");
    CHECK_EQ(d.size(), std::size_t{4});
    CHECK_EQ(d.at("STR_NONAME"), std::string{"SENZANOME"});
    CHECK_EQ(d.at("STR_ON"), std::string{"ON"});
    CHECK_EQ(d.at("V_FONT"), std::string{"127"});
    CHECK_EQ(d.at("ESC"), std::string{"a\\b\"c"});
    const Locale locale{d};
    CHECK_EQ(locale.get("STR_ON"), std::string{"ON"});
    CHECK_EQ(locale.get("STR_MISSING", "x"), std::string{"x"});
}

void testPaths() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / std::format("gromada_ui_test_{}", std::random_device{}());
    fs::create_directories(root / "MAPS" / "Levels");
    std::ofstream{root / "MAPS" / "Levels" / "01.TXT", std::ios::binary} << "BRIEF\r\n";
    const auto p = resolveGamePath(root, "maps\\levels\\01.txt");
    CHECK(p.has_value());
    if (p) CHECK_EQ(p->filename().string(), std::string{"01.TXT"});
    CHECK(resolveGamePath(root, "maps/LEVELS/01.Txt").has_value());
    CHECK(!resolveGamePath(root, "maps\\levels\\02.txt"));
    CHECK(!resolveGamePath(root, "maps\\..\\maps\\levels\\01.txt"));
    CHECK_EQ(readGameText(root, briefingPath(1)).value_or(""), std::string{"BRIEF\r\n"});
    CHECK_EQ(briefingPath(7), std::string{"maps\\levels\\07.txt"});
    CHECK_EQ(mapSummaryPath(12), std::string{"maps\\levels\\12s.txt"});
    CHECK_EQ(weaponTextPath(3), std::string{"maps\\w03.txt"});

    TextManager texts;
    CHECK(texts.printFile(5, 6, *p, 1) > 0);
    CHECK_EQ(texts.texts().back().bytes, std::string{"BRIEF\r\n"});
    fs::remove_all(root);
}

std::vector<std::byte> bytes(std::initializer_list<int> values) {
    std::vector<std::byte> out;
    for (int v : values) out.push_back(static_cast<std::byte>(v));
    return out;
}

void testMenuFile() {
    std::vector<Vid> vids(4);
    vids[0].type = ObjectClass::Image;
    vids[1].type = ObjectClass::Static;   // material: 1 byte hp
    vids[2].type = ObjectClass::Building; // asset: hp + behave in map version 0
    vids[3].type = ObjectClass::Unknown;

    std::vector<int> data = {0, 0, 0, 0};
    for (int layer = 0; layer < 16; ++layer) {
        if (layer == 10) // image at (320, 240), direction 0x40
            data.insert(data.end(), {0, 0, 0x40, 0x01, 0xF0, 0x00, 0, 0, 0x40, 0});
        if (layer == 12) { // material + asset
            data.insert(data.end(), {1, 0, 1, 0, 2, 0, 0xFF, 0xFF, 0, 0, 7});
            data.insert(data.end(), {2, 0, 3, 0, 4, 0, 0, 0, 0, 0, 9, 1});
        }
        data.insert(data.end(), {0xFF, 0xFF});
    }
    std::vector<std::byte> raw;
    for (int v : data) raw.push_back(static_cast<std::byte>(v));
    const auto file = parseMenuFile(raw, vids);
    CHECK_EQ(file.header, std::uint32_t{0});
    CHECK_EQ(file.objects.size(), std::size_t{3});
    CHECK_EQ(file.objects[0].layer, std::uint8_t{10});
    CHECK_EQ(file.objects[0].x, std::int16_t{320});
    CHECK_EQ(file.objects[0].y, std::int16_t{240});
    CHECK_EQ(file.objects[0].direction, std::uint8_t{0x40});
    CHECK(!file.objects[0].hp.has_value());
    CHECK_EQ(file.objects[1].z, std::int16_t{-1});
    CHECK_EQ(file.objects[1].hp.value_or(0), std::uint8_t{7});
    CHECK_EQ(file.objects[2].hp.value_or(0), std::uint8_t{9});
    CHECK_EQ(file.objects[2].behave.value_or(0), std::uint8_t{1});
    CHECK_EQ(file.objects[2].layer, std::uint8_t{12});

    auto fails = [&](std::vector<std::byte> b) {
        try { parseMenuFile(b, vids); } catch (const MenuFormatError&) { return true; }
        return false;
    };
    auto truncated = raw; truncated.pop_back();
    CHECK(fails(truncated));
    auto trailing = raw; trailing.push_back(std::byte{0});
    CHECK(fails(trailing));
    CHECK(fails(bytes({0, 0, 0})));
    auto badNvid = raw; badNvid[4] = std::byte{9}; badNvid[5] = std::byte{0};
    CHECK(fails(badNvid));
    auto badClass = raw; badClass[4] = std::byte{3}; badClass[5] = std::byte{0};
    CHECK(fails(badClass));
}

void testMenuScreen() {
    std::vector<Vid> vids(3);
    for (auto& v : vids) { v.type = ObjectClass::Image; v.directionsCount = 4; v.z_layer = 13; }
    MenuScreen menu{vids};
    const int a = menu.create(0, 2, 10, 10);
    const int b = menu.create(1, 2, 10, 10);
    CHECK(a > 0 && b > a);
    CHECK_EQ(menu.create(7, 0, 0, 0), 0);
    CHECK_EQ(menu.objects()[0].layer, 13);
    CHECK_EQ(menu.setAction(0, 2, 4), 1);
    CHECK_EQ(menu.objects()[0].action, 4);
    CHECK_EQ(menu.setAction(0, 1, 4), 0);
    CHECK_EQ(menu.setAction(1, 2, ActDestroy), 1);
    CHECK_EQ(menu.objects().size(), std::size_t{1});
    CHECK(menu.destroy(a));
    CHECK(!menu.destroy(a));

    MenuFile file;
    file.objects.push_back({.nvid = 2, .x = 5, .y = 6, .direction = 0xC0, .layer = 12});
    menu.load(file);
    CHECK_EQ(menu.objects().size(), std::size_t{1});
    CHECK_EQ(menu.objects()[0].dirIndex, 3);
    CHECK_EQ(menu.objects()[0].layer, 12);
    menu.clear();
    CHECK(menu.objects().empty());
}

void testLens() {
    // 8x6 sprite, rows 1..2: row 1 n=3 steps {1,2,x} -> offsets 0,1,3; row 2 n=2 steps {2,x}.
    // centerX = 4 (centre pixel lands on column 3), rowStep 1 then 0.
    VidGraphics gfx;
    gfx.width = 8;
    gfx.height = 6;
    gfx.dataFormat = 7;
    const auto data = bytes({1, 0, 2, 0,  4, 0, 3, 0, 1, 2, 99, 1,  4, 0, 2, 0, 2, 99, 0});
    const VidGraphics::Frame frame{.data = data, .parent = &gfx};

    std::vector<RGBA8> pixels(8 * 6);
    for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 8; ++x)
            pixels[y * 8 + x] = RGBA8{{static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y), 0}, 255};
    FramebufferRef fb{pixels.data(), 6, 8};
    CHECK(applyLensFrame(frame, 0, 0, fb));
    auto at = [&](int x, int y) { return std::pair{int{pixels[y * 8 + x].r}, int{pixels[y * 8 + x].g}}; };
    // Row 1 samples source row 1 around srcCX = 4: columns 1..5 <- x 1,3,4,5,7.
    CHECK((at(0, 1) == std::pair{0, 1}));
    CHECK((at(1, 1) == std::pair{1, 1}));
    CHECK((at(2, 1) == std::pair{3, 1}));
    CHECK((at(3, 1) == std::pair{4, 1}));
    CHECK((at(4, 1) == std::pair{5, 1}));
    CHECK((at(5, 1) == std::pair{7, 1}));
    CHECK((at(6, 1) == std::pair{6, 1}));
    // Row 2 samples source row 2 (rowStep 1): columns 2..4 <- x 2,4,6.
    CHECK((at(2, 2) == std::pair{2, 2}));
    CHECK((at(3, 2) == std::pair{4, 2}));
    CHECK((at(4, 2) == std::pair{6, 2}));
    CHECK((at(5, 2) == std::pair{5, 2}));
    // Untouched rows.
    CHECK((at(3, 0) == std::pair{3, 0}));
    CHECK((at(3, 3) == std::pair{3, 3}));

    // Truncated data is rejected without crashing.
    const auto bad = bytes({1, 0, 2, 0, 4, 0, 3, 0, 1});
    CHECK(!applyLensFrame(VidGraphics::Frame{.data = bad, .parent = &gfx}, 0, 0, fb));
}

} // namespace

int main() {
    testLayout();
    testMeasure();
    testAlign();
    testWrap();
    testEncoding();
    testTextManager();
    testTypewriterTicks();
    testDefines();
    testPaths();
    testMenuFile();
    testMenuScreen();
    testLens();
    return gromada_test::finish();
}
