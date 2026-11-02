// Controls on the original data (issue #19, needs GROMADA_DATA, skipped in CI): every weapon
// head of the Kassandra is classified from its fire chain as agreed in the issue (laser,
// cannons, machine gun and flamethrower direct; HAIL ballistic; the launchers missiles; PLUTO
// and CHAOS mine layers, CHAOS with the fixed head), and the ranges the level scripts set.
#include "game/engine.hpp"
#include "game/session_controls.hpp"
#include "test.hpp"

using namespace gromada;
using namespace gromada::game;

int main() {
    SKIP_WITHOUT_DATA();
    const auto data = *gromada_test::dataDir();
    GameResources res{data / "fw.res"};
    GameEngine e{res};
    e.loadMap(data / "maps" / "levels" / "01.map", 1);
    World& w = e.world();
    using K = input::WeaponKind;
    struct Head { int nvid; K kind; int weapon; std::uint8_t flags, category; };
    static constexpr Head heads[] = {
        {88, K::Direct, 18, 0, 0x04},          // HEPHEST laser
        {89, K::Direct, 12, 0, 0x04},          // PHOEBUS cannon
        {90, K::Missile, 14, 1, 0x08},         // ZEPHYR missiles
        {91, K::Ballistic, 16, 0, 0x04},       // HAIL ballistic cannon
        {202, K::Direct, 28, 0, 0x04},         // HELIOS laser
        {203, K::Direct, 23, 0, 0x0c},         // THUNDER anti-aircraft cannon
        {204, K::Missile, 27, 1, 0x0c},        // HARPY homing missiles (AviaMissile)
        {205, K::FixedMineLayer, 24, 5, 0x04}, // CHAOS mine layer, fixed head
        {207, K::Direct, 26, 0, 0x04},         // PHOENIX flamethrower
        {208, K::Direct, 25, 0, 0x04},         // VULCAN machine gun
        {209, K::Missile, 15, 1, 0x0c},        // HYDRA missiles
        {210, K::MineLayer, 16, 0, 0x04},      // PLUTO ballistic mine
    };
    for (const Head& h : heads) {
        const PlayerAim::FireChain c = fireChain(w, h.nvid);
        const K k = weaponKind(c);
        std::fprintf(stderr, "head %3d weapon %2d flags %d category %02x classes %05x projectile %d (%d,%d): kind %d\n", h.nvid,
                     w.vid(h.nvid).weapon, c.flags, c.targetCategory, c.classes, c.projectileClass, c.projectileSpeedX,
                     c.projectileSpeedY, static_cast<int>(k));
        CHECK(k == h.kind);
        CHECK_EQ(static_cast<int>(w.vid(h.nvid).weapon), h.weapon);
        CHECK_EQ(c.flags, h.flags);
        CHECK_EQ(c.targetCategory, h.category);
    }
    CHECK(weaponKind(fireChain(w, -1)) == K::Unknown);
    return gromada_test::finish();
}
