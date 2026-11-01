#pragma once
// On-screen texts with handles: the state behind the script builtins
//   PrintText(x, y, text, font)      -> handle  (116)
//   PrintFile(x, y, filename, font)  -> handle  (117)
//   DeleteText(handle)                          (118)
//   Action(handle, act_settextcount, n, 0)      (act_settextcount = 32: show the first n bytes)
//
// In gromada.exe a text is a screen-space sprite object (class "STEXT") whose vid is the font;
// it is drawn in the font's z layer (14 for V_FONT) together with the menu objects. Its text is
// a byte string (CP866) and `visible` bytes of it are drawn. Setting the visible count below the
// length starts the typewriter: one reveal step per tick, a whole run of white space is revealed
// together with the next character, and a run containing '\n' pauses 150 ms before the next line
// (the font's sfx 1/2 play for characters/line breaks). The level scripts use it for the
// briefing (HEADER.INI: Action(gamemenu, act_settextcount, 0, 0) after GameMenuOn()).
//
// Header-only. Drawing goes through FontSet (libgromada); the bookkeeping does not.

#include "core/std.hpp"
#include "ui/font.hpp"

namespace gromada::ui {

struct ScreenText {
    int handle = 0;
    int x = 0, y = 0;   // position passed to PrintText/PrintFile (screen pixels)
    int font = 0;       // nvid of the font
    std::string bytes;  // CP866 bytes, CR/LF kept as in the file
    std::size_t visible = 0; // bytes drawn (== bytes.size() unless typing)

    // typewriter state
    float pause = 0.0f;  // seconds left before the next reveal step
    float clock = 0.0f;  // time accumulated towards the next step

    [[nodiscard]] bool typing() const noexcept { return visible < bytes.size(); }
};

// Typewriter timing for a font. Defaults are the values found in the exe/data for V_FONT; the
// per-character cadence is a hypothesis (one step per animation tick, i.e. the font vid's
// frameDuration of 71 ms), see docs/ui.md.
struct TypewriterTiming {
    float stepSeconds = 0.071f;      // one reveal step
    float lineBreakSeconds = 0.150f; // exe: [obj+0x33] = 150 (ms countdown)
    int charSfx = -1;                // font vid nsfx[1] (23 for V_FONT)
    int lineSfx = -1;                // font vid nsfx[2] (24 for V_FONT)
};

inline TypewriterTiming typewriterTimingOf(const Vid& font) {
    TypewriterTiming t;
    if (const auto* g = std::get_if<Vid::Graphics>(&font.graphicsData); g && *g && (*g)->frameDuration > 0)
        t.stepSeconds = (*g)->frameDuration * 0.001f;
    t.charSfx = font.nsfx[1] ? font.nsfx[1] : -1;
    t.lineSfx = font.nsfx[2] ? font.nsfx[2] : -1;
    return t;
}

class TextManager {
public:
    using TimingLookup = std::function<TypewriterTiming(int font)>;

    TextManager() = default;
    explicit TextManager(TimingLookup timing) : m_timing{std::move(timing)} {}
    // Typewriter timings taken from the font vids.
    explicit TextManager(const FontSet& fonts)
        : m_timing{[&fonts](int font) {
              const Vid* v = fonts.vid(font);
              return v ? typewriterTimingOf(*v) : TypewriterTiming{};
          }} {}

    // PrintText: returns a handle > 0, or 0 for an empty text (like the exe).
    int print(int x, int y, std::string bytes, int font) {
        if (bytes.empty() || bytes.front() == '\0')
            return 0;
        // The exe copies a C string: anything after a NUL is not part of the text.
        if (const auto nul = bytes.find('\0'); nul != std::string::npos)
            bytes.resize(nul);
        ScreenText t;
        t.handle = m_nextHandle++;
        t.x = x;
        t.y = y;
        t.font = font;
        t.visible = bytes.size();
        t.bytes = std::move(bytes);
        m_texts.push_back(std::move(t));
        return m_texts.back().handle;
    }

