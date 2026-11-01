#pragma once
// Fonts of fw.res and glyph drawing.
//
// A font is an ordinary vid of class Font (19). fw.res of the Italian edition has exactly one:
// nvid 127 "Font" (V_FONT in maps/HEADER.INI): 256 frames of 7x8 pixels in format 2 (RLE
// with its own palette), one per byte value laid out like the VGA ROM font with CP866
// Cyrillic capitals at 0x80..0x9F; cell sizeX x sizeY = 7 x 10. The glyph of byte c is
// frame c (not the direction mapping used by sprites). See docs/ui.md.
//
// Header-only. Uses libgromada (must be included once per binary, like the rest of the core).

#include "core/std.hpp"
#include "libgromada/libgromada_all.hpp"
#include "ui/text_layout.hpp"

namespace gromada::ui {

inline FontMetrics metricsOf(const Vid& vid) {
    const auto& gfx = vid.graphics();
    FontMetrics m;
    m.glyphWidth = gfx.width;
    m.glyphHeight = gfx.height;
    m.advance = vid.sizeX ? vid.sizeX : gfx.width;
    m.lineHeight = vid.sizeY ? vid.sizeY : gfx.height;
    return m;
}

// Holds the vids the text renderer may draw with. Any vid with graphics can act as a font
// after an ESC switch (the exe does not check the class there); the class check only applies
// to the font a text is created with (see isFont).
class FontSet {
public:
    FontSet() = default;
    explicit FontSet(std::span<const Vid> vids) : m_vids{vids} {}

    [[nodiscard]] std::span<const Vid> vids() const noexcept { return m_vids; }

    [[nodiscard]] const Vid* vid(int nvid) const noexcept {
        if (nvid < 0 || nvid >= static_cast<int>(m_vids.size()))
            return nullptr;
        const Vid& v = m_vids[nvid];
        const auto* g = std::get_if<Vid::Graphics>(&v.graphicsData);
        return g && *g ? &v : nullptr;
    }

    [[nodiscard]] bool isFont(int nvid) const noexcept {
        const Vid* v = vid(nvid);
        return v && v->type == ObjectClass::Font;
    }

    [[nodiscard]] std::optional<FontMetrics> metrics(int nvid) const {
        if (const Vid* v = vid(nvid))
            return metricsOf(*v);
        return std::nullopt;
    }

    [[nodiscard]] FontMetricsLookup lookup() const {
        return [this](int nvid) { return metrics(nvid); };
    }

    // nvids of all the vids of class Font.
    [[nodiscard]] std::vector<int> fontNvids() const {
        std::vector<int> result;
        for (int i = 0; i < static_cast<int>(m_vids.size()); ++i)
            if (m_vids[i].type == ObjectClass::Font)
                result.push_back(i);
        return result;
    }

    // True when the font has a non-empty glyph for this byte.
    [[nodiscard]] bool hasGlyph(int nvid, std::uint8_t code) const {
        const Vid* v = vid(nvid);
        if (!v || code >= v->graphics().frames.size())
            return false;
        const auto& frame = v->graphics().frames[code];
        if (frame.data.size() < 4)
            return false;
        if (v->graphics().dataFormat == 0)
            return true;
        // RLE formats start with (u16 first row, u16 row count).
        auto reader = frame.read();
        reader.skip(2);
        return reader.read<std::uint16_t>() > 0;
    }

    void drawGlyph(int nvid, std::uint8_t code, int x, int y, FramebufferRef fb) const {
        const Vid* v = vid(nvid);
        if (!v || code >= v->graphics().frames.size())
            return;
        DrawSprite(v->graphics().frames[code], x, y, fb);
    }

    // Draws `text` printed at (x, y) like the exe does (see text_layout.hpp).
    void drawText(std::string_view text, int font, int x, int y, FramebufferRef fb,
                  const LayoutOptions& options = {}) const {
        for (const auto& g : layoutText(text, font, x, y, lookup(), options))
            drawGlyph(g.font, g.code, g.x, g.y, fb);
    }

private:
    std::span<const Vid> m_vids;
};

} // namespace gromada::ui
