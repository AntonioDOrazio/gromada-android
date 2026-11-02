#pragma once
// Game session, files: the script builtins FOpen/FCreate/FRead/FWrite/FClose/FileCopy
// (docs/re/runtime.md §3, §7) on top of the libretro save directory.
//
// The original opens files relative to the game folder with the C runtime in text mode:
//   FOpen(name)   fopen(name, "r+t"), 0 if it does not exist
//   FCreate(name) fopen(name, "w+t")
//   FRead(f)      fgets(buf, 128): one line without the trailing "\n"/"\r"; with f = 0 or at
//                 the end of the file it returns the previous line (one static buffer shared
//                 by every file, 0x43f8f0)
//   FWrite(f, v)  fprintf("%s\n") in text mode: CRLF on disk
//   FileCopy(s,d) CopyFileA(s, d, FALSE)
// Here nothing is ever written to the game folder: every write goes to `saveDir`; reads look
// in `saveDir` first and fall back to the game folder (read-only originals). Names are
// resolved case-insensitively (the scripts write "save1", "fw.ini", DOS paths). With an empty
// `saveDir` (frontend without a save directory) written files live in memory only.
// seed() copies the original profiles/options (save1..5, fw.ini) from the game folder into
// the save directory the first time, so the player starts from the shipped files.
//
// Header-only; depends on core/ and the script module's case-insensitive path resolver.

#include "core/std.hpp"
#include "script/source.hpp"

#include <map>

namespace gromada::game {

class SaveFiles {
public:
    // Files copied from the game folder to the save directory on first run.
    static constexpr std::array<std::string_view, 6> SeededFiles = {"save1", "save2", "save3", "save4", "save5", "fw.ini"};

    SaveFiles() = default;
    SaveFiles(std::filesystem::path gameDir, std::filesystem::path saveDir)
        : m_gameDir{std::move(gameDir)}, m_saveDir{std::move(saveDir)} {}

    [[nodiscard]] const std::filesystem::path& saveDir() const noexcept { return m_saveDir; }
    [[nodiscard]] const std::filesystem::path& gameDir() const noexcept { return m_gameDir; }

    // Copies the shipped files that are missing from the save directory. Returns how many
    // were copied.
    int seed() {
        if (m_saveDir.empty())
            return 0;
        std::error_code ec;
        std::filesystem::create_directories(m_saveDir, ec);
        int copied = 0;
        for (const auto name : SeededFiles) {
            if (script::resolveCaseInsensitive(m_saveDir, name))
                continue;
            const auto src = script::resolveCaseInsensitive(m_gameDir, name);
            if (!src)
                continue;
            if (std::filesystem::copy_file(*src, m_saveDir / std::string{name}, ec))
                ++copied;
        }
        return copied;
    }

    // FOpen: 0 when the file does not exist (in the save directory or the game folder).
    int open(std::string_view name) {
        const auto key = sanitize(name);
        if (!key)
            return 0;
        auto data = readFile(*key);
        if (!data)
            return 0;
        return add(File{*key, std::move(*data), 0, false});
    }

    // FCreate: an empty file, created on disk right away like fopen("w+t").
    int create(std::string_view name) {
        const auto key = sanitize(name);
        if (!key)
            return 0;
        File f{*key, {}, 0, true};
        if (!flush(f))
            return 0;
        return add(std::move(f));
    }

    // FRead: the next line, or the previous one (shared buffer) at the end or for a bad handle.
    std::string readLine(int handle) {
        File* f = find(handle);
        if (!f || f->pos >= f->data.size())
            return m_lastLine;
        // fgets(buf, 128): at most 127 bytes, newline included.
        std::string line;
        while (f->pos < f->data.size() && line.size() < 127) {
            const char c = f->data[f->pos++];
            if (c == '\r' && f->pos < f->data.size() && f->data[f->pos] == '\n')
                continue; // text mode: CRLF -> LF
            line += c;
            if (c == '\n')
                break;
        }
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        m_lastLine = line;
        return line;
    }

    // FWrite: the text and a line end (CRLF, as the text-mode C runtime writes on Windows).
    bool writeLine(int handle, std::string_view text) {
        File* f = find(handle);
        if (!f)
            return false;
        const std::string bytes = std::string{text} + "\r\n";
        if (f->pos > f->data.size())
            f->pos = f->data.size();
        f->data.replace(f->pos, std::min(bytes.size(), f->data.size() - f->pos), bytes);
        f->pos += bytes.size();
        f->dirty = true;
        return true;
    }

