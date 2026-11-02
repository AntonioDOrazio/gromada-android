#pragma once
// Game session, screen objects: menu objects, on-screen texts and the status bar, i.e. the
// state behind the script builtins LoadMenu/FreeMenu/MenuCreate/MenuAction/MenuVid/MenuDir/
// MenuSelectVid/PrintText/PrintFile/DeleteText/StateBar/Message and Action() on their handles.
//
// In gromada.exe these are ordinary sprites fixed to the screen (state bit 4) living in the
// world's 16 layer lists; buttons (vid class 10, Image) are also put in the "menu list"
// (obj+0x970) that the per-frame menu update 0x40c1d0 scans. Reproduced here:
//  - menu list order (0x40d3f0): sorted by the vid's z_layer (vid+0x46), higher first; a new
//    object goes before the first one whose layer is <= its own (newest first among equals);
//  - menu update (0x40c1d0), once per frame after the world: selection, MenuDir, click
//    consumption (the input bits are zeroed), stand/select/press actions -- see updateMenu();
//  - hit test (0x40bec0): |mouse - centre| <= vid size/2 (sizeX, sizeY), inclusive;
//  - drawing: layer by layer (0..15); inside a layer by y, then z, then x, then creation
//    (the world lists' insertion order, docs/re/engine.md §9.2); texts are sprites of their
//    font's layer (14 for V_FONT);
//  - animations: one frame per vid frameDuration (screen objects run during Pause()); a vid
//    with a birth animation starts in act_birth and then stands; act_death removes the object
//    at the end of its animation; the action's sound plays once when the action starts.
// Handles: every object/text handle has bit 30 set (SessionHandleBase) so the session can
// recognise them when a script passes them to Action/GetX/GetY/GetUnitVid; engine unit
// handles must stay below it.
//
// Header-only; depends on libgromada, ui/ (fonts, text manager, .men parser, lens).

#include "core/std.hpp"
#include "libgromada/libgromada_all.hpp"
#include "ui/ui.hpp"

namespace gromada::game {

inline constexpr std::int32_t SessionHandleBase = 0x40000000;
inline constexpr std::int32_t MenuHandleBase = 0x40000000;
inline constexpr std::int32_t TextHandleBase = 0x48000000;
[[nodiscard]] constexpr bool isSessionHandle(std::int32_t h) noexcept { return h >= SessionHandleBase; }
[[nodiscard]] constexpr bool isTextHandle(std::int32_t h) noexcept { return h >= TextHandleBase; }

// Original action numbers used on screen objects (maps/HEADER.INI).
namespace act {
inline constexpr int Stand = 0, MenuStandUp = 1, MenuSelectDown = 2, MenuSelectUp = 3, MenuPressDown = 4,
                     MenuPressUp = 5, MenuDisable = 6, Birth = 14, Death = 15;
inline constexpr int SetTextCount = 32, ChangeAction = 37, Destroy = 54, SetText = 71, GetText = 72,
                     SetFile = 73, GetAction = 82;
} // namespace act

enum class UiList : std::uint8_t { Menu = 0, StateBar = 1 };

struct ScreenObject {
    int handle = 0;
    std::uint16_t nvid = 0;
    int x = 0, y = 0, z = 0;  // centre, screen pixels of the script's screen
    int dirIndex = 0;         // direction index (MenuCreate's dir, MenuDir())
    int action = 0;           // 0..15
    int layer = 0;
    UiList list = UiList::Menu;
    bool button = false;      // in the menu list (vid class 10)
    std::uint64_t seq = 0;    // creation order
    int frame = 0;            // frame inside the current animation
    float clockMs = 0;        // time towards the next frame
};

// Result of the last menu update (read by MenuVid/MenuDir/MenuSelectVid in the next frame).
struct MenuState {
    int selected = 0;       // nvid under the mouse (MenuSelectVid), or clicked
    int dir = 0;            // its direction index (MenuDir)
    bool click = false;     // MenuVid() = click ? selected : 0
    int selectedHandle = 0;
    [[nodiscard]] int menuVid() const noexcept { return click ? selected : 0; }
};

class SessionUi {
public:
    friend struct SaveStateAccess; // save states (game/savestate.hpp)
    explicit SessionUi(std::span<const Vid> vids)
        : m_vids{vids}, m_fonts{vids}, m_texts{m_fonts} {}

