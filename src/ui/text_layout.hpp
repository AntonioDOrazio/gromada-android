#pragma once
// Bitmap text layout, measurement, wrapping and alignment (pure logic, no game data).
//
// Reproduces the text drawing loop of gromada.exe (STEXT objects, see docs/ui.md):
//  - the glyph of byte c is frame c of the font vid, drawn with its top-left corner at the pen;
//  - the pen starts at (x + sizeX/2 - width/2, y + sizeY/2 - height/2) of the text's font
//    (vid sizeX/sizeY = cell, graphics width/height = glyph box): for the 7x8 font with a
//    7x10 cell the first glyph lands at (x, y + 1);
//  - every byte advances the pen by the CURRENT font's sizeX, control bytes included;
//  - '\r' moves the pen back to the line start, '\n' does the same and moves down by the
//    current font's sizeY;
//  - ESC (0x1B) followed by a byte switches to the font whose nvid is that byte (the pair
//    takes one cell); a trailing ESC is ignored;
//  - other bytes < 0x20 draw nothing.
// There is no automatic wrapping or alignment in the original: those are extensions here
// (wrapText, LayoutOptions::align) for text produced by the core itself.

#include "core/std.hpp"

namespace gromada::ui {

inline constexpr char Esc = 0x1B;

// Cell metrics of one font (a vid of class Font).
struct FontMetrics {
    int advance = 7;      // vid.sizeX: horizontal pen advance per byte
    int lineHeight = 10;  // vid.sizeY: vertical advance per '\n'
    int glyphWidth = 7;   // graphics width
    int glyphHeight = 8;  // graphics height

