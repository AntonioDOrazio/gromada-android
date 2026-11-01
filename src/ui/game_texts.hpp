#pragma once
// Localized texts of the game data (see docs/ui.md, "Testi localizzati"):
//  - maps/LOCALE.INI: script #defines with the UI strings (STR_NONAME, STR_ON, ...), included by
//    HEADER.INI;
//  - maps/levels/NN.TXT: mission briefing shown by GameMenuOn() (PrintFile at centre - (230, 140));
//  - maps/levels/NNS.TXT, maps/net/NNS.TXT: one-line map summaries of the mission choice screen
//    (PrintFile at centre + (-80, 117));
//  - maps/W01.TXT..W15.TXT: weapon (W01..W12) and transmission (W13..W15) descriptions of the
//    design screen (PrintFile at centre + (-80, 117));
//  - vid names (GetVidName) are localized in fw.res itself.
// All of them are CP866 bytes (pure ASCII in the Italian edition, apart from a Cyrillic
// "АРЕНА" left in levels/09S.TXT) with CRLF line ends.
//
// Script paths use backslashes and the case of the original Windows file system
// ("maps\\levels\\01.txt" names the file maps/levels/01.TXT): resolveGamePath() looks the
// components up case-insensitively.
//
// Header-only, depends only on core/.

#include "core/std.hpp"

#include <cctype>
#include <map>

namespace gromada::ui {

namespace detail {
inline bool equalsNoCase(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}
} // namespace detail

// Resolves a script path (backslashes or slashes, any case) below the game root folder.
// Returns nullopt when some component does not exist. ".." components are rejected.
inline std::optional<std::filesystem::path> resolveGamePath(const std::filesystem::path& root, std::string_view scriptPath) {
    std::filesystem::path current = root;
    std::size_t start = 0;
    while (start <= scriptPath.size()) {
        std::size_t end = scriptPath.find_first_of("\\/", start);
        if (end == std::string_view::npos)
            end = scriptPath.size();
        const std::string_view part = scriptPath.substr(start, end - start);
        start = end + 1;
        if (part.empty() || part == ".")
            continue;
        if (part == "..")
            return std::nullopt;

        std::error_code ec;
        const auto exact = current / std::string{part};
        if (std::filesystem::exists(exact, ec)) {
            current = exact;
            continue;
        }
        std::optional<std::filesystem::path> match;
        for (const auto& entry : std::filesystem::directory_iterator{current, ec}) {
            if (detail::equalsNoCase(entry.path().filename().string(), part)) {
                match = entry.path();
                break;
            }
        }
        if (!match)
            return std::nullopt;
        current = *match;
    }
    return current;
}

inline std::optional<std::string> readGameText(const std::filesystem::path& root, std::string_view scriptPath) {
    const auto path = resolveGamePath(root, scriptPath);
    if (!path)
        return std::nullopt;
    std::ifstream file{*path, std::ios::binary};
    if (!file)
        return std::nullopt;
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

// Script-relative paths of the localized text files (as the scripts build them).
inline std::string briefingPath(int level) { return std::format("maps\\levels\\{:02}.txt", level); }
inline std::string mapSummaryPath(int level) { return std::format("maps\\levels\\{:02}s.txt", level); }
inline std::string netMapSummaryPath(int map) { return std::format("maps\\net\\{:02}s.txt", map); }
inline std::string weaponTextPath(int index) { return std::format("maps\\w{:02}.txt", index); } // 1..15

// #define NAME value lines of a script file (LOCALE.INI and friends). String values are
// unquoted (\\ and \" escapes resolved), other values are kept as written (comments removed).
// Bytes are returned as they are (CP866).
inline std::map<std::string, std::string, std::less<>> parseDefines(std::string_view source) {
    std::map<std::string, std::string, std::less<>> defines;
    std::size_t pos = 0;
    while (pos < source.size()) {
        std::size_t eol = source.find('\n', pos);
        if (eol == std::string_view::npos)
            eol = source.size();
        std::string_view line = source.substr(pos, eol - pos);
        pos = eol + 1;

        auto trimLeft = [](std::string_view s) {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
            return s;
        };
        line = trimLeft(line);
        if (!line.starts_with("#define"))
            continue;
        line = trimLeft(line.substr(7));
        const std::size_t nameEnd = line.find_first_of(" \t\r");
        if (nameEnd == 0 || nameEnd == std::string_view::npos)
            continue;
        const std::string name{line.substr(0, nameEnd)};
        line = trimLeft(line.substr(nameEnd));

        std::string value;
        if (line.starts_with('"')) {
            for (std::size_t i = 1; i < line.size() && line[i] != '"'; ++i) {
                if (line[i] == '\\' && i + 1 < line.size()) ++i;
                value += line[i];
            }
        } else {
            if (const auto comment = line.find("//"); comment != std::string_view::npos)
                line = line.substr(0, comment);
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
                line.remove_suffix(1);
            value = line;
        }
        defines.insert_or_assign(name, std::move(value));
    }
    return defines;
}

// UI strings of maps/LOCALE.INI.
class Locale {
public:
    Locale() = default;
    explicit Locale(std::map<std::string, std::string, std::less<>> strings) : m_strings{std::move(strings)} {}

    static Locale load(const std::filesystem::path& gameRoot) {
        if (const auto text = readGameText(gameRoot, "maps\\locale.ini"))
            return Locale{parseDefines(*text)};
        return {};
    }

    // The string, or `fallback` when the key is missing.
    [[nodiscard]] std::string get(std::string_view key, std::string_view fallback = {}) const {
        const auto it = m_strings.find(key);
        return it != m_strings.end() ? it->second : std::string{fallback};
    }
    [[nodiscard]] bool contains(std::string_view key) const { return m_strings.contains(key); }
    [[nodiscard]] const auto& strings() const noexcept { return m_strings; }

private:
    std::map<std::string, std::string, std::less<>> m_strings;
};

} // namespace gromada::ui