    SessionUi(const SessionUi&) = delete;
    SessionUi& operator=(const SessionUi&) = delete;

    [[nodiscard]] std::span<const Vid> vids() const noexcept { return m_vids; }
    [[nodiscard]] const ui::FontSet& fonts() const noexcept { return m_fonts; }

    // ---- objects ---------------------------------------------------------------------
    // MenuCreate(nvid, dir, x, y): 0 for an invalid nvid. Class-10 vids join the menu list.
    int create(int nvid, int dirIndex, int x, int y, UiList list = UiList::Menu, int z = 0) {
        if (!validVid(nvid))
            return 0;
        const Vid& v = m_vids[nvid];
        ScreenObject o;
        o.handle = MenuHandleBase + m_nextObject++;
        o.nvid = static_cast<std::uint16_t>(nvid);
        o.x = x;
        o.y = y;
        o.z = z;
        o.dirIndex = v.directionsCount ? std::clamp(dirIndex, 0, v.directionsCount - 1) : 0;
        o.action = v.animationLengths[act::Birth] ? act::Birth : act::Stand;
        o.layer = std::min<int>(v.z_layer, ui::MenuLayerCount - 1);
        o.list = list;
        o.button = v.type == ObjectClass::Image;
        o.seq = m_seq++;
        startAction(o);
        m_objects.push_back(o);
        if (o.button && list == UiList::Menu)
            insertMenu(o.handle, o.layer);
        return o.handle;
    }

    // LoadMenu: appends the objects of a .men file (direction bytes -> indices).
    void loadMenu(const ui::MenuFile& file, UiList list = UiList::Menu) {
        for (const auto& r : file.objects) {
            if (!validVid(r.nvid))
                continue;
            const Vid& v = m_vids[r.nvid];
            const int dir = v.directionsCount ? getDirectionIndex(v.directionsCount, r.direction) : 0;
            const int h = create(r.nvid, dir, r.x, r.y, list, r.z);
            if (h && r.action)
                changeAction(h, r.action);
        }
    }

    // FreeMenu (list Menu) / StateBar(0) (list StateBar): destroys the objects of the list.
    void freeList(UiList list) {
        std::erase_if(m_objects, [&](const ScreenObject& o) { return o.list == list; });
        if (list == UiList::Menu)
            m_menu.clear();
    }

    // Map unload: every screen object and text goes.
    void clear() {
        m_objects.clear();
        m_menu.clear();
        m_texts.clear();
        m_textSeq.clear();
        m_menuState = {};
        m_message.clear();
    }

    // MenuAction(nvid, dir, action): every menu-list object with that nvid and direction.
    int menuAction(int nvid, int dirIndex, int action) {
        int count = 0;
        const auto handles = m_menu; // act_destroy edits the list
        for (const int h : handles) {
            const ScreenObject* o = find(h);
            if (o && o->nvid == nvid && o->dirIndex == dirIndex) {
                objectAction(h, action, 0, 0);
                ++count;
            }
        }
        return count;
    }

    // Action(handle, act, var1, var2) on a screen object or a text. Returns the script value.
    int objectAction(int handle, int action, int var1, int var2) {
        if (isTextHandle(handle)) {
            const int t = handle - TextHandleBase;
            if (action == act::Destroy || action == act::Death) {
                deleteText(handle);
            } else if (action == act::SetTextCount) {
                m_texts.setVisibleCount(t, static_cast<std::size_t>(std::max(0, var1)));
            }
            return 0;
        }
        ScreenObject* o = findMutable(handle);
        if (!o)
            return 0;
        if (action <= 15) {
            changeAction(handle, action);
            return 0;
        }
        switch (action) {
        case act::Destroy: destroy(handle); return 0;
        case act::ChangeAction: { // var1 = action, var2 = direction byte
            const Vid& v = m_vids[o->nvid];
            const int dir = v.directionsCount ? getDirectionIndex(v.directionsCount, static_cast<std::uint8_t>(var2)) : 0;
            changeAction(handle, std::clamp(var1, 0, 15), dir);
            return 0;
        }
        case act::GetAction: return o->action;
        default: return 0;
        }
    }

