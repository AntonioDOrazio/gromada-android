// Controls on the real gameplay (issue #16), synthetic: move orders and the edge band in the
// input record, the camera that follows the player (dead zone, look-ahead, 60 = 120 fps),
// weapon availability from the script globals, the weapon indicator; issue #19: the aim point
// (also off screen) in the input record, the weapon kind from the fire chain.
#include "game/session_controls.hpp"
#include "game/session_input.hpp"
#include "test.hpp"

#include <map>

using namespace gromada;
using namespace gromada::game;

namespace {

bool near(float a, float b, float eps) { return std::abs(a - b) <= eps; }

void testMoveOrderRecord() {
    InputRecordBuilder b;
    input::PcState pc;
    pc.cursor = {300.5f, 100.2f};        // aim point (drawn)
    pc.right = {true, false, false};     // fire held
    pc.moveOrder = true;
    pc.moveTarget = {420.7f, -30.0f};    // order ahead of the player, above the view
    pc.edgeScroll = false;
    // view at x = 10 in the output (as in a demo), camera at (1000, 2000)
    auto r = b.build(pc, 290, 100, 1000, 2000, 1.0f / 60, InputView{640, 480});
    CHECK_EQ(r.mouseWorldX, 1000 + 410); // the click goes to the order
    CHECK_EQ(r.mouseWorldY, 2000 - 30);
    CHECK_EQ(r.bits, OriginalInput::LClick | OriginalInput::LDown); // no right button: it would fire there
    CHECK_EQ(r.mouseScreenY, InputRecordBuilder::EdgeBand); // not in the edge-scroll band
    CHECK_EQ(r.mouseScreenX, 410);
    // Next frame: the aim point again, fire held.
    pc.moveOrder = false;
    r = b.build(pc, 290, 100, 1000, 2000, 1.0f / 60, InputView{640, 480});
    CHECK_EQ(r.mouseWorldX, 1290);
    CHECK_EQ(r.bits, OriginalInput::RDown);
    // Touch near the border: the click lands where the finger is, the view does not scroll.
    pc.cursor = {2, 478};
    r = b.build(pc, 2, 478, 0, 0, 1.0f / 60, InputView{640, 480});
    CHECK(r.mouseWorldX == 2 && r.mouseWorldY == 478);
    CHECK(r.mouseScreenX == 6 && r.mouseScreenY == 473);
    // The pad cursor does scroll (edge scroll of the original), and so does any cursor in menus.
    pc.edgeScroll = true;
    r = b.build(pc, 2, 478, 0, 0, 1.0f / 60, InputView{640, 480});
    CHECK(r.mouseScreenX == 2 && r.mouseScreenY == 478);
    pc.edgeScroll = false;
    r = b.build(pc, 2, 478, 0, 0, 1.0f / 60);
    CHECK(r.mouseScreenX == 2 && r.mouseScreenY == 478);
}

// Issue #19: the direct scheme's aim point is the record's mouse point, also off screen; the
// reticle (cursor) is only drawn. A move order still wins its frame.
void testAimTargetRecord() {
    InputRecordBuilder b;
    input::PcState pc;
    pc.cursor = {320, 2};                // reticle on the top border
    pc.hasAimTarget = true;
    pc.aimTarget = {330.5f, -90.2f};     // locked target 330 px up, off screen
    pc.right = {true, true, false};
    pc.edgeScroll = false;
    auto r = b.build(pc, 320, 2, 1000, 2000, 1.0f / 60, InputView{640, 480});
    CHECK(r.mouseWorldX == 1330 && r.mouseWorldY == 2000 - 91); // the turret aims and fires there
    CHECK(r.mouseScreenX == 330 && r.mouseScreenY == InputRecordBuilder::EdgeBand); // no edge scroll
    CHECK_EQ(r.bits, OriginalInput::RClick | OriginalInput::RDown);
    // View offset (16:9 menus centred, demo view): the same shift applies to the aim point.
    r = b.build(pc, 300, 2, 0, 0, 1.0f / 60, InputView{640, 480});
    CHECK(r.mouseWorldX == 310 && r.mouseWorldY == -91);
    // A move order wins the frame (left click at its point, no right button).
    pc.moveOrder = true;
    pc.moveTarget = {100, 200};
    r = b.build(pc, 320, 2, 0, 0, 1.0f / 60, InputView{640, 480});
    CHECK(r.mouseWorldX == 100 && r.mouseWorldY == 200 && r.bits == (OriginalInput::LClick | OriginalInput::LDown));
    // Without an aim point (cursor scheme): the cursor.
    pc.moveOrder = false;
    pc.hasAimTarget = false;
    r = b.build(pc, 320, 2, 0, 0, 1.0f / 60, InputView{640, 480});
    CHECK(r.mouseWorldX == 320 && r.mouseWorldY == 2);
}

// Issue #19: weapon kinds from the fire chain (the player heads' data, see weaponKind()).
void testWeaponKind() {
    using K = input::WeaponKind;
    auto chain = [](int head, std::uint8_t flags, std::initializer_list<int> classes, int pcls, int sx, int sy) {
        PlayerAim::FireChain c;
        c.headNvid = head;
        c.flags = flags;
        for (int k : classes) c.classes |= 1u << k;
        c.projectileClass = pcls;
        c.projectileSpeedX = sx;
        c.projectileSpeedY = sy;
        return c;
    };
    CHECK(weaponKind({}) == K::Unknown);
    CHECK(weaponKind(chain(88, 0, {12, 5}, 5, 280, 100)) == K::Direct);   // HEPHEST laser
    CHECK(weaponKind(chain(89, 0, {12, 5}, 5, 9999, 80)) == K::Direct);   // PHOEBUS cannon
    CHECK(weaponKind(chain(207, 0, {5}, 5, 280, 100)) == K::Direct);      // PHOENIX flamethrower
    CHECK(weaponKind(chain(91, 0, {12, 5, 11}, 5, 200, 400)) == K::Ballistic); // HAIL
    CHECK(weaponKind(chain(90, 1, {12, 6, 5}, 6, 400, 200)) == K::Missile);    // ZEPHYR
    CHECK(weaponKind(chain(204, 1, {12, 15, 5}, 15, 300, 80)) == K::Missile);  // HARPY (homing)
    CHECK(weaponKind(chain(209, 1, {12, 11}, -1, 0, 0)) == K::Missile);        // HYDRA
    CHECK(weaponKind(chain(210, 0, {12, 5, 13}, 5, 200, 400)) == K::MineLayer); // PLUTO
    CHECK(weaponKind(chain(205, 5, {13}, 13, 30, 0)) == K::FixedMineLayer);     // CHAOS
    CHECK(weaponKind(chain(1, 0, {12}, -1, 0, 0)) == K::Unknown);               // nothing flies
    CHECK(!input::magneticWeapon(K::Direct) && !input::magneticWeapon(K::Unknown));
    CHECK(input::magneticWeapon(K::Ballistic) && input::magneticWeapon(K::Missile) &&
          input::magneticWeapon(K::MineLayer) && input::magneticWeapon(K::FixedMineLayer));
}

// Runs the follower for `seconds` with the player moving at (vx, vy) px/s from (px, py).
Camera follow(int fps, float seconds, float px, float py, float vx, float vy, Camera cam = {0, 0, 640, 480}) {
    FollowCamera f;
    const int frames = static_cast<int>(std::lround(seconds * fps));
    for (int i = 0; i < frames; ++i) {
        const float t = static_cast<float>(i) / fps;
        f.update(cam, static_cast<int>(std::lround(px + vx * t)), static_cast<int>(std::lround(py + vy * t)), 1.0f / fps, false);
    }
    return cam;
}

void testFollowCamera() {
    // Inside the dead zone (10 % of the view each side of the centre): nothing moves.
    Camera c = follow(60, 1, 320 + 50, 240 - 40, 0, 0);
    CHECK(c.x == 0 && c.y == 0);
    // Outside: the view catches up to the dead zone border.
    c = follow(60, 3, 320 + 200, 240 + 100, 0, 0);
    CHECK(near(static_cast<float>(c.x), 200 - 64, 2) && near(static_cast<float>(c.y), 100 - 48, 2));
    // Moving right at 100 px/s for 4 s: the player ends up ahead of the centre (look-ahead),
    // within the view, and the same at 60 and 120 fps.
    const Camera a = follow(60, 4, 320, 240, 100, 0), b = follow(120, 4, 320, 240, 100, 0);
    const float playerX = 320 + 400 - static_cast<float>(a.x);
    CHECK(playerX > 230 && playerX < 290); // the view leads: the player is ~60 px left of the centre
    CHECK(near(static_cast<float>(a.x), static_cast<float>(b.x), 3) && a.y == b.y);
    // 16:9: the same rule on the wider view.
    const Camera w = follow(60, 3, 427 + 300, 240, 0, 0, {0, 0, 854, 480});
    CHECK(near(static_cast<float>(w.x), 300 - 85, 2));
    // Someone else moved the view (SetPosition, the engine's clamp): taken as the new position.
    FollowCamera f;
    Camera cam{0, 0, 640, 480};
    f.update(cam, 320, 240, 1.0f / 60, false);
    cam.x = 500;
    f.update(cam, 320, 240, 1.0f / 60, false);
    CHECK(cam.x < 500 && cam.x > 400);
    // Scrolling with the cursor at the edge pauses the follower until the vehicle moves.
    FollowCamera g;
    Camera v{0, 0, 640, 480};
    g.update(v, 320, 240, 1.0f / 60, true);
    v.x = 300; // the engine scrolled
    for (int i = 0; i < 60; ++i) g.update(v, 320, 240, 1.0f / 60, false);
    CHECK(v.x == 300 && g.holding());
    for (int i = 0; i < 60; ++i) g.update(v, 320 + i * 2, 240, 1.0f / 60, false); // 120 px/s
    CHECK(!g.holding() && v.x < 300);
}

void testWeaponSlots() {
    std::map<std::pair<std::string, int>, int> globals{
        {{"nweapon", 0}, 2},
        {{"weapon", 0}, 88}, {{"weapon", 1}, 90}, {{"weapon", 2}, 204}, {{"weapon", 3}, 0}};
    std::map<int, int> ammo{{2, 5}, {2 + 100, 0}};
    auto g = [&](std::string_view n, int i) { return globals[{std::string{n}, i}]; };
    std::vector<int> asked;
    auto a = [&](int index) { asked.push_back(index); return index == 2 ? ammo[2] : 0; };
    WeaponSlots w = weaponSlots(g, a);
    CHECK_EQ(w.current, 2);
    // slot 1: head 90 = ammunition 2 (5 left); slot 2: head 204 = HEAD12 + 2: index 2 too;
    // slot 0 always (the script never takes it away); slot 3 empty.
    CHECK_EQ(w.mask, 0b0111);
    ammo[2] = 0;
    w = weaponSlots(g, a);
    CHECK_EQ(w.mask, 0b0001);
    CHECK(w.nvid[1] == 90 && w.nvid[3] == 0);
    globals[{"weapon", 3}] = 209; // HEAD13 + 2
    w = weaponSlots(g, a);
    CHECK(asked.back() == 2 && w.mask == 0b0001);
}

void testWeaponHud() {
    CHECK(WeaponHud::label(1, std::string_view{"ZEPHYR   : LANCIAMISSILI\0\0\0", 27}, 12, false) ==
          "2 ZEPHYR: LANCIAMISSILI  12");
    CHECK(WeaponHud::label(0, "HEPHEST  : CANNONE LASER", -1, true) == "1 HEPHEST: CANNONE LASER ...");
    WeaponHud h;
    CHECK(!h.update(0, 0, 0.1f)); // level start: nothing to show
    CHECK(h.update(0, 1, 0.1f));  // requested: shown
    CHECK(h.update(1, 1, 1.0f));  // switched: shown again for 2 s
    CHECK(h.update(1, 1, 0.9f));
    CHECK(!h.update(1, 1, 0.2f));
    CHECK(!h.update(-1, -1, 0.1f));
    CHECK(!h.update(0, 0, 0.1f));
    CHECK(h.update(0, 0, 0.1f, true)); // key pressed, nothing changes: shown anyway
}

} // namespace

int main() {
    testMoveOrderRecord();
    testAimTargetRecord();
    testWeaponKind();
    testFollowCamera();
    testWeaponSlots();
    testWeaponHud();
    return gromada_test::finish();
}
