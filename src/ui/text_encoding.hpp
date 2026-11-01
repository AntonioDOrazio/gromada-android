#pragma once
// Text encodings of Gromada's data (see docs/ui.md, "Codifiche").
//
// The engine works on raw bytes: a font glyph is simply frame[byte] of the font vid.
// The byte values follow DOS code page 866 (the original game is Russian):
//  - 0x20..0x7E ASCII; the Italian edition writes accented letters as letter + apostrophe
//    ("UNITA'") and is pure ASCII (all maps/*.TXT, LOCALE.INI, vid names);
//  - 0x80..0x9F Cyrillic capitals (А..Я): leftovers such as maps/levels/09S.TXT "АРЕНА";
//  - script comments and vid names of the Russian data are CP866 as well.
// Header-only, depends only on core/.

#include "core/cp866.hpp"
#include "core/std.hpp"

namespace gromada::ui {

// Game bytes (CP866) -> UTF-8, for logs and tools.
inline std::string gameToUtf8(std::string_view bytes) { return cp866_to_utf8(bytes); }

namespace detail {

// Decodes one UTF-8 sequence starting at text[i]; advances i. Invalid input -> U+FFFD.
inline char32_t decodeUtf8(std::string_view text, std::size_t& i) {
    const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(text[k]); };
    const unsigned char c = byte(i++);
    if (c < 0x80)
        return c;
    int extra = 0;
    char32_t cp = 0;
    if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
    else return U'�';
    for (int k = 0; k < extra; ++k) {
        if (i >= text.size() || (byte(i) & 0xC0) != 0x80)
            return U'�';
        cp = (cp << 6) | (byte(i++) & 0x3F);
    }
    return cp;
}

// Latin letters with diacritics -> base ASCII letter (Italian, French, German, Spanish...).
inline char latinBase(char32_t cp) {
    static constexpr std::string_view upper = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTs"; // U+00C0..U+00DF
    static constexpr std::string_view lower = "aaaaaaaceeeeiiiidnooooo/ouuuuyty"; // U+00E0..U+00FF
    if (cp >= 0xC0 && cp <= 0xDF) return upper[cp - 0xC0];
    if (cp >= 0xE0 && cp <= 0xFF) return lower[cp - 0xE0];
    return 0;
}

} // namespace detail

// UTF-8 -> game bytes, for strings produced by the core (option values, messages...).
//  - ASCII is copied, CP866 code points map to their byte;
//  - accented Latin letters follow the Italian edition's convention: base letter followed
//    by an apostrophe for grave/acute vowels ("à" -> "a'", "È" -> "E'"), plain base letter
//    otherwise ("ç" -> "c", "ñ" -> "n");
//  - anything else becomes `fallback`.
inline std::string utf8ToGame(std::string_view utf8, char fallback = '?') {
    std::string out;
    out.reserve(utf8.size());
    for (std::size_t i = 0; i < utf8.size();) {
        const char32_t cp = detail::decodeUtf8(utf8, i);
        if (cp < 0x80) {
            out += static_cast<char>(cp);
            continue;
        }
        if (const auto it = std::ranges::find(cp866_to_unicode, static_cast<std::uint16_t>(cp));
            cp <= 0xFFFF && it != cp866_to_unicode.end()) {
            out += static_cast<char>(0x80 + (it - cp866_to_unicode.begin()));
            continue;
        }
        if (const char base = detail::latinBase(cp)) {
            out += base;
            // à á è é ì í ò ó ù ú (and capitals): the edition writes them as "A'".
            static constexpr std::u32string_view accented = U"ÀÁÈÉÌÍÒÓÙÚàáèéìíòóùú";
            if (accented.find(cp) != std::u32string_view::npos)
                out += '\'';
            continue;
        }
        out += fallback;
    }
    return out;
}

} // namespace gromada::ui