    // ChangeAction(act, dir) of the original: a no-op when nothing changes.
    bool changeAction(int handle, int action, int dirIndex = -1) {
        ScreenObject* o = findMutable(handle);
        if (!o)
            return false;
        const Vid& v = m_vids[o->nvid];
        if (dirIndex < 0)
            dirIndex = o->dirIndex;
        dirIndex = v.directionsCount ? std::clamp(dirIndex, 0, v.directionsCount - 1) : 0;
        if (action == o->action && dirIndex == o->dirIndex)
            return true;
        o->action = std::clamp(action, 0, 15);
        o->dirIndex = dirIndex;
        startAction(*o);
        return true;
    }

    bool destroy(int handle) {
        std::erase(m_menu, handle);
        return std::erase_if(m_objects, [&](const ScreenObject& o) { return o.handle == handle; }) > 0;
    }

    [[nodiscard]] const ScreenObject* find(int handle) const {
        const auto it = std::ranges::find(m_objects, handle, &ScreenObject::handle);
        return it == m_objects.end() ? nullptr : &*it;
    }
    [[nodiscard]] std::span<const ScreenObject> objects() const noexcept { return m_objects; }
    [[nodiscard]] std::span<const int> menuList() const noexcept { return m_menu; }
    [[nodiscard]] bool hasMenu() const noexcept { return !m_menu.empty(); }

    // GetX/GetY/GetUnitVid on session handles.
    [[nodiscard]] std::optional<std::array<int, 3>> position(int handle) const {
        if (isTextHandle(handle)) {
            if (const auto* t = m_texts.find(handle - TextHandleBase))
                return std::array{t->x, t->y, t->font};
            return std::nullopt;
        }
        if (const auto* o = find(handle))
            return std::array{o->x, o->y, int{o->nvid}};
        return std::nullopt;
    }

    // ---- menu update (0x40c1d0) ----------------------------------------------------------
    // mouse: screen coordinates of the script's screen. `bits`: input bits of the frame; a
    // left click on a button is consumed (all bits zeroed). Returns true when it consumed it.
    bool updateMenu(int mouseX, int mouseY, std::uint16_t& bits) {
        m_menuState = {};
        const auto handles = m_menu;
        for (const int h : handles) {
            ScreenObject* o = findMutable(h);
            if (!o)
                continue;
            const Vid& v = m_vids[o->nvid];
            if ((static_cast<unsigned>(v.flags) & 0x20u) || o->action == act::MenuDisable || o->action == act::Death)
                continue;
            if (!hit(*o, mouseX, mouseY)) {
                changeAction(h, o->action & 1);
                continue;
            }
            if (o->action == act::Birth)
                continue;
            const int before = o->action;
            if (m_menuState.selected == 0 || (bits & 1u)) {
                m_menuState.selected = o->nvid;
                m_menuState.dir = o->dirIndex;
                m_menuState.selectedHandle = h;
                if (bits & 1u) {
                    m_menuState.click = true;
                    bits = 0;
                    changeAction(h, (before & 1) ? act::MenuPressUp : act::MenuPressDown);
                    return true;
                }
            }
            if (before == act::Stand)
                changeAction(h, act::MenuSelectDown);
            else if (before == act::MenuStandUp)
                changeAction(h, act::MenuSelectUp);
        }
        return false;
    }

    [[nodiscard]] const MenuState& menuState() const noexcept { return m_menuState; }
    void resetMenuState() { m_menuState = {}; }

