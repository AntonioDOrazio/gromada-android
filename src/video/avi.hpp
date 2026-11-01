#pragma once
// AVI (RIFF) demuxer for the game's movies (avi/0.avi ... avi/3.avi).
//
// Reads the headers (avih, strl/strh/strf) and builds a per-stream chunk index, from `idx1`
// when present and consistent, otherwise by walking the `movi` list (including `rec ` lists
// and OpenDML `AVIX` continuation RIFFs). Chunk payloads are read on demand from the file,
// so memory use does not depend on the movie length.
//
// Chunk ids are "NNxx": NN = stream number, xx = dc/db (video), wb (audio), pc (palette
// change). A zero-sized video chunk is a "drop frame": the previous picture is repeated.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace gromada::video {

constexpr std::uint32_t fourcc(const char (&s)[5]) noexcept {
    return std::uint32_t(std::uint8_t(s[0])) | std::uint32_t(std::uint8_t(s[1])) << 8 |
           std::uint32_t(std::uint8_t(s[2])) << 16 | std::uint32_t(std::uint8_t(s[3])) << 24;
}

inline std::string fourccString(std::uint32_t v) {
    std::string s(4, ' ');
    for (int i = 0; i < 4; ++i) {
        const char c = static_cast<char>((v >> (8 * i)) & 0xFF);
        s[i] = (c >= 32 && c < 127) ? c : '?';
    }
    return s;
}

struct AviChunk {
    std::uint64_t offset = 0; // file offset of the payload (after the 8-byte chunk header)
    std::uint32_t size = 0;
    std::uint32_t id = 0;     // chunk fourcc, e.g. '00dc'
    bool keyframe = false;
};

struct AviVideoFormat {         // BITMAPINFOHEADER (+ palette)
    std::int32_t width = 0, height = 0; // height < 0: top-down DIB
    std::uint16_t bitCount = 0;
    std::uint32_t compression = 0;      // fourcc, e.g. 'CRAM', 'IV32'
    std::vector<std::uint32_t> palette; // 0x00RRGGBB, for palettised formats
};

struct AviAudioFormat {         // WAVEFORMATEX
    std::uint16_t formatTag = 0;        // 1 = PCM
    std::uint16_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint16_t blockAlign = 0;
    std::uint16_t bitsPerSample = 0;
};

struct AviStream {
    std::uint32_t type = 0;     // 'vids' / 'auds'
    std::uint32_t handler = 0;
    std::uint32_t scale = 1, rate = 0, start = 0, length = 0, sampleSize = 0;
    std::uint32_t initialFrames = 0;
    AviVideoFormat video;
    AviAudioFormat audio;
    std::vector<AviChunk> chunks; // data chunks in file order (palette changes excluded)
};

class AviFile {
public:
    // Throws std::runtime_error on unreadable or malformed files.
    explicit AviFile(const std::filesystem::path& path) : m_in{path, std::ios::binary} {
        if (!m_in)
            throw std::runtime_error("cannot open " + path.string());
        m_in.seekg(0, std::ios::end);
        m_fileSize = static_cast<std::uint64_t>(m_in.tellg());
        parse();
    }

    [[nodiscard]] const std::vector<AviStream>& streams() const noexcept { return m_streams; }
    [[nodiscard]] std::uint32_t microSecPerFrame() const noexcept { return m_usPerFrame; }
    [[nodiscard]] bool indexFromIdx1() const noexcept { return m_usedIdx1; }

    // First stream of the given type ('vids' / 'auds'), or -1.
    [[nodiscard]] int findStream(std::uint32_t type) const noexcept {
        for (std::size_t i = 0; i < m_streams.size(); ++i)
            if (m_streams[i].type == type) return static_cast<int>(i);
        return -1;
    }

    // Reads a chunk payload into `out` (resized). Returns false on I/O error.
    bool read(const AviChunk& c, std::vector<std::uint8_t>& out) {
        out.resize(c.size);
        if (c.size == 0)
            return true;
        m_in.clear();
        m_in.seekg(static_cast<std::streamoff>(c.offset));
        m_in.read(reinterpret_cast<char*>(out.data()), c.size);
        return static_cast<std::size_t>(m_in.gcount()) == c.size;
    }

    // Rebuilds the index by walking `movi` even when idx1 exists (used by tests to check
    // that both sources agree).
    [[nodiscard]] std::vector<std::vector<AviChunk>> scanMovi() {
        std::vector<std::vector<AviChunk>> perStream(m_streams.size());
        for (const auto& [begin, end] : m_moviLists)
            scanList(begin, end, perStream);
        return perStream;
    }

private:
    static constexpr std::uint32_t RIFF = fourcc("RIFF"), LIST = fourcc("LIST");
    static constexpr std::uint32_t AVIIF_KEYFRAME = 0x10;

    struct Range { std::uint64_t begin, end; }; // payload of a list, after its type fourcc

