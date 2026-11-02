// Game session, synthetic tests (no game data): input record, save files, menu list and menu
// update, texts, the frame order with a mock engine, script builtins with synthetic scripts,
// int cells across Load(), demo playback and recording.
#include "core/std.hpp"
#include "libgromada/libgromada_all.hpp"
#include "game/session.hpp"
#include "test.hpp"

#include <unistd.h>

using namespace gromada;
using namespace gromada::game;
using script::Value;

namespace {

std::filesystem::path tempDir(const char* name) {
    auto p = std::filesystem::temp_directory_path() / std::format("gromada_{}_{}", name, ::getpid());
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p;
}

void writeFile(const std::filesystem::path& p, std::string_view text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out{p, std::ios::binary};
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string readFile(const std::filesystem::path& p) {
    std::ifstream in{p, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, {}};
}

// ---- synthetic vids ---------------------------------------------------------------------------
std::vector<Vid> makeVids() {
    std::vector<Vid> vids(130);
    auto make = [&](int n, ObjectClass cls, int sx, int sy, int layer, int dirs, std::initializer_list<int> anims) {
        Vid& v = vids[static_cast<std::size_t>(n)];
        v.type = cls;
        v.sizeX = static_cast<std::uint16_t>(sx);
        v.sizeY = static_cast<std::uint16_t>(sy);
        v.z_layer = static_cast<std::uint8_t>(layer);
        v.directionsCount = static_cast<std::uint8_t>(dirs);
        int a = 0;
        for (const int len : anims) v.animationLengths[static_cast<std::size_t>(a++)] = static_cast<std::uint8_t>(len);
        auto g = std::make_shared<VidGraphics>();
        g->dataFormat = 2;
        g->frameDuration = 71;
        g->numOfFrames = 0;
        g->dataSize = 0;
        g->width = static_cast<std::uint16_t>(sx);
        g->height = static_cast<std::uint16_t>(sy);
        v.graphicsData = g;
    };
    make(1, ObjectClass::Image, 100, 20, 12, 4, {1, 0, 1, 0, 1});     // button, 4 directions
    make(2, ObjectClass::Image, 200, 12, 14, 12, {1});                // invisible row (V_RECTANGLE-like)
    make(3, ObjectClass::Image, 640, 480, 12, 1, {1});                // full-screen console
    make(4, ObjectClass::Image, 50, 50, 15, 1, {3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3}); // noise (death anim)
    make(5, ObjectClass::Image, 300, 300, 12, 1, {2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2}); // birth anim
    make(6, ObjectClass::Static, 10, 10, 3, 1, {1});                  // not a button
    make(127, ObjectClass::Font, 7, 10, 14, 255, {1});
    vids[127].nsfx[1] = 23;
    vids[127].nsfx[2] = 24;
    return vids;
}

// .men file: u32 0, 16 layers of {u16 nvid, s16 x, s16 y, s16 z, u8 dir, u8 action} ... 0xFFFF
std::string makeMen(std::initializer_list<std::array<int, 4>> objects /* nvid, x, y, dirbyte */,
                    const std::vector<Vid>& vids) {
    std::string out(4, '\0');
    auto p16 = [&](int v) { out += char(v & 0xff); out += char((v >> 8) & 0xff); };
    for (int layer = 0; layer < 16; ++layer) {
        for (const auto& o : objects) {
            if (vids[static_cast<std::size_t>(o[0])].z_layer != layer) continue;
            p16(o[0]); p16(o[1]); p16(o[2]); p16(0);
            out += char(o[3]);
            out += char(0);
        }
        p16(0xFFFF);
    }
    return out;
}

// ---- mock engine ------------------------------------------------------------------------------
struct MockEngine final : Engine {
    std::vector<std::string>* events;
    int clk = 0;
    Camera cam;
    std::vector<OriginalInput> inputs;
    std::vector<std::string> maps;
    bool scrollLock = false;
    explicit MockEngine(std::vector<std::string>* e) : events{e} {}
    void loadMap(const std::filesystem::path& f, std::uint32_t seed) override {
        maps.push_back(f.filename().string());
        clk = 4000000; // realistic: demos tell ticks from FRead records by clock >= 0x10000
        events->push_back(std::format("loadMap {} {}", f.filename().string(), seed));
    }
    void unloadMap() override { events->push_back("unloadMap"); }
    void beginFrame(int ms) override { clk += std::min(ms, 71); events->push_back(std::format("beginFrame {}", ms)); }
    void setInput(const OriginalInput& in) override { inputs.push_back(in); events->push_back(std::format("setInput {}", in.bits)); }
    void tactWorld() override { events->push_back("tactWorld"); }
    void tactPlayers() override { events->push_back("tactPlayers"); }
    void tactGroupAi() override { events->push_back("tactGroupAi"); }
    void updateCamera() override { events->push_back("updateCamera"); }
    void draw(FramebufferRef) override { events->push_back("draw"); }
    void drawScreenEffects(FramebufferRef) override { events->push_back("effects"); }
    int clock() const override { return clk; }
    Camera& camera() override { return cam; }
    CursorKind cursorAt(int, int) const override { return CursorKind::Arrow; }
    std::vector<SoundEvent> takeSounds() override { events->push_back("takeSounds"); return {}; }
    std::optional<Value> callBuiltin(int id, std::span<const Value>) override {
        if (!isEngineBuiltin(id)) return std::nullopt;
        events->push_back(std::format("engine {}", id));
        if (id == 69) return Value{clk};
        return Value{0};
    }
    int rand() override { return 0; }
    void setScrollLock(bool on) override { scrollLock = on; }
};

struct Fixture {
    std::filesystem::path game = tempDir("session_game"), save = tempDir("session_save");
    std::vector<Vid> vids = makeVids();
    std::vector<std::string> events;
    std::vector<std::string> logs;
    MockEngine* engine = nullptr;
    std::unique_ptr<Session> s;

    Fixture() {
        writeFile(game / "maps" / "a.map", "map");
        writeFile(game / "maps" / "b.map", "map");
        writeFile(game / "maps" / "test.men", makeMen({{3, 320, 240, 0}, {1, 100, 300, 0}, {1, 500, 300, 64}}, vids));
    }
    ~Fixture() {
        s.reset();
        std::filesystem::remove_all(game);
        std::filesystem::remove_all(save);
    }
    void start(std::string_view aIni, std::string_view bIni = "main(){}") {
        writeFile(game / "maps" / "a.ini", aIni);
        writeFile(game / "maps" / "b.ini", bIni);
        auto e = std::make_unique<MockEngine>(&events);
        engine = e.get();
        SessionConfig cfg;
        cfg.gameDir = game;
        cfg.saveDir = save;
        cfg.predeclareBuiltins = true; // synthetic scripts don't include maps\header.ini
        cfg.log = [this](LogLevel, std::string_view t) { logs.emplace_back(t); };
        s = std::make_unique<Session>(vids, std::span<const SoundData>{}, std::move(e), nullptr, nullptr, cfg);
        s->start("maps\\a.map");
    }
    void frame(const input::PcState& pc = {}, double dt = 1.0 / 60) { s->runFrame(pc, dt); }
    std::int32_t g(std::string_view name, int i = 0) { return s->globalInt(name, i); }
};

// ---- tests ---------------------------------------------------------------------------------------
void testInputRecord() {
    InputRecordBuilder b;
    input::PcState pc;
    pc.left = {true, true, false};
    pc.keys[static_cast<std::size_t>(input::Key::Enter)] = {true, true, false};
    pc.keys[static_cast<std::size_t>(input::Key::Escape)] = {true, true, false};
    pc.keys[static_cast<std::size_t>(input::Key::Up)] = {true, true, false};
    auto r = b.build(pc, 100, 50, 1000, 2000, 1.0f / 60);
    CHECK_EQ(r.mouseScreenX, 100);
    CHECK_EQ(r.mouseScreenY, 50);
    CHECK_EQ(r.mouseWorldX, 1100);
    CHECK_EQ(r.mouseWorldY, 2050);
    CHECK_EQ(r.bits, OriginalInput::LClick | OriginalInput::LDown | OriginalInput::Up);
    CHECK_EQ(r.key, keycode::ArrowUp); // keys of one frame are delivered one per frame
    pc.left = {true, false, false};
    for (auto& k : pc.keys) k.pressed = false;
    r = b.build(pc, 0, 0, 0, 0, 1.0f / 60);
    CHECK_EQ(r.bits, OriginalInput::LDown | OriginalInput::Up);
    CHECK_EQ(r.key, keycode::Escape);
    r = b.build(pc, 0, 0, 0, 0, 1.0f / 60);
    CHECK_EQ(r.key, keycode::Enter);
    r = b.build(pc, 0, 0, 0, 0, 1.0f / 60);
    CHECK_EQ(r.key, 0);
    // Typematic repeat of the held keys: first repeat after 0.5 s, then 15 per second.
    int repeats = 0;
    for (int i = 0; i < 60; ++i) // 1 s more
        repeats += b.build(pc, 0, 0, 0, 0, 1.0f / 60).key != 0;
    CHECK(repeats >= 20 && repeats <= 28); // 3 keys x (1 + ~7.5) repeats, delivered one per frame
    // right button, text, other keys
    InputRecordBuilder c;
    input::PcState q;
    q.right = {true, true, false};
    q.text = "a";
    r = c.build(q, 0, 0, 0, 0, 0.0f);
    CHECK_EQ(r.bits, OriginalInput::RClick | OriginalInput::RDown);
    CHECK_EQ(r.key, 'a');
    CHECK_EQ(InputRecordBuilder::keyCode(input::Key::Tab), 9);
    CHECK_EQ(InputRecordBuilder::keyCode(input::Key::Backspace), 8);
    CHECK_EQ(InputRecordBuilder::keyCode(input::Key::Num3), '3');
    CHECK_EQ(InputRecordBuilder::keyCode(input::Key::Left), 1);
    CHECK_EQ(InputRecordBuilder::keyCode(input::Key::Down), 4);
}

void testSaveFiles() {
    const auto game = tempDir("files_game"), save = tempDir("files_save");
    writeFile(game / "save1", "PLAYER\r\n7\r\n");
    writeFile(game / "FW.INI", "0\r\n1\r\n");
    writeFile(game / "maps" / "data.txt", "x\r\n");
    writeFile(save / "save2", "MINE\r\n");
    SaveFiles f{game, save};
    CHECK_EQ(f.seed(), 2); // save1 and fw.ini (FW.INI matched case-insensitively)
    CHECK_EQ(f.seed(), 0);
    CHECK_EQ(readFile(save / "save1"), std::string{"PLAYER\r\n7\r\n"});
    CHECK_EQ(readFile(save / "save2"), std::string{"MINE\r\n"});
    CHECK_EQ(f.open("missing"), 0);
    // FRead: lines without CR/LF, the previous line at the end and for handle 0.
    const int h = f.open("save1");
    CHECK(h != 0);
    CHECK_EQ(f.readLine(h), std::string{"PLAYER"});
    CHECK_EQ(f.readLine(h), std::string{"7"});
    CHECK_EQ(f.readLine(h), std::string{"7"});
    CHECK_EQ(f.readLine(0), std::string{"7"});
    CHECK(f.close(h));
    CHECK(!f.close(h));
    // FCreate + FWrite: CRLF, in the save directory only.
    const int w = f.create("save3");
    CHECK(w != 0);
    CHECK(std::filesystem::exists(save / "save3")); // created right away
    f.writeLine(w, "NAME");
    f.writeLine(w, "42");
    f.close(w);
    CHECK_EQ(readFile(save / "save3"), std::string{"NAME\r\n42\r\n"});
    CHECK(!std::filesystem::exists(game / "save3"));
    // A file of the game folder opened and written: the copy goes to the save directory.
    const int m = f.open("maps\\DATA.TXT");
    CHECK(m != 0);
    f.writeLine(m, "y");
    f.close(m);
    CHECK_EQ(readFile(game / "maps" / "data.txt"), std::string{"x\r\n"});
    CHECK_EQ(readFile(save / "maps" / "DATA.TXT"), std::string{"y\r\n"}); // the name the script used
    // Long lines: fgets(128) returns 127 bytes at a time.
    writeFile(save / "long", std::string(200, 'a') + "\r\nb\r\n");
    const int l = f.open("long");
    CHECK_EQ(f.readLine(l).size(), std::size_t{127});
    CHECK_EQ(f.readLine(l).size(), std::size_t{73});
    CHECK_EQ(f.readLine(l), std::string{"b"});
    f.close(l);
    // FileCopy into the save directory.
    CHECK(f.copy("save1", "copy1"));
    CHECK_EQ(readFile(save / "copy1"), std::string{"PLAYER\r\n7\r\n"});
    // No escaping the directories.
    CHECK_EQ(f.create("..\\evil"), 0);
    CHECK_EQ(f.create("/tmp/evil"), 0);
    CHECK_EQ(f.create("c:\\evil"), 0);
    CHECK_EQ(SaveFiles::sanitize("maps\\\\x.ini").value_or(""), std::string{"maps/x.ini"});
    // Without a save directory: memory only.
    SaveFiles mem{game, {}};
    const int k = mem.create("fw.ini");
    mem.writeLine(k, "5");
    mem.close(k);
    CHECK_EQ(readFile(game / "FW.INI"), std::string{"0\r\n1\r\n"});
    const int k2 = mem.open("FW.ini");
    CHECK_EQ(mem.readLine(k2), std::string{"5"});
    std::filesystem::remove_all(game);
    std::filesystem::remove_all(save);
}

void testMenuList() {
    auto vids = makeVids();
    SessionUi ui{vids};
    const int console = ui.create(3, 0, 320, 240);   // layer 12
    const int b1 = ui.create(1, 0, 100, 300);         // layer 12, newer: before the console
    const int row = ui.create(2, 5, 320, 100);        // layer 14: first
    const int other = ui.create(6, 0, 10, 10);        // not a button: not in the menu list
    CHECK(console && b1 && row && other);
    const auto list = ui.menuList();
    CHECK_EQ(list.size(), std::size_t{3});
    CHECK_EQ(list[0], row);
    CHECK_EQ(list[1], b1);
    CHECK_EQ(list[2], console);
    CHECK(isSessionHandle(row) && !isTextHandle(row));

    // Hover: the first object under the mouse is selected; overlapping ones get "select".
    std::uint16_t bits = 0;
    CHECK(!ui.updateMenu(100, 300, bits));
    CHECK_EQ(ui.menuState().selected, 1);
    CHECK_EQ(ui.menuState().menuVid(), 0);
    CHECK_EQ(ui.find(b1)->action, act::MenuSelectDown);
    CHECK_EQ(ui.find(console)->action, act::MenuSelectDown);
    // Click: consumed (all bits zeroed), MenuVid, press action, the scan stops.
    bits = OriginalInput::LClick | OriginalInput::LDown | OriginalInput::Up;
    CHECK(ui.updateMenu(100, 300, bits));
    CHECK_EQ(bits, 0);
    CHECK_EQ(ui.menuState().menuVid(), 1);
    CHECK_EQ(ui.find(b1)->action, act::MenuPressDown);
    // Leaving: back to stand.
    bits = 0;
    ui.updateMenu(600, 20, bits);
    CHECK_EQ(ui.find(b1)->action, act::Stand);
    CHECK_EQ(ui.menuState().selected, 3); // the console covers the screen
    // Edges are inclusive: half sizes 50 x 10.
    CHECK(ui.hit(*ui.find(b1), 150, 310));
    CHECK(!ui.hit(*ui.find(b1), 151, 310));
    // Rows with direction indices, MenuDir.
    ui.updateMenu(320, 100, bits);
    CHECK_EQ(ui.menuState().selected, 2);
    CHECK_EQ(ui.menuState().dir, 5);
    // Disabled buttons are skipped.
    ui.changeAction(row, act::MenuDisable);
    ui.updateMenu(320, 100, bits);
    CHECK_EQ(ui.menuState().selected, 3);
    // Two-position buttons keep their low bit.
    ui.changeAction(b1, act::MenuStandUp);
    ui.updateMenu(100, 300, bits);
    CHECK_EQ(ui.find(b1)->action, act::MenuSelectUp);
    bits = OriginalInput::LClick;
    ui.updateMenu(100, 300, bits);
    CHECK_EQ(ui.find(b1)->action, act::MenuPressUp);
    ui.updateMenu(0, 0, bits);
    CHECK_EQ(ui.find(b1)->action, act::MenuStandUp);
    // MenuAction / act_changeaction with a direction byte / act_destroy.
    const int b2 = ui.create(1, 2, 500, 300);
    CHECK_EQ(ui.menuAction(1, 2, act::MenuStandUp), 1);
    CHECK_EQ(ui.find(b2)->action, act::MenuStandUp);
    ui.objectAction(b2, act::ChangeAction, 0, 192); // 192/256 of 4 directions = 3
    CHECK_EQ(ui.find(b2)->dirIndex, 3);
    CHECK_EQ(ui.objectAction(b2, act::GetAction, 0, 0), 0);
    CHECK_EQ(ui.menuAction(1, 3, act::Destroy), 1);
    CHECK(ui.find(b2) == nullptr);
    CHECK_EQ(ui.menuList().size(), std::size_t{3});
    // Animations on the game clock (SpriteTact): one frame per tick when 71 ms have passed,
    // at most one per frame; birth then stand; death removes the object at its end.
    const int born = ui.create(5, 0, 320, 240);
    CHECK_EQ(ui.find(born)->action, act::Birth);
    ui.update(0.f, 70);
    CHECK_EQ(ui.find(born)->frame, 0);
    ui.update(0.f, 71);
    CHECK_EQ(ui.find(born)->frame, 1);
    ui.update(0.f, 1000);   // no catch-up
    CHECK_EQ(ui.find(born)->action, act::Stand);
    const int noise = ui.create(4, 0, 50, 50);   // created at 1000
    ui.objectAction(noise, act::ChangeAction, act::Death, 0);
    ui.update(0.f, 1071);
    ui.update(0.f, 1142);
    CHECK(ui.find(noise) != nullptr);
    ui.update(0.f, 1212);
    CHECK(ui.find(noise) != nullptr);
    ui.update(0.f, 1213);
    CHECK(ui.find(noise) == nullptr);
    // Pause(0) brings the clock back: the timers move with it.
    CHECK_EQ(ui.find(born)->lastTact, 1213u);
    ui.update(0.f, 1250);
    ui.update(0.f, 250);    // rewind by 1000
    CHECK_EQ(ui.find(born)->lastTact, 213u);
    // Init: a standing Image of 3+ frames starts at a random frame (shared rand()).
    int calls = 0;
    ui.setRandom([&] { ++calls; return 2; });
    vids[1].animationLengths[0] = 3;
    const int phased = ui.create(1, 0, 600, 20);
    CHECK_EQ(calls, 1);
    CHECK_EQ(ui.find(phased)->frame, 2);
    vids[1].animationLengths[0] = 1;
    // FreeMenu.
    ui.freeList(UiList::Menu);
    CHECK(!ui.hasMenu());
    CHECK(ui.objects().empty());
}

void testTexts() {
    const auto vids = makeVids();
    SessionUi ui{vids};
    CHECK_EQ(ui.print(10, 10, "", 127), 0);
    const int t = ui.print(10, 10, "HELLO WORLD", 127);
    CHECK(isTextHandle(t));
    CHECK_EQ(ui.text(t)->visible, std::size_t{11});
    // Action(t, act_settextcount, 0, 0): the typewriter starts (one step per 71 ms).
    ui.objectAction(t, act::SetTextCount, 0, 0);
    CHECK_EQ(ui.text(t)->visible, std::size_t{0});
    const auto sounds = ui.update(0.075f, 0);
    CHECK_EQ(ui.text(t)->visible, std::size_t{1});
    CHECK(!sounds.empty() && sounds[0] == 23);
    ui.update(1.0f, 0);
    CHECK(!ui.text(t)->typing());
    CHECK_EQ(ui.position(t).value()[0], 10);
    // Destroy through Action and DeleteText.
    const int u = ui.print(0, 0, "X", 127);
    ui.objectAction(u, act::Destroy, 0, 0);
    CHECK(ui.text(u) == nullptr);
    CHECK(ui.deleteText(t));
    CHECK(!ui.deleteText(t));
    CHECK(!ui.deleteText(0));
}

void testFrameOrder() {
    Fixture f;
    f.start(R"(
extern Print(int i) 66;
extern GetTime() 69;
extern Load(string filename) 64;
extern LoadMenu(string filename) 71;
int n;
main() { n++; GetTime(); if (n==1) LoadMenu("maps\\test.men"); if (n==3) { Load("maps\\b.map"); Print(7); } }
)");
    CHECK_EQ(f.engine->maps.size(), std::size_t{1});
    f.events.clear();
    f.frame();
    const std::vector<std::string> expected = {"beginFrame 16", "setInput 0", "engine 69", "draw", "tactWorld",
                                               "tactPlayers", "tactGroupAi", "updateCamera", "takeSounds", "effects"};
    CHECK(f.events == expected);
    if (f.events != expected)
        for (const auto& e : f.events) std::fprintf(stderr, "  event: %s\n", e.c_str());
    // A click on a button: the menu update consumes it and re-sends the record without bits.
    f.events.clear();
    input::PcState pc;
    pc.cursor = {100, 300};
    pc.left = {true, true, false};
    f.frame(pc);
    const auto setInputs = std::ranges::count_if(f.events, [](const std::string& e) { return e.starts_with("setInput"); });
    CHECK_EQ(setInputs, 2);
    CHECK_EQ(f.engine->inputs.back().bits, 0);
    CHECK_EQ(f.s->ui().menuState().menuVid(), 1);
    // Load() posted in the frame runs after it: Print(7) still executed, then the map changes.
    f.events.clear();
    f.frame();
    CHECK_EQ(f.engine->maps.size(), std::size_t{2});
    CHECK(f.s->currentMap() == "maps\\b.map");
    CHECK(std::ranges::find(f.logs, std::string{"Print: 7"}) != f.logs.end());
    const auto updateCamera = std::ranges::find(f.events, std::string{"updateCamera"});
    const auto unload = std::ranges::find(f.events, std::string{"unloadMap"});
    CHECK(updateCamera < unload);
    CHECK(!f.s->ui().hasMenu()); // screen objects go with the map
    // 120 fps: 8.33 ms frames give 8/8/9 ms steps (the remainder is carried).
    f.events.clear();
    for (int i = 0; i < 3; ++i) f.frame({}, 1.0 / 120);
    CHECK((std::ranges::count(f.events, std::string{"beginFrame 8"}) == 2 && std::ranges::count(f.events, std::string{"beginFrame 9"}) == 1));
}

void testCellsAcrossLoad() {
    Fixture f;
    f.start(R"(
int x; int y=5; string s; int arr[3]={1}; int last;
main() { x=42; y=7; s="z"; arr[1]=11; arr[2]=12; last=99; Load("maps\\b.map"); }
)",
            R"(
int x; int y=9; int s; int arr[3]={4,5}; int last; int extra;
main() {}
)");
    f.frame();
    CHECK(f.s->currentMap() == "maps\\b.map");
    CHECK_EQ(f.g("x"), 42);       // same cell, no initializer: kept
    CHECK_EQ(f.g("y"), 9);        // initializer applied at load
    CHECK_EQ(f.g("s"), 0);        // the cell held a string
    CHECK_EQ(f.g("arr", 0), 4);   // covered by the initializer
    CHECK_EQ(f.g("arr", 1), 5);
    CHECK_EQ(f.g("arr", 2), 12);  // not covered: kept
    CHECK_EQ(f.g("last"), 99);
    // `extra` is not checked: cells carry over by index including the VM stack leftovers of
    // the previous script (docs/script-language.md), so a new uninitialised cell is not 0.
}

void testBuiltins() {
    Fixture f;
    f.start(R"(
int t, len, k, bits, m, mv, md, ms, h, h2, cr, q, w, e, nk;
string line, line2, name;
main() {
  iff(1) {
    t = PrintText(10, 20, "ABC", 127);
    len = strlen("hello");
    Action(t, 32, 1, 0);
    m = MenuCreate(1, 2, 100, 300);
    LoadMenu("maps\\test.men");
    cr = FCreate("prof");
    FWrite(cr, "NAME"); FWrite(cr, 42); FClose(cr);
    h = FOpen("prof"); line = FRead(h); line2 = FRead(h); FClose(h);
    h2 = FOpen("nothing");
    q = GetNetInfo(6); w = GetNetInfo(1);
    ScrollLock(1);
    return;
  }
  k = GetKey(); bits = GetInputState(); nk = GetNetKey(0);
  mv = MenuVid(); md = MenuDir(); ms = MenuSelectVid();
  if (k == 27) { DeleteText(t); t = 0; FreeMenu(); }
  if (k == 'x') e = PrintFile(0, 0, "maps\\note.txt", 127);
}
)");
    writeFile(f.game / "maps" / "NOTE.TXT", "NOTE\r\n");
    f.frame();
    CHECK(isTextHandle(f.g("t")));
    CHECK_EQ(f.s->ui().text(f.g("t"))->visible, std::size_t{1});
    CHECK_EQ(f.g("len"), 5);
    CHECK(isSessionHandle(f.g("m")));
    CHECK_EQ(f.s->ui().find(f.g("m"))->dirIndex, 2);
    CHECK_EQ(f.s->ui().menuList().size(), std::size_t{4});
    CHECK_EQ(readFile(f.save / "prof"), std::string{"NAME\r\n42\r\n"});
    CHECK_EQ(f.s->interpreter()->get("line").toString(), std::string{"NAME"});
    CHECK_EQ(f.s->interpreter()->get("line2").toString(), std::string{"42"});
    CHECK_EQ(f.g("h2"), 0);
    CHECK_EQ(f.g("q"), 0);
    CHECK_EQ(f.g("w"), 0);
    CHECK(f.s->scrollLocked() && f.engine->scrollLock);
    // Input of the frame: a right click and Enter; the menu reports the hover of last frame.
    input::PcState pc;
    pc.cursor = {500, 300};
    pc.right = {true, true, false};
    pc.keys[static_cast<std::size_t>(input::Key::Enter)] = {true, true, false};
    f.frame(pc);
    CHECK_EQ(f.g("k"), 13);
    CHECK_EQ(f.g("nk"), 13);
    CHECK_EQ(f.g("bits"), OriginalInput::RClick | OriginalInput::RDown);
    pc = {};
    pc.cursor = {500, 300};
    f.frame(pc);
    CHECK_EQ(f.g("k"), 0);
    CHECK_EQ(f.g("ms"), 1);    // hovered in the previous frame's menu update
    CHECK_EQ(f.g("md"), 1);    // test.men: direction byte 64 of 4 directions
    CHECK_EQ(f.g("mv"), 0);
    pc.left = {true, true, false};
    f.frame(pc);
    pc.left = {false, false, true};
    f.frame(pc);
    CHECK_EQ(f.g("mv"), 1);
    pc = {};
    pc.text = "x";
    f.frame(pc);
    CHECK(isTextHandle(f.g("e")));
    pc.text.clear();
    pc.keys[static_cast<std::size_t>(input::Key::Escape)] = {true, true, false};
    f.frame(pc);
    CHECK_EQ(f.s->ui().text(f.s->globalInt("t")), nullptr);
    CHECK(!f.s->ui().hasMenu());
}

void testContextChangeFilter() {
    Fixture f;
    f.start(R"(
int k, reset_name;
main() { k = GetKey(); }
)");
    // Not player.map: context follows the menu list only.
    CHECK_EQ(f.s->inputContext(), input::Context::Gameplay);
    input::PcState pc;
    pc.keys[static_cast<std::size_t>(input::Key::Enter)] = {true, true, false};
    f.frame(pc);
    CHECK_EQ(f.g("k"), 13);
}

void testDemoPlayback() {
    Fixture f;
    // A demo of maps\a.map recorded with DemoRecorder (same format as the shipped ones).
    DemoRecorder rec{"maps\\a.map", 0x1234, 4000000, std::vector<std::int32_t>{5, 6}};
    OriginalInput in;
    in.key = 'q';
    in.mouseScreenX = 10;
    rec.tick(4000030, in);
    rec.fread("LINE1");
    rec.fread("LINE2");
    in.key = 0;
    in.bits = OriginalInput::LDown;
    rec.tick(4000065, in);
    rec.tick(4000100, in);
    auto bytes = rec.finish();
    const std::string trailer = "maps\\b.map";
    bytes.insert(bytes.end(), trailer.begin(), trailer.end());
    writeFile(f.game / "maps" / "demo1.dem", std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()});
    const auto parsed = demo::parseDemo(bytes);
    CHECK(parsed.has_value());
    if (parsed) {
        CHECK_EQ(parsed->ticks.size(), std::size_t{3});
        CHECK_EQ(parsed->freads.size(), std::size_t{2});
        CHECK_EQ(parsed->nextMap, trailer);
        CHECK_EQ(parsed->intGlobals.size(), std::size_t{2});
    }
    f.start(R"(
int a, b, k, h, bits;
string l1, l2;
main() { k = GetKey(); bits = GetInputState(); iff(1) { h = FOpen("anything"); l1 = FRead(h); l2 = FRead(h); } }
)");
    f.s->playDemo(1);
    CHECK(f.s->demoPlaying());
    CHECK_EQ(f.g("a"), 5); // recorded int cells
    CHECK_EQ(f.g("b"), 6);
    CHECK_EQ(f.s->viewWidth(), 640);
    CHECK(f.events.back().starts_with("loadMap a.map 4660"));
    // 30 ms recorded step: nothing before 30 ms of real time.
    f.frame({}, 0.020);
    CHECK_EQ(f.s->ticks(), std::uint64_t{0});
    f.frame({}, 0.020);
    CHECK_EQ(f.s->ticks(), std::uint64_t{1});
    CHECK_EQ(f.g("k"), 'q');
    CHECK_EQ(f.g("h"), 0);
    CHECK_EQ(f.s->interpreter()->get("l1").toString(), std::string{"LINE1"});
    CHECK_EQ(f.s->interpreter()->get("l2").toString(), std::string{"LINE2"});
    CHECK_EQ(f.engine->inputs.back().mouseScreenX, 10);
    CHECK_EQ(f.engine->clock(), 4000030);
    f.frame({}, 0.040);
    CHECK_EQ(f.g("bits"), OriginalInput::LDown);
    CHECK_EQ(f.engine->clock(), 4000065);
    f.frame({}, 0.040);
    f.frame({}, 0.040); // end of the records: the trailer map
    CHECK(!f.s->demoPlaying());
    CHECK(f.s->currentMap() == "maps\\b.map");
    // A real key press interrupts a demo.
    f.s->playDemo(1);
    input::PcState pc;
    pc.keys[static_cast<std::size_t>(input::Key::Enter)] = {true, true, false};
    f.frame(pc);
    CHECK(!f.s->demoPlaying());
    CHECK(f.s->currentMap() == "maps\\b.map");
}