    [[nodiscard]] bool hit(const ScreenObject& o, int mx, int my) const {
        const Vid& v = m_vids[o.nvid];
        const int hw = v.sizeX / 2, hh = v.sizeY / 2;
        return mx >= o.x - hw && mx <= o.x + hw && my >= o.y - hh && my <= o.y + hh;
    }

    // ---- texts -----------------------------------------------------------------------------
    // PrintText: 0 for an empty text.
    int print(int x, int y, std::string bytes, int font) {
        const int t = m_texts.print(x, y, std::move(bytes), font);
        if (!t)
            return 0;
        m_textSeq[t] = m_seq++;
        return TextHandleBase + t;
    }
    int printFile(int x, int y, const std::filesystem::path& file, int font) {
        const int t = m_texts.printFile(x, y, file, font);
        if (!t)
            return 0;
        m_textSeq[t] = m_seq++;
        return TextHandleBase + t;
    }
    bool deleteText(int handle) {
        if (!isTextHandle(handle))
            return false;
        m_textSeq.erase(handle - TextHandleBase);
        return m_texts.erase(handle - TextHandleBase);
    }
    [[nodiscard]] const ui::ScreenText* text(int handle) const {
        return isTextHandle(handle) ? m_texts.find(handle - TextHandleBase) : nullptr;
    }
    [[nodiscard]] const ui::TextManager& texts() const noexcept { return m_texts; }

    // Status bar text (Message): drawn on the status bar while it is shown.
    void setMessage(std::string bytes) { m_message = std::move(bytes); }
    [[nodiscard]] const std::string& message() const noexcept { return m_message; }

    // ---- time --------------------------------------------------------------------------------
    // Advances animations and typewriters by dt seconds; returns the sound effects requested
    // (fw.res 1-based indices, like PlaySFX).
    std::vector<int> update(float dt) {
        std::vector<int> sounds = std::exchange(m_sounds, {});
        const float ms = dt * 1000.0f;
        std::vector<int> dead;
        for (auto& o : m_objects) {
            const Vid& v = m_vids[o.nvid];
            const auto* g = std::get_if<Vid::Graphics>(&v.graphicsData);
            if (!g || !*g || (*g)->frameDuration == 0)
                continue;
            const float period = (*g)->frameDuration;
            o.clockMs += ms;
            while (o.clockMs >= period) {
                o.clockMs -= period;
                const int len = animationLength(o);
                if (++o.frame >= len) {
                    o.frame = 0;
                    if (o.action == act::Death) { dead.push_back(o.handle); break; }
                    if (o.action == act::Birth) { o.action = act::Stand; startAction(o); }
                }
            }
        }
        for (const int h : dead)
            destroy(h);
        m_texts.update(dt);
        for (const int s : m_texts.takeSounds())
            sounds.push_back(s);
        for (const int s : std::exchange(m_sounds, {}))
            sounds.push_back(s);
        return sounds;
    }