    // Offset from the text position to the first glyph's top-left corner (exe: sar by 1).
    [[nodiscard]] constexpr int originX() const noexcept { return (advance >> 1) - (glyphWidth >> 1); }
    [[nodiscard]] constexpr int originY() const noexcept { return (lineHeight >> 1) - (glyphHeight >> 1); }
};

// Resolves the metrics of a font nvid (ESC sequences may name any nvid). nullopt = unknown:
// glyphs of an unknown font are skipped and the pen keeps the previous font's metrics.
using FontMetricsLookup = std::function<std::optional<FontMetrics>(int nvid)>;

enum class Align : std::uint8_t { Left, Center, Right };

struct LayoutOptions {
    Align align = Align::Left;  // per-line alignment inside boxWidth (extension)
    int boxWidth = 0;           // used by Center/Right
    std::size_t maxBytes = std::numeric_limits<std::size_t>::max(); // visible prefix (typewriter)
};

struct PlacedGlyph {
    int font;           // nvid of the font to draw with
    std::uint8_t code;  // frame index
    int x, y;           // top-left corner
};

struct TextExtent {
    int width = 0;   // widest line, in pixels (cells * advance)
    int height = 0;  // lines * line height
    int lines = 0;
};

namespace detail {

// One pass of the exe's loop. Calls glyph(font, code, x, y) for printable bytes and
// lineEnd(lineIndex, penXEnd) at every line end (including the last one).
template <typename GlyphFn, typename LineFn>
void walkText(std::string_view text, int font, int x, int y, const FontMetricsLookup& lookup,
              std::size_t maxBytes, std::span<const int> lineShift, GlyphFn&& glyph, LineFn&& lineEnd) {
    const FontMetrics fallback{};
    auto metricsOf = [&](int nvid, const FontMetrics& previous) { return lookup ? lookup(nvid).value_or(previous) : previous; };
    const bool known0 = lookup && lookup(font).has_value();
    FontMetrics m = metricsOf(font, fallback);
    bool known = known0;

    const int x0 = x + m.originX();
    int penY = y + m.originY();
    const int lineBase = x0 - m.advance; // exe: line start is kept as x0 - advance(first font)
    int line = 0;
    auto shift = [&](int l) { return l < static_cast<int>(lineShift.size()) ? lineShift[l] : 0; };
    int penX = x0 + shift(0);
    int lineStart = x0; // where the current line's first cell starts (unshifted)
    int lineWidth = 0;  // rightmost pen position reached on this line, relative to lineStart

    const std::size_t end = std::min(text.size(), maxBytes);
    for (std::size_t i = 0; i < end; ++i) {
        const auto c = static_cast<std::uint8_t>(text[i]);
        if (c == '\n') {
            lineEnd(line, lineWidth);
            penY += m.lineHeight;
            ++line;
            lineWidth = 0;
            penX = lineBase + shift(line);
            lineStart = lineBase + m.advance;
        } else if (c == '\r') {
            penX = lineBase + shift(line);
            lineStart = lineBase + m.advance;
        } else if (c == static_cast<std::uint8_t>(Esc) && i + 1 < end) {
            font = static_cast<std::uint8_t>(text[++i]);
            known = lookup && lookup(font).has_value();
            m = metricsOf(font, m);
        } else if (c >= 0x20 && known) {
            glyph(font, c, penX, penY);
        }
        penX += m.advance;
        if (c != '\n' && c != '\r')
            lineWidth = std::max(lineWidth, penX - shift(line) - lineStart);
    }
    lineEnd(line, lineWidth);
}

} // namespace detail

// Places the visible glyphs of `text` printed at (x, y) with font `font`.
inline std::vector<PlacedGlyph> layoutText(std::string_view text, int font, int x, int y,
                                           const FontMetricsLookup& lookup, const LayoutOptions& options = {}) {
    std::vector<int> shifts;
    if (options.align != Align::Left) {
        detail::walkText(text, font, x, y, lookup, options.maxBytes, {}, [](int, std::uint8_t, int, int) {},
                         [&](int, int width) {
                             const int free = options.boxWidth - width;
                             shifts.push_back(options.align == Align::Center ? free / 2 : free);
                         });
    }
    std::vector<PlacedGlyph> glyphs;
    detail::walkText(text, font, x, y, lookup, options.maxBytes, shifts,
                     [&](int f, std::uint8_t code, int gx, int gy) { glyphs.push_back({f, code, gx, gy}); },
                     [](int, int) {});
    return glyphs;
}

// Size of the text box: widest line (in pen advance, trailing spaces included) and the number
// of lines times the line height of the text's font. A trailing '\n' starts a new (empty) line.
inline TextExtent measureText(std::string_view text, int font, const FontMetricsLookup& lookup,
                              std::size_t maxBytes = std::numeric_limits<std::size_t>::max()) {
    TextExtent extent;
    detail::walkText(text, font, 0, 0, lookup, maxBytes, {}, [](int, std::uint8_t, int, int) {},
                     [&](int, int width) {
                         extent.width = std::max(extent.width, width);
                         ++extent.lines;
                     });
    const FontMetrics m = lookup ? lookup(font).value_or(FontMetrics{}) : FontMetrics{};
    extent.height = extent.lines * m.lineHeight;
    return extent;
}

// Word wrap for monospaced text: breaks lines longer than `columns` cells at spaces,
// hard-splits longer words, keeps existing line breaks ("\r\n" or "\n"), trims the
// spaces at automatic breaks and emits "\n" there. ESC pairs count as one cell, like
// the exe does. Not an original feature.
inline std::string wrapText(std::string_view text, int columns) {
    if (columns <= 0)
        return std::string{text};
    std::string out;
    out.reserve(text.size() + text.size() / std::max(columns, 1));

    std::string line;      // current line, as bytes
    int lineCells = 0;
    auto flushLine = [&](bool hard) {
        // Trim spaces at automatic breaks only.
        if (!hard)
            while (!line.empty() && line.back() == ' ') { line.pop_back(); --lineCells; }
        out += line;
        line.clear();
        lineCells = 0;
    };

    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c == '\r' || c == '\n') {
            flushLine(true);
            out += c;
            ++i;
            continue;
        }
        if (c == ' ') {
            if (lineCells < columns) { line += ' '; ++lineCells; }
            else { flushLine(false); out += '\n'; }
            ++i;
            continue;
        }
        // A word: bytes up to the next space or line break (ESC pairs are one cell).
        std::string word;
        int cells = 0;
        while (i < text.size() && text[i] != ' ' && text[i] != '\r' && text[i] != '\n') {
            if (text[i] == Esc && i + 1 < text.size()) { word += text[i++]; }
            word += text[i++];
            ++cells;
        }
        if (lineCells + cells <= columns) {
            line += word;
            lineCells += cells;
            continue;
        }
        if (lineCells > 0) {
            flushLine(false);
            out += '\n';
        }
        // Hard-split words longer than a whole line.
        std::size_t k = 0;
        while (cells > columns) {
            int taken = 0;
            while (taken < columns) {
                if (word[k] == Esc && k + 1 < word.size()) line += word[k++];
                line += word[k++];
                ++taken;
            }
            lineCells = taken;
            cells -= taken;
            flushLine(true);
            out += '\n';
        }
        line += word.substr(k);
        lineCells = cells;
    }
    flushLine(false);
    return out;
}

// Convenience: wrap to a pixel width with the given font metrics.
inline std::string wrapTextToWidth(std::string_view text, int widthPx, const FontMetrics& metrics) {
    return wrapText(text, metrics.advance > 0 ? widthPx / metrics.advance : 0);
}

} // namespace gromada::ui