    std::uint32_t u32(std::span<const std::uint8_t> b, std::size_t o) const {
        if (o + 4 > b.size()) throw std::runtime_error("AVI: truncated header");
        return std::uint32_t(b[o]) | std::uint32_t(b[o + 1]) << 8 | std::uint32_t(b[o + 2]) << 16 | std::uint32_t(b[o + 3]) << 24;
    }
    std::uint16_t u16(std::span<const std::uint8_t> b, std::size_t o) const {
        if (o + 2 > b.size()) throw std::runtime_error("AVI: truncated header");
        return static_cast<std::uint16_t>(b[o] | b[o + 1] << 8);
    }

    std::vector<std::uint8_t> readAt(std::uint64_t offset, std::size_t size) {
        if (offset + size > m_fileSize)
            throw std::runtime_error("AVI: chunk past end of file");
        std::vector<std::uint8_t> buf(size);
        m_in.clear();
        m_in.seekg(static_cast<std::streamoff>(offset));
        m_in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(size));
        if (static_cast<std::size_t>(m_in.gcount()) != size)
            throw std::runtime_error("AVI: read error");
        return buf;
    }

    // Header of the chunk at `pos`: {id, size, listType (0 if not a list)}.
    struct Header { std::uint32_t id, size, listType; };
    std::optional<Header> headerAt(std::uint64_t pos, std::uint64_t end) {
        if (pos + 8 > end || pos + 8 > m_fileSize)
            return std::nullopt;
        const auto h = readAt(pos, (pos + 12 <= m_fileSize) ? 12 : 8);
        Header r{u32(h, 0), u32(h, 4), 0};
        if ((r.id == RIFF || r.id == LIST) && h.size() >= 12)
            r.listType = u32(h, 8);
        return r;
    }

    void parse() {
        std::uint64_t pos = 0;
        bool first = true;
        // Top level: RIFF 'AVI ' followed by optional RIFF 'AVIX' (OpenDML continuation).
        while (auto h = headerAt(pos, m_fileSize)) {
            if (h->id != RIFF) break;
            if (first && h->listType != fourcc("AVI "))
                throw std::runtime_error("not an AVI file");
            const std::uint64_t end = std::min<std::uint64_t>(pos + 8 + h->size, m_fileSize);
            parseList(pos + 12, end, first);
            first = false;
            pos = end + (h->size & 1);
        }
        if (first)
            throw std::runtime_error("not a RIFF file");
        if (m_streams.empty())
            throw std::runtime_error("AVI: no streams");
        if (m_moviLists.empty())
            throw std::runtime_error("AVI: no movi list");

        if (m_idx1 && loadIdx1()) {
            m_usedIdx1 = true;
        } else {
            auto perStream = scanMovi();
            for (std::size_t i = 0; i < m_streams.size(); ++i)
                m_streams[i].chunks = std::move(perStream[i]);
        }
    }

    void parseList(std::uint64_t pos, std::uint64_t end, bool mainRiff) {
        while (auto h = headerAt(pos, end)) {
            const std::uint64_t next = pos + 8 + h->size + (h->size & 1);
            if (h->id == LIST) {
                const std::uint64_t listEnd = std::min<std::uint64_t>(pos + 8 + h->size, end);
                if (h->listType == fourcc("hdrl") || h->listType == fourcc("strl"))
                    parseList(pos + 12, listEnd, mainRiff);
                else if (h->listType == fourcc("movi"))
                    m_moviLists.push_back({pos + 12, listEnd});
                if (h->listType == fourcc("strl"))
                    m_inStrl = false; // a new strl starts a new stream
            } else if (h->id == fourcc("avih")) {
                const auto b = readAt(pos + 8, std::min<std::uint32_t>(h->size, 56));
                m_usPerFrame = u32(b, 0);
            } else if (h->id == fourcc("strh")) {
                const auto b = readAt(pos + 8, std::min<std::uint32_t>(h->size, 56));
                AviStream s;
                s.type = u32(b, 0);
                s.handler = u32(b, 4);
                s.initialFrames = u32(b, 16);
                s.scale = std::max<std::uint32_t>(u32(b, 20), 1);
                s.rate = u32(b, 24);
                s.start = u32(b, 28);
                s.length = u32(b, 32);
                s.sampleSize = u32(b, 44);
                m_streams.push_back(std::move(s));
                m_inStrl = true;
            } else if (h->id == fourcc("strf") && m_inStrl && !m_streams.empty()) {
                parseFormat(m_streams.back(), readAt(pos + 8, h->size));
            } else if (h->id == fourcc("idx1") && mainRiff) {
                m_idx1 = Range{pos + 8, pos + 8 + h->size};
            }
            pos = next;
        }
    }

    void parseFormat(AviStream& s, const std::vector<std::uint8_t>& b) {
        if (s.type == fourcc("vids") && b.size() >= 40) {
            const std::uint32_t biSize = u32(b, 0);
            s.video.width = static_cast<std::int32_t>(u32(b, 4));
            s.video.height = static_cast<std::int32_t>(u32(b, 8));
            s.video.bitCount = u16(b, 14);
            s.video.compression = u32(b, 16);
            std::uint32_t colors = u32(b, 32);
            if (colors == 0 && s.video.bitCount <= 8)
                colors = 1u << s.video.bitCount;
            const std::size_t palOffset = std::max<std::uint32_t>(biSize, 40);
            colors = static_cast<std::uint32_t>(std::min<std::size_t>(colors, b.size() > palOffset ? (b.size() - palOffset) / 4 : 0));
            for (std::uint32_t i = 0; i < colors; ++i) {
                const std::size_t o = palOffset + 4 * i; // RGBQUAD: B, G, R, reserved
                s.video.palette.push_back(std::uint32_t(b[o + 2]) << 16 | std::uint32_t(b[o + 1]) << 8 | b[o]);
            }
        } else if (s.type == fourcc("auds") && b.size() >= 14) {
            s.audio.formatTag = u16(b, 0);
            s.audio.channels = u16(b, 2);
            s.audio.sampleRate = u32(b, 4);
            s.audio.blockAlign = u16(b, 12);
            s.audio.bitsPerSample = b.size() >= 16 ? u16(b, 14) : 8;
        }
    }

    // Stream number from a chunk id "NNxx"; -1 if not a stream chunk.
    int streamOf(std::uint32_t id) const noexcept {
        const auto d0 = static_cast<int>(id & 0xFF) - '0', d1 = static_cast<int>((id >> 8) & 0xFF) - '0';
        if (d0 < 0 || d0 > 9 || d1 < 0 || d1 > 9) return -1;
        const int n = d0 * 10 + d1;
        return n < static_cast<int>(m_streams.size()) ? n : -1;
    }
    static bool isPaletteChange(std::uint32_t id) noexcept { return (id >> 16) == ('p' | 'c' << 8); }

    void scanList(std::uint64_t pos, std::uint64_t end, std::vector<std::vector<AviChunk>>& out) {
        while (auto h = headerAt(pos, end)) {
            if (h->id == LIST) {
                if (h->listType == fourcc("rec "))
                    scanList(pos + 12, std::min<std::uint64_t>(pos + 8 + h->size, end), out);
            } else if (const int s = streamOf(h->id); s >= 0 && !isPaletteChange(h->id)) {
                if (pos + 8 + h->size > m_fileSize)
                    break; // truncated file: drop the partial chunk
                out[s].push_back({pos + 8, h->size, h->id, false});
            }
            pos += 8 + std::uint64_t{h->size} + (h->size & 1);
        }
        // Without idx1 there are no keyframe flags: mark every non-empty chunk as one.
        for (auto& v : out)
            for (auto& c : v) c.keyframe = c.size > 0;
    }

    // idx1 offsets are relative either to the 'movi' fourcc (usual) or to the file start.
    bool loadIdx1() {
        const auto data = readAt(m_idx1->begin, static_cast<std::size_t>(m_idx1->end - m_idx1->begin));
        const std::size_t n = data.size() / 16;
        if (n == 0)
            return false;
        const std::uint64_t moviBase = m_moviLists.front().begin - 4;
        const auto matches = [&](std::uint64_t base, std::size_t i) {
            const std::uint32_t id = u32(data, 16 * i), off = u32(data, 16 * i + 8);
            const auto h = headerAt(base + off, m_fileSize);
            return h && h->id == id && h->size == u32(data, 16 * i + 12);
        };
        std::uint64_t base = 0;
        if (matches(moviBase, 0)) base = moviBase;
        else if (!matches(0, 0)) return false;

        std::vector<std::vector<AviChunk>> perStream(m_streams.size());
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t id = u32(data, 16 * i), flags = u32(data, 16 * i + 4);
            const std::uint32_t off = u32(data, 16 * i + 8), size = u32(data, 16 * i + 12);
            const int s = streamOf(id);
            if (s < 0 || isPaletteChange(id))
                continue;
            const std::uint64_t payload = base + off + 8;
            if (payload + size > m_fileSize)
                return false;
            perStream[s].push_back({payload, size, id, (flags & AVIIF_KEYFRAME) != 0});
        }
        // Spot-check the last entry too: a stale idx1 falls back to the movi walk.
        if (!matches(base, n - 1))
            return false;
        for (std::size_t i = 0; i < m_streams.size(); ++i)
            m_streams[i].chunks = std::move(perStream[i]);
        return true;
    }

    std::ifstream m_in;
    std::uint64_t m_fileSize = 0;
    std::uint32_t m_usPerFrame = 0;
    std::vector<AviStream> m_streams;
    std::vector<Range> m_moviLists;
    std::optional<Range> m_idx1;
    bool m_inStrl = false;
    bool m_usedIdx1 = false;
};

} // namespace gromada::video