    // FClose: writes back modified files. False for an unknown handle.
    bool close(int handle) {
        const auto it = m_open.find(handle);
        if (it == m_open.end())
            return false;
        if (it->second.dirty)
            flush(it->second);
        m_open.erase(it);
        return true;
    }

    void closeAll() {
        for (auto& [h, f] : m_open)
            if (f.dirty)
                flush(f);
        m_open.clear();
    }

    // FileCopy(source, dest): source from the save directory or the game folder, destination
    // in the save directory.
    bool copy(std::string_view source, std::string_view dest) {
        const auto src = sanitize(source), dst = sanitize(dest);
        if (!src || !dst)
            return false;
        auto data = readFile(*src);
        if (!data)
            return false;
        File f{*dst, std::move(*data), 0, true};
        return flush(f);
    }

    // Whole content of a file as the scripts would see it (tests, demos), nullopt if missing.
    [[nodiscard]] std::optional<std::string> read(std::string_view name) const {
        const auto key = sanitize(name);
        return key ? readFile(*key) : std::nullopt;
    }

    // Path a file is written to (empty without a save directory).
    [[nodiscard]] std::filesystem::path writePath(std::string_view name) const {
        const auto key = sanitize(name);
        if (!key || m_saveDir.empty())
            return {};
        if (const auto existing = script::resolveCaseInsensitive(m_saveDir, *key))
            return *existing;
        return m_saveDir / *key;
    }

    [[nodiscard]] std::size_t openCount() const noexcept { return m_open.size(); }
    [[nodiscard]] const std::string& lastLine() const noexcept { return m_lastLine; }

    // A relative path with '/' separators, or nullopt for absolute paths, ".." and empty names.
    static std::optional<std::string> sanitize(std::string_view name) {
        std::string out;
        std::string part;
        auto flushPart = [&]() -> bool {
            if (part.empty() || part == ".") { part.clear(); return true; }
            if (part == "..") return false;
            if (!out.empty()) out += '/';
            out += part;
            part.clear();
            return true;
        };
        if (!name.empty() && (name.front() == '/' || name.front() == '\\'))
            return std::nullopt;
        if (name.size() >= 2 && name[1] == ':')
            return std::nullopt; // drive letter
        for (const char c : name) {
            if (c == '\0') break;
            if (c == '/' || c == '\\') {
                if (!flushPart()) return std::nullopt;
            } else {
                part += c;
            }
        }
        if (!flushPart() || out.empty())
            return std::nullopt;
        return out;
    }

private:
    struct File {
        std::string key;
        std::string data;
        std::size_t pos = 0;
        bool dirty = false;
    };

    int add(File f) {
        const int h = m_next++;
        m_open.emplace(h, std::move(f));
        return h;
    }
    File* find(int handle) {
        const auto it = m_open.find(handle);
        return it == m_open.end() ? nullptr : &it->second;
    }

    static std::string lower(std::string s) {
        for (auto& c : s) c = (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
        return s;
    }

    std::optional<std::string> readFile(const std::string& key) const {
        if (const auto it = m_memory.find(lower(key)); it != m_memory.end())
            return it->second;
        for (const auto* dir : {&m_saveDir, &m_gameDir}) {
            if (dir->empty())
                continue;
            const auto path = script::resolveCaseInsensitive(*dir, key);
            std::error_code ec;
            if (!path || !std::filesystem::is_regular_file(*path, ec))
                continue;
            std::ifstream in{*path, std::ios::binary};
            if (!in)
                continue;
            return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        }
        return std::nullopt;
    }

    bool flush(File& f) {
        f.dirty = false;
        if (m_saveDir.empty()) {
            m_memory[lower(f.key)] = f.data;
            return true;
        }
        const auto path = writePath(f.key);
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out{path, std::ios::binary | std::ios::trunc};
        if (!out) {
            m_memory[lower(f.key)] = f.data; // keep it for this session at least
            return false;
        }
        out.write(f.data.data(), static_cast<std::streamsize>(f.data.size()));
        m_memory.erase(lower(f.key));
        return static_cast<bool>(out);
    }

    std::filesystem::path m_gameDir, m_saveDir;
    std::map<int, File> m_open;
    std::map<std::string, std::string> m_memory; // written files without a save directory
    std::string m_lastLine;
    int m_next = 1;
};

} // namespace gromada::game