void testDemoRecording() {
    Fixture f;
    f.start(R"(
int n, v=3;
string l;
main() { n++; if (n==1) { SaveDemo("rec.dem"); Load("maps\\b.map"); } }
)",
            R"(
int n, v=3, h;
string l;
main() { n++; iff(1) { h = FOpen("fw.ini"); l = FRead(h); FClose(h); } if (n==4) Load("maps\\a.map"); }
)");
    writeFile(f.save / "fw.ini", "1\r\n");
    f.frame();
    CHECK(f.s->currentMap() == "maps\\b.map");
    for (int i = 0; i < 3; ++i) f.frame();
    CHECK(f.s->currentMap() == "maps\\a.map");
    const auto data = readFile(f.save / "rec.dem");
    const std::vector<std::uint8_t> bytes{data.begin(), data.end()};
    const auto d = demo::parseDemo(bytes, 3);
    CHECK(d.has_value());
    if (d) {
        CHECK_EQ(d->mapName, std::string{"maps\\b.map"});
        CHECK_EQ(d->startClock, 4000000u);
        CHECK_EQ(d->intGlobals.size(), std::size_t{3});
        CHECK_EQ(d->intGlobals[0], 1u); // n carried from a.ini's cell
        CHECK_EQ(d->ticks.size(), std::size_t{3});
        CHECK_EQ(d->freads.size(), std::size_t{1});
        CHECK_EQ(d->freads[0].text, std::string{"1"});
    }
}

void testExitAndGraphMode() {
    Fixture f;
    f.start(R"(
int n;
main() { n++; if (n==1) SetGraphMode(2); if (n==2) Exit(); }
)");
    f.frame();
    CHECK_EQ(f.s->scriptScreen(), (std::pair{800, 600}));
    CHECK_EQ(f.s->menuOrigin(), (std::pair{-80, -60}));
    f.s->setOutputSize(854, 480);
    CHECK_EQ(f.s->menuOrigin(), (std::pair{27, -60}));
    f.frame();
    CHECK(f.s->exitRequested());
}

} // namespace

int main() {
    testInputRecord();
    testSaveFiles();
    testMenuList();
    testTexts();
    testFrameOrder();
    testCellsAcrossLoad();
    testBuiltins();
    testContextChangeFilter();
    testDemoPlayback();
    testDemoRecording();
    testExitAndGraphMode();
    return gromada_test::finish();
}