    // PrintFile: the whole file as raw bytes ("rb"); 0 when it cannot be read or is empty.
    int printFile(int x, int y, const std::filesystem::path& path, int font) {
        std::ifstream file{path, std::ios::binary};
        if (!file)
            return 0;
        std::string bytes{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
        return print(x, y, std::move(bytes), font);
    }

    // DeleteText: false for 0 or unknown handles (scripts call DeleteText(0) freely).
    bool erase(int handle) {
        const auto it = std::ranges::find(m_texts, handle, &ScreenText::handle);
        if (handle == 0 || it == m_texts.end())
            return false;
        m_texts.erase(it);
        return true;
    }

    void clear() { m_texts.clear(); }

    // act_settextcount: shows the first `count` bytes; a smaller count than the length starts
    // the typewriter.
    bool setVisibleCount(int handle, std::size_t count) {
        ScreenText* t = find(handle);
        if (!t)
            return false;
        t->visible = std::min(count, t->bytes.size());
        t->pause = 0.0f;
        t->clock = 0.0f;
        return true;
    }

    void revealAll() {
        for (auto& t : m_texts)
            t.visible = t.bytes.size();
    }

    // Advances the typewriters by dt seconds.
    void update(float dt) {
        for (auto& t : m_texts) {
            if (!t.typing())
                continue;
            const TypewriterTiming timing = m_timing ? m_timing(t.font) : TypewriterTiming{};
            if (timing.stepSeconds <= 0.0f) {
                t.visible = t.bytes.size();
                continue;
            }
            t.clock += dt;
            while (t.clock >= timing.stepSeconds && t.typing()) {
                t.clock -= timing.stepSeconds;
                if (t.pause > 0.0f) {
                    t.pause -= timing.stepSeconds;
                    continue;
                }
                step(t, timing);
            }
        }
    }

    // Sound effects (fw.res sound indices) requested by the typewriters since the last call.
    std::vector<int> takeSounds() { return std::exchange(m_sounds, {}); }

    [[nodiscard]] ScreenText* find(int handle) {
        const auto it = std::ranges::find(m_texts, handle, &ScreenText::handle);
        return handle != 0 && it != m_texts.end() ? &*it : nullptr;
    }
    [[nodiscard]] const ScreenText* find(int handle) const { return const_cast<TextManager*>(this)->find(handle); }
    [[nodiscard]] std::span<const ScreenText> texts() const noexcept { return m_texts; }

    // Draws the texts in creation order; with `layer`, only those whose font vid lives in that
    // z layer (menus interleave texts with their layers, see MenuScreen::draw).
    void draw(const FontSet& fonts, FramebufferRef fb, int originX = 0, int originY = 0,
              std::optional<int> layer = std::nullopt) const {
        for (const auto& t : m_texts) {
            if (layer) {
                const Vid* v = fonts.vid(t.font);
                if (!v || v->z_layer != *layer)
                    continue;
            }
            fonts.drawText(t.bytes, t.font, t.x + originX, t.y + originY, fb, {.maxBytes = t.visible});
        }
    }

private:
    // One act_nextcommand of the STEXT object (gromada.exe 0x40e01c).
    void step(ScreenText& t, const TypewriterTiming& timing) {
        const auto isSpace = [](char c) { return c == ' ' || (c >= '\t' && c <= '\r'); };
        const char c = t.bytes[t.visible++];
        if (isSpace(c)) {
            bool lineBreak = false;
            while (t.visible < t.bytes.size()) {
                const char n = t.bytes[t.visible++];
                lineBreak |= n == '\n';
                if (!isSpace(n))
                    break;
            }
            if (lineBreak) {
                if (t.visible < t.bytes.size())
                    --t.visible; // the next line's first character waits for the pause
                t.pause = timing.lineBreakSeconds;
                if (timing.lineSfx >= 0)
                    m_sounds.push_back(timing.lineSfx);
                return;
            }
        }
        if (timing.charSfx >= 0)
            m_sounds.push_back(timing.charSfx);
    }

    std::vector<ScreenText> m_texts;
    std::vector<int> m_sounds;
    TimingLookup m_timing;
    int m_nextHandle = 1;
};

} // namespace gromada::ui
