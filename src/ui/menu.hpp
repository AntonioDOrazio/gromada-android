#pragma once
// Menu screens: .men files and the menu object list behind the script builtins
//   LoadMenu(filename)            (71)  append the objects of a .men file
//   FreeMenu()                    (72)  remove every menu object
//   MenuCreate(nvid, dir, x, y)   (102) add one object, returns its unique number
//   MenuAction(nvid, dir, action) (101) change the action of the matching objects
//
// .men format (all 14 files of the game, see docs/ui.md):
//   u32 header (always 0)
//   16 x layer: { u16 nvid, s16 x, s16 y, s16 z, u8 direction, u8 action, payload } ... u16 0xFFFF
// i.e. the object records of a map "Objects" section, map version 0 (payload by class: 1 byte hp
// for material objects, hp + behave for asset objects, nothing for images/fonts), bucketed by
// layer: list k holds the objects whose vid has z_layer k (verified on every file), which is also
// the drawing order. Coordinates are screen pixels of the object's centre (640x480 files and
// *8.men 800x600 variants). Menu objects created by scripts go to their vid's z_layer.
//
// Header-only; depends on libgromada and ui/lens.hpp (graphics format 7).

#include "core/std.hpp"
#include "libgromada/libgromada_all.hpp"
#include "ui/lens.hpp"