    // ---- drawing -----------------------------------------------------------------------------
    // Objects of `list` and (for the Menu list) the texts, layer by layer, at an origin.
    void draw(FramebufferRef fb, UiList list, int originX, int originY) const {
        struct Item { int layer, y, z, x; std::uint64_t seq; const ScreenObject* obj; const ui::ScreenText* text; };
        std::vector<Item> items;
        items.reserve(m_objects.size() + m_texts.texts().size());
        for (const auto& o : m_objects)
            if (o.list == list)
                items.push_back({o.layer, o.y, o.z, o.x, o.seq, &o, nullptr});
        if (list == UiList::Menu) {
            for (const auto& t : m_texts.texts()) {
                const Vid* v = m_fonts.vid(t.font);
                const auto it = m_textSeq.find(t.handle);
                items.push_back({v ? std::min<int>(v->z_layer, ui::MenuLayerCount - 1) : ui::MenuLayerCount - 1, t.y, 0, t.x,
                                 it == m_textSeq.end() ? 0 : it->second, nullptr, &t});
            }
        }
        std::ranges::sort(items, [](const Item& a, const Item& b) {
            return std::tie(a.layer, a.y, a.z, a.x, a.seq) < std::tie(b.layer, b.y, b.z, b.x, b.seq);
        });
        for (const auto& it : items) {
            if (it.text) {
                m_fonts.drawText(it.text->bytes, it.text->font, it.text->x + originX, it.text->y + originY, fb,
                                 {.maxBytes = it.text->visible});
                continue;
            }
            const ScreenObject& o = *it.obj;
            const Vid& v = m_vids[o.nvid];
            const auto* g = std::get_if<Vid::Graphics>(&v.graphicsData);
            if (!g || !*g)
                continue;
            const auto frame = frameIndex(o);
            if (!frame || *frame >= (*g)->frames.size())
                continue;
            const int left = originX + o.x - ((*g)->width >> 1);
            const int top = originY + o.y - ((*g)->height >> 1) - o.z;
            if ((*g)->dataFormat == 7)
                ui::applyLensFrame((*g)->frames[*frame], left, top, fb);
            else
                DrawSprite((*g)->frames[*frame], left, top, fb);
        }
        if (list == UiList::StateBar && !m_message.empty())
            m_fonts.drawText(m_message, ui::vids::Font, originX + MessageX, originY + MessageY, fb);
    }

    // Absolute frame shown by an object (nullopt when its vid has no frame for it).
    [[nodiscard]] std::optional<std::size_t> frameIndex(const ScreenObject& o) const {
        const Vid& v = m_vids[o.nvid];
        if (!std::holds_alternative<Vid::Graphics>(v.graphicsData) || v.directionsCount == 0)
            return std::nullopt;
        const int dir = std::clamp(o.dirIndex, 0, v.directionsCount - 1);
        auto range = getAnimationFrameRangeDirIndex(v, static_cast<Action>(o.action), dir);
        if (!range)
            range = getAnimationFrameRangeDirIndex(v, Action::act_stand, dir);
        if (!range)
            return std::nullopt;
        return std::min(range->first + static_cast<std::size_t>(std::max(o.frame, 0)), range->second);
    }

    // Position of the Message text on the status bar (hypothesis: left part of the bar).
    static constexpr int MessageX = 24, MessageY = 452;

private:
    [[nodiscard]] bool validVid(int nvid) const noexcept {
        return nvid >= 0 && nvid < static_cast<int>(m_vids.size());
    }

    ScreenObject* findMutable(int handle) {
        const auto it = std::ranges::find(m_objects, handle, &ScreenObject::handle);
        return it == m_objects.end() ? nullptr : &*it;
    }

    [[nodiscard]] int animationLength(const ScreenObject& o) const {
        const Vid& v = m_vids[o.nvid];
        const int a = v.animationLengths[static_cast<std::size_t>(o.action)] ? o.action : 0;
        return std::max<int>(1, v.animationLengths[static_cast<std::size_t>(a)]);
    }

    // SetAnimation: restart at the first frame; the action's sound plays once.
    void startAction(ScreenObject& o, bool sound = true) {
        o.frame = 0;
        o.clockMs = 0;
        const Vid& v = m_vids[o.nvid];
        if (sound && v.nsfx[static_cast<std::size_t>(o.action)])
            m_sounds.push_back(v.nsfx[static_cast<std::size_t>(o.action)]);
    }

    void insertMenu(int handle, int layer) {
        auto it = m_menu.begin();
        for (; it != m_menu.end(); ++it) {
            const ScreenObject* other = find(*it);
            if (other && other->layer <= layer)
                break;
        }
        m_menu.insert(it, handle);
    }

    std::span<const Vid> m_vids;
    ui::FontSet m_fonts;
    ui::TextManager m_texts;
    std::vector<ScreenObject> m_objects;
    std::vector<int> m_menu; // menu list (handles), in the original's order
    std::map<int, std::uint64_t> m_textSeq;
    MenuState m_menuState;
    std::vector<int> m_sounds;
    std::string m_message;
    int m_nextObject = 1;
    std::uint64_t m_seq = 0;
};

} // namespace gromada::game
