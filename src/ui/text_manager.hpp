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
    // game-clock typewriter (tick(), the original's STEXT sprite)
    bool started = false;        // lastTact set (the sprite ticks from the frame after its creation)
    std::uint32_t lastTact = 0;  // SPRITE+0x1e
    int timerMs = 0;             // SPRITE+0x33: 150 ms after a line break
    int action = 0;              // 0 stand, 1 character typed, 2 line break
    bool soundDone = true;       // state & 0x200: the action's sound has been played

    [[nodiscard]] bool typing() const noexcept { return visible < bytes.size(); }
};

// Typewriter timing for a font. Defaults are the values found in the exe/data for V_FONT; the
// per-character cadence is a hypothesis (one step per animation tick, i.e. the font vid's
// frameDuration of 71 ms), see docs/ui.md.
// A sound of a screen sprite (font or menu): nsfx and position on the script's screen; the
// original attenuates and pans it from the screen centre (0x409a20, branch state & 4).
struct ScreenSound {
    int nsfx = 0;
    int x = 0, y = 0;
};

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
    // Typewriter timings taken from the font vids (`fonts` must outlive the manager).
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

    // The original's typewriter (gromada.exe: the text is a screen sprite of class STEXT whose
    // vid is the font). It ticks like every sprite (0x409a20): once per frame at most, when
    // `clock - lastTact >= frameDuration`, never in the frame of its creation. A tick first
    // plays the sound of the current action if it has not been played yet (font nsfx[1] for a
    // character, nsfx[2] for a line break), counts the 150 ms line-break timer down by the
    // elapsed time, then runs act_nextcommand (0x40e01c): while the timer is 0 one reveal step,
    // which sets action 1 or 2 again (ChangeAction clears the "sound played" bit, so every step
    // is heard once, one tick later); at the end action 0. Use either tick() or update().
    void tick(std::uint32_t clock) {
        for (auto& t : m_texts) {
            if (!t.started) {
                t.started = true;
                t.lastTact = clock;
                continue;
            }
            const TypewriterTiming timing = m_timing ? m_timing(t.font) : TypewriterTiming{};
            const auto period = static_cast<std::uint32_t>(timing.stepSeconds * 1000.0f + 0.5f);
            const std::uint32_t dt = clock - t.lastTact;
            if (dt < period)
                continue;
            if (!t.soundDone) {
                t.soundDone = true;
                const int nsfx = t.action == 1 ? timing.charSfx : t.action == 2 ? timing.lineSfx : -1;
                if (nsfx >= 0)
                    m_screenSounds.push_back({nsfx, t.x, t.y});
            }
            t.timerMs = static_cast<std::uint32_t>(t.timerMs) < dt ? 0 : t.timerMs - static_cast<int>(dt);
            if (t.timerMs == 0) {
                if (t.typing()) {
                    const bool lineBreak = reveal(t);
                    t.action = lineBreak ? 2 : 1;
                    if (lineBreak)
                        t.timerMs = static_cast<int>(timing.lineBreakSeconds * 1000.0f + 0.5f);
                    t.soundDone = false;
                } else if (t.action != 0) {
                    t.action = 0;
                    t.soundDone = false;
                }
            }
            t.lastTact = clock;
        }
    }
    // Sounds of tick() with the position of their text.
    std::vector<ScreenSound> takeScreenSounds() { return std::exchange(m_screenSounds, {}); }

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
    // The reveal of one act_nextcommand of the STEXT object (gromada.exe 0x40e01c): one
    // character, or a run of white space with the next character; true when the run holds a
    // line break (then the next line's first character waits).
    static bool reveal(ScreenText& t) {
        const auto isSpace = [](char c) { return c == ' ' || (c >= '\t' && c <= '\r'); };
        const char c = t.bytes[t.visible++];
        if (!isSpace(c))
            return false;
        bool lineBreak = false;
        while (t.visible < t.bytes.size()) {
            const char n = t.bytes[t.visible++];
            lineBreak |= n == '\n';
            if (!isSpace(n))
                break;
        }
        if (lineBreak && t.visible < t.bytes.size())
            --t.visible;
        return lineBreak;
    }

    // update() step: reveal plus the sound and pause right away.
    void step(ScreenText& t, const TypewriterTiming& timing) {
        if (reveal(t)) {
            t.pause = timing.lineBreakSeconds;
            if (timing.lineSfx >= 0)
                m_sounds.push_back(timing.lineSfx);
            return;
        }
        if (timing.charSfx >= 0)
            m_sounds.push_back(timing.charSfx);
    }

    std::vector<ScreenText> m_texts;
    std::vector<int> m_sounds;
    std::vector<ScreenSound> m_screenSounds;
    TimingLookup m_timing;
    int m_nextHandle = 1;
};

} // namespace gromada::ui
