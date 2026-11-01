#pragma once
// Script sources: where the text of the main file and of `#include`d files comes from.
//
// The game writes include paths DOS-style, relative to the game folder and in any letter case
// (`#include "maps\header.ini"` while the file is `maps/HEADER.INI`). Loaders therefore accept
// backslashes and resolve every path component case-insensitively.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "script/diagnostics.hpp"

namespace gromada::script {

// Stable storage for file names referenced by SourceLocation::file.
class SourceFiles {
public:
    const std::string* intern(std::string name) {
        for (const auto& n : names_)
            if (*n == name) return n.get();
        names_.push_back(std::make_unique<std::string>(std::move(name)));
        return names_.back().get();
    }

private:
    std::vector<std::unique_ptr<std::string>> names_;
};

struct LoadedSource {
    std::string name;  // display name used in diagnostics
    std::string key;   // identity of the file (resolved path), used to detect include cycles
    std::string text;  // raw bytes
};

class SourceLoader {
public:
    virtual ~SourceLoader() = default;
    // `path` is the include path as written in the script (or the main file's path).
    // `includedFrom` is the key of the including file, empty for the main file.
    virtual std::optional<LoadedSource> load(std::string_view path, std::string_view includedFrom) = 0;
};

namespace detail {
inline char asciiLower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }
inline std::string asciiLower(std::string_view s) {
    std::string r{s};
    for (auto& c : r) c = asciiLower(c);
    return r;
}
inline std::string normalizeSlashes(std::string_view p) {
    std::string r{p};
    std::ranges::replace(r, '\\', '/');
    return r;
}
} // namespace detail

// Resolves `relative` (DOS or POSIX separators) below `base`, matching each component without
// regard to ASCII letter case. Returns the existing path, or nullopt.
inline std::optional<std::filesystem::path> resolveCaseInsensitive(const std::filesystem::path& base,
                                                                   std::string_view relative) {
    namespace fs = std::filesystem;
    const fs::path rel{detail::normalizeSlashes(relative)};
    fs::path current = rel.is_absolute() ? rel.root_path() : base;
    std::error_code ec;
    for (const auto& part : rel.relative_path()) {
        const std::string name = part.string();
        if (name.empty() || name == ".") continue;
        if (name == "..") { current = current.parent_path(); continue; }
        fs::path exact = current / part;
        if (fs::exists(exact, ec)) { current = std::move(exact); continue; }
        const std::string wanted = detail::asciiLower(name);
        std::optional<fs::path> found;
        for (fs::directory_iterator it{current, ec}, end; !ec && it != end; it.increment(ec)) {
            if (detail::asciiLower(it->path().filename().string()) == wanted) { found = it->path(); break; }
        }
        if (!found) return std::nullopt;
        current = *found;
    }
    if (!fs::exists(current, ec)) return std::nullopt;
    return current;
}

// Loads files from disk. Include paths are tried relative to `root` (the game folder: the
// scripts write `maps\...`) and then relative to the directory of the including file.
class FileSystemSourceLoader : public SourceLoader {
public:
    explicit FileSystemSourceLoader(std::filesystem::path root) : root_{std::move(root)} {}

    std::optional<LoadedSource> load(std::string_view path, std::string_view includedFrom) override {
        namespace fs = std::filesystem;
        std::vector<fs::path> bases{root_};
        if (!includedFrom.empty()) bases.push_back(fs::path{includedFrom}.parent_path());
        else bases.push_back(fs::current_path());
        for (const auto& base : bases) {
            const auto resolved = resolveCaseInsensitive(base, path);
            if (!resolved || !fs::is_regular_file(*resolved)) continue;
            std::ifstream in{*resolved, std::ios::binary};
            if (!in) continue;
            std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
            std::error_code ec;
            const fs::path canonical = fs::weakly_canonical(*resolved, ec);
            return LoadedSource{std::string{path}, (ec ? *resolved : canonical).string(), std::move(text)};
        }
        return std::nullopt;
    }

    const std::filesystem::path& root() const { return root_; }

private:
    std::filesystem::path root_;
};

// In-memory sources (tests, tools). Paths are matched case-insensitively with either separator.
class MemorySourceLoader : public SourceLoader {
public:
    MemorySourceLoader& add(std::string_view path, std::string text) {
        files_[key(path)] = std::move(text);
        return *this;
    }

    std::optional<LoadedSource> load(std::string_view path, std::string_view) override {
        const auto k = key(path);
        const auto it = files_.find(k);
        if (it == files_.end()) return std::nullopt;
        return LoadedSource{std::string{path}, k, it->second};
    }

private:
    static std::string key(std::string_view path) { return detail::asciiLower(detail::normalizeSlashes(path)); }
    std::map<std::string, std::string> files_;
};

} // namespace gromada::script