namespace gromada::ui {

// Vid numbers used by the menu scripts (maps/HEADER.INI #defines).
namespace vids {
inline constexpr int Font = 127;            // V_FONT
inline constexpr int TextMenu = 26;         // V_TEXTMENU: prerendered menu labels, dir = menu state
inline constexpr int FourButton = 222;      // V_4BUTTON: light of the four round buttons
inline constexpr int Rectangle = 292;       // V_RECTANGLE: invisible hot spot of a text menu row
inline constexpr int Logo = 136;            // V_LOGO: main menu picture (mainmenu.ini)
inline constexpr int Credits = 137;         // V_CREDITS
inline constexpr int Screen = 85;           // V_SCREEN: opaque monitor background
inline constexpr int BackScreen = 84;       // console frame with a transparent monitor
inline constexpr int Lens = 134;            // format 7 lens over the monitor (gamemenu.men)
inline constexpr int BlackFrame800 = 83;    // 800x600 border
inline constexpr int ChoiceMap = 13;        // V_CHOICEMAP
inline constexpr int WinnedMap = 331;       // V_WINNEDMAP
inline constexpr int ClosedButton = 298;    // "close service centre / options" cover
inline constexpr int WeaponScreen = 296;
inline constexpr int Volume = 268;
inline constexpr int FontCursor = 130;
inline constexpr int YourLose = 86;         // V_YOURLOSE
inline constexpr int YourWin = 87;          // V_YOURWIN
} // namespace vids

inline constexpr int MenuLayerCount = 16;
inline constexpr int ActDestroy = 54; // act_destroy

struct MenuFileObject {
    std::uint16_t nvid = 0;
    std::int16_t x = 0, y = 0, z = 0;
    std::uint8_t direction = 0; // 0..255, like map objects
    std::uint8_t action = 0;
    std::uint8_t layer = 0;     // index of the list the record was found in
    std::optional<std::uint8_t> hp, behave; // class-dependent payload (unused by menus)
};

struct MenuFile {
    std::uint32_t header = 0;
    std::vector<MenuFileObject> objects; // in file order (layer by layer)
};

class MenuFormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Strict parser: throws MenuFormatError on truncation, unknown nvids/classes or trailing bytes.
inline MenuFile parseMenuFile(std::span<const std::byte> data, std::span<const Vid> vids) {
    std::size_t pos = 0;
    auto need = [&](std::size_t n, const char* what) {
        if (data.size() - pos < n)
            throw MenuFormatError(std::format("truncated .men file at byte {} ({})", pos, what));
    };
    auto u8 = [&] { return std::to_integer<std::uint8_t>(data[pos++]); };
    auto u16 = [&] { const auto lo = u8(); return static_cast<std::uint16_t>(lo | (u8() << 8)); };

    MenuFile file;
    need(4, "header");
    file.header = u16();
    file.header |= static_cast<std::uint32_t>(u16()) << 16;

    for (int layer = 0; layer < MenuLayerCount; ++layer) {
        for (;;) {
            need(2, "nvid");
            const std::uint16_t nvid = u16();
            if (nvid == 0xFFFF)
                break;
            if (nvid >= vids.size())
                throw MenuFormatError(std::format("nvid {} out of range at byte {}", nvid, pos - 2));
            need(8, "object record");
            MenuFileObject o;
            o.nvid = nvid;
            o.x = static_cast<std::int16_t>(u16());
            o.y = static_cast<std::int16_t>(u16());
            o.z = static_cast<std::int16_t>(u16());
            o.direction = u8();
            o.action = u8();
            o.layer = static_cast<std::uint8_t>(layer);
            GameObject::Payload payload;
            try {
                payload = getPayloadPrototype(vids[nvid].type);
            } catch (const std::exception&) {
                throw MenuFormatError(std::format("nvid {} has an unknown class", nvid));
            }
            if (std::holds_alternative<Payloads::AssetObject>(payload)) {
                need(2, "asset payload");
                o.hp = u8();
                o.behave = u8();
            } else if (std::holds_alternative<Payloads::MaterialObject>(payload)) {
                need(1, "material payload");
                o.hp = u8();
            }
            file.objects.push_back(o);
        }
    }
    if (pos != data.size())
        throw MenuFormatError(std::format("{} trailing bytes after the 16 layers", data.size() - pos));
    return file;
}

inline std::vector<std::byte> readBinaryFile(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    if (!file)
        throw std::runtime_error(std::format("cannot open {}", path.string()));
    std::vector<char> bytes{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    std::vector<std::byte> out(bytes.size());
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return out;
}

inline MenuFile loadMenuFile(const std::filesystem::path& path, std::span<const Vid> vids) {
    const auto bytes = readBinaryFile(path);
    try {
        return parseMenuFile(bytes, vids);
    } catch (const MenuFormatError& e) {
        throw MenuFormatError(std::format("{}: {}", path.string(), e.what()));
    }
}

struct MenuObject {
    int id = 0;            // unique number (MenuCreate's return value)
    std::uint16_t nvid = 0;
    int x = 0, y = 0, z = 0; // screen position of the centre
    int dirIndex = 0;      // direction index (MenuCreate's dir argument, MenuDir())
    int action = 0;        // act_menustanddown (0) ... act_menudisable (6)
    int layer = 0;         // drawing layer 0..15
};

class MenuScreen {
public:
    explicit MenuScreen(std::span<const Vid> vids) : m_vids{vids} {}

    // LoadMenu: appends the file's objects (FreeMenu is separate in the scripts).
    void load(const MenuFile& file) {
        for (const auto& o : file.objects) {
            const Vid& vid = m_vids[o.nvid];
            add(MenuObject{
                .nvid = o.nvid,
                .x = o.x, .y = o.y, .z = o.z,
                .dirIndex = vid.directionsCount ? getDirectionIndex(vid.directionsCount, o.direction) : 0,
                .action = o.action,
                .layer = o.layer,
            });
        }
    }

    // MenuCreate(nvid, dir, x, y): dir is a direction index (0 .. directionsCount-1).
    // Returns 0 for an invalid nvid.
    int create(int nvid, int dirIndex, int x, int y) {
        if (nvid < 0 || nvid >= static_cast<int>(m_vids.size()))
            return 0;
        const Vid& vid = m_vids[nvid];
        return add(MenuObject{
            .nvid = static_cast<std::uint16_t>(nvid),
            .x = x, .y = y,
            .dirIndex = dirIndex,
            .layer = std::min<int>(vid.z_layer, MenuLayerCount - 1),
        });
    }

    // MenuAction(nvid, dir, action): act_destroy (54) removes the objects. Returns how many
    // objects matched.
    int setAction(int nvid, int dirIndex, int action) {
        int count = 0;
        for (auto& o : m_objects)
            if (o.nvid == nvid && o.dirIndex == dirIndex) {
                o.action = action;
                ++count;
            }
        if (action == ActDestroy)
            std::erase_if(m_objects, [&](const MenuObject& o) { return o.action == ActDestroy; });
        return count;
    }

    bool destroy(int id) { return std::erase_if(m_objects, [&](const MenuObject& o) { return o.id == id; }) > 0; }

    // FreeMenu.
    void clear() { m_objects.clear(); }

    [[nodiscard]] std::span<const MenuObject> objects() const noexcept { return m_objects; }
    [[nodiscard]] std::span<const Vid> vids() const noexcept { return m_vids; }

    // Frame shown by an object in its current action and direction (first frame of the
    // animation; menus are drawn statically for now). nullopt when the vid has no such frame.
    [[nodiscard]] std::optional<std::size_t> frameOf(const MenuObject& o) const {
        const Vid& vid = m_vids[o.nvid];
        if (!std::holds_alternative<Vid::Graphics>(vid.graphicsData) || vid.directionsCount == 0)
            return std::nullopt;
        const int dir = std::clamp(o.dirIndex, 0, vid.directionsCount - 1);
        const auto act = static_cast<Action>(std::clamp(o.action, 0, 15));
        auto range = getAnimationFrameRangeDirIndex(vid, act, dir);
        if (!range)
            range = getAnimationFrameRangeDirIndex(vid, Action::act_stand, dir);
        if (!range)
            return std::nullopt;
        return range->first;
    }

    // Draws layer by layer (0..15, objects in insertion order); afterLayer(k) runs once each
    // layer is complete, so texts can be drawn in their font's layer.
    void draw(FramebufferRef fb, int originX = 0, int originY = 0,
              const std::function<void(int layer)>& afterLayer = {}) const {
        for (int layer = 0; layer < MenuLayerCount; ++layer) {
            for (const auto& o : m_objects) {
                if (o.layer != layer)
                    continue;
                const auto frame = frameOf(o);
                if (!frame)
                    continue;
                const auto& gfx = m_vids[o.nvid].graphics();
                const int left = originX + o.x - (gfx.width >> 1);
                const int top = originY + o.y - (gfx.height >> 1);
                if (gfx.dataFormat == 7)
                    applyLensFrame(gfx.frames[*frame], left, top, fb);
                else
                    DrawSprite(gfx.frames[*frame], left, top, fb);
            }
            if (afterLayer)
                afterLayer(layer);
        }
    }

private:
    int add(MenuObject o) {
        o.id = m_nextId++;
        m_objects.push_back(o);
        return o.id;
    }

    std::span<const Vid> m_vids;
    std::vector<MenuObject> m_objects;
    int m_nextId = 1;
};

} // namespace gromada::ui
