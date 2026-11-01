#pragma once
// Intel Indeo 3 ("IV31" / "IV32") decoder.
//
// Bitstream summary (see docs/video.md for the full description):
//  - every frame starts with a 16-byte "OS header" (frame number, a word, a checksum and a
//    size; checksum = xor of the three others and 'FRMH') and a 32-byte bitstream header
//    (version 32, flags, data size in bits, codebook offset, picture size, offsets of the
//    Y, V and U plane data), followed by the 16-byte alternate quantiser table;
//  - pictures are YUV 4:1:0 (chroma 1/4 x 1/4) with 7-bit samples, decoded into one of two
//    buffers (flag bit 9); the other buffer is the motion-compensation reference;
//  - each plane is a binary tree of cells (units of 4x4 pixels) coded with 2-bit codes:
//    horizontal / vertical split, then in the "motion" tree INTRA or INTER(+vector index)
//    and in the "VQ" tree NULL (copy) or DATA (a vector-quantised cell);
//  - VQ cells: one mode byte (mode, codebook) and one code per row of 4 pixels: a "dyad"
//    (two codes, one per pixel pair) or a "quad" (both pairs in one byte) of deltas added
//    to the row above (intra) or to the motion-compensated reference (inter), plus RLE
//    escapes (0xF9..0xFF) that copy rows / blocks. Modes 3/4 (intra) and 10/11 code every
//    other row (and mode 10 every other column) and interpolate the rest.
//
// Provenance: written for this project without copying code, but the algorithm follows
// FFmpeg's decoder (libavcodec/indeo3.c, LGPL-2.1+), the most complete description of the
// format, including the quirks needed for identical output: see "Provenienza e licenze" in
// docs/video.md. Build with -DGROMADA_NO_INDEO3 to leave this decoder out.
//
// Implementation notes:
//  - the tables are Intel's (indeo3_tables.hpp);
//  - behaviour matches FFmpeg's decoder bit for bit on the game's movies (tests/video_data_test),
//    including its quirks: the requantisation of the prediction row is done in place
//    (also on the reference picture for INTER cells), RLE "skip" codes in INTRA cells keep
//    whatever the buffer holds, sync (16-byte) frames repeat the previous picture.

#include "video/indeo3_tables.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace gromada::video {

namespace indeo3_detail {

struct VqTable {
    unsigned numDyads = 0; // codes 0..numDyads-1 are dyads, numDyads..247 quads
    unsigned quadBase = 1; // quad code q -> dyads (q / quadBase, q % quadBase)
    bool swapQuads = false;
    std::array<std::int32_t, 256> delta2{};  // 2 pixels (16-bit little-endian word)
    std::array<std::uint32_t, 256> delta4{}; // 4 pixels: both deltas doubled horizontally (mode 10)
};

struct Tables {
    std::array<VqTable, 24> vq;
    std::array<std::array<std::uint8_t, 128>, 8> requant;
};

// Requantisation tables: section i rounds a 7-bit sample to a multiple of (i + 2), shifted
// by a per-section offset. Intel's table follows the formula except for a few entries,
// patched below (checked entry by entry against the table of the original codec).
inline std::array<std::array<std::uint8_t, 128>, 8> buildRequant() {
    static constexpr int offsets[8] = {1, 1, 2, -3, -3, 3, 4, 4};
    static constexpr int deltas[8] = {0, 1, 0, 4, 4, 1, 0, 1};
    std::array<std::array<std::uint8_t, 128>, 8> t{};
    for (int i = 0; i < 8; ++i) {
        const int step = i + 2;
        for (int j = 0; j < 128; ++j)
            t[i][j] = static_cast<std::uint8_t>((j + offsets[i]) / step * step + deltas[i]); // C division: truncates
    }
    // values that would leave the 7-bit range, and two oddities of Intel's table
    t[0][127] = 126;
    t[1][119] = t[1][120] = 118;
    t[2][126] = t[2][127] = 124;
    t[6][124] = t[6][125] = t[6][126] = t[6][127] = 120;
    t[1][7] = 10;
    t[4][8] = 10;
    return t;
}

inline Tables buildTables() {
    Tables t{};
    std::size_t p = 0;
    for (auto& tab : t.vq) {
        const unsigned count = kIndeo3VqSeed[p++];
        const auto expand = static_cast<std::int8_t>(kIndeo3VqSeed[p + 2 * count]);
        tab.numDyads = count;
        tab.quadBase = static_cast<unsigned>(std::abs(expand));
        tab.swapQuads = expand < 0;
        for (unsigned i = 0; i < count; ++i) {
            const int a = static_cast<std::int8_t>(kIndeo3VqSeed[p + 2 * i]);     // left pixel
            const int b = static_cast<std::int8_t>(kIndeo3VqSeed[p + 2 * i + 1]); // right pixel
            // Signed arithmetic on purpose: a negative low delta borrows from the high byte,
            // exactly like the 16/32-bit SWAR additions of the reference decoders.
            tab.delta2[i] = a + b * 256;
            tab.delta4[i] = static_cast<std::uint32_t>(a + a * 256) + static_cast<std::uint32_t>(b + b * 256) * 65536u;
        }
        p += 2 * count + 1;
    }
    t.requant = buildRequant();
    return t;
}

inline const Tables& tables() {
    static const Tables t = buildTables();
    return t;
}

inline std::uint32_t rd16(const std::uint8_t* p) noexcept { return p[0] | p[1] << 8; }
inline std::uint32_t rd32(const std::uint8_t* p) noexcept {
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}
inline void wr16(std::uint8_t* p, std::uint32_t v) noexcept { p[0] = std::uint8_t(v); p[1] = std::uint8_t(v >> 8); }
inline void wr32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = std::uint8_t(v); p[1] = std::uint8_t(v >> 8); p[2] = std::uint8_t(v >> 16); p[3] = std::uint8_t(v >> 24);
}
// ABCD -> AACC (top row of a mode 10 cell: the even pixels are doubled)
inline std::uint32_t evenPixels(std::uint32_t v) noexcept { v &= 0x00FF00FFu; return v | v << 8; }
// rounding-down average of 7-bit samples
inline void average(std::uint8_t* dst, const std::uint8_t* a, const std::uint8_t* b, int n) noexcept {
    for (int i = 0; i < n; ++i) dst[i] = static_cast<std::uint8_t>((a[i] + b[i]) >> 1);
}

} // namespace indeo3_detail

class Indeo3Decoder {
public:
    enum class Status { Decoded, Repeat, Error };

    struct PlaneView {
        const std::uint8_t* data; // 7-bit samples (0..127)
        int width, height;
        std::ptrdiff_t pitch;
    };

    Indeo3Decoder() = default;
    Indeo3Decoder(int width, int height) { allocate(width, height); }

    [[nodiscard]] int width() const noexcept { return m_width; }
    [[nodiscard]] int height() const noexcept { return m_height; }
    [[nodiscard]] const std::string& lastError() const noexcept { return m_error; }

    // Plane 0 = Y, 1 = U, 2 = V of the last decoded picture.
    [[nodiscard]] PlaneView plane(int p) const noexcept {
        const Plane& pl = m_planes[p];
        return {pl.buffers[m_shown].data() + pl.pitch, pl.width, pl.height, pl.pitch};
    }

    // Decodes one compressed frame. Repeat = sync frame (the previous picture stays).
    // On Error the previous picture stays visible and lastError() tells why.
    Status decode(std::span<const std::uint8_t> frame) {
        using namespace indeo3_detail;
        if (frame.size() < 16 + 16)
            return fail("frame too short");
        const std::uint8_t* os = frame.data();
        const std::uint32_t frameNum = rd32(os), word2 = rd32(os + 4), checksum = rd32(os + 8), osSize = rd32(os + 12);
        if ((frameNum ^ word2 ^ osSize ^ 0x46524D48u /* 'FRMH' */) != checksum)
            return fail("OS header checksum mismatch");

        const std::uint8_t* bs = os + 16; // bitstream header; plane offsets are relative to it
        if (rd16(bs) != 32)
            return fail("unsupported bitstream version");
        const std::uint32_t flags = rd16(bs + 2);
        std::size_t dataSize = (std::size_t{rd32(bs + 4)} + 7) >> 3;
        m_cbOffset = bs[8];
        if (dataSize == 16)
            return Status::Repeat; // sync frame: nothing coded
        if (frame.size() < 16 + 48)
            return fail("frame too short");
        dataSize = std::min(dataSize, frame.size() - 16);
        const int height = static_cast<int>(rd16(bs + 12)), width = static_cast<int>(rd16(bs + 14));
        if (width < 16 || height < 16 || width > 1024 || height > 1024 || (width & 3) || (height & 3))
            return fail("invalid picture size");
        if (width != m_width || height != m_height)
            allocate(width, height);
        if (flags & BS_8BIT_PEL)
            return fail("8-bit pixel format not supported");
        if (flags & (BS_MV_X_HALF | BS_MV_Y_HALF))
            return fail("half-pel motion vectors not supported");

        const std::uint32_t starts[3] = {rd32(bs + 16), rd32(bs + 20), rd32(bs + 24)}; // Y, V, U
        std::size_t ends[3];
        for (int j = 0; j < 3; ++j) {
            ends[j] = dataSize;
            for (int i = 0; i < 3; ++i)
                if (starts[i] > starts[j] && starts[i] < ends[j]) ends[j] = starts[i];
            if (starts[j] >= ends[j])
                return fail("invalid plane offsets");
        }
        std::memcpy(m_altQuant, bs + 32, 16);

        m_cur = (flags >> BS_BUFFER) & 1;
        static constexpr int planeOf[3] = {0, 2, 1}; // header order is Y, V, U
        static constexpr int decodeOrder[3] = {0, 2, 1}; // Y, then U, then V
        for (const int j : decodeOrder) {
            const int strip = j == 0 ? 40 : 10; // widest cell before a forced vertical split
            if (!decodePlane(m_planes[planeOf[j]], bs + starts[j], ends[j] - starts[j], strip))
                return Status::Error;
        }
        m_shown = m_cur;
        return Status::Decoded;
    }

private:
    static constexpr std::uint32_t BS_8BIT_PEL = 1 << 1, BS_MV_Y_HALF = 1 << 4, BS_MV_X_HALF = 1 << 5;
    static constexpr int BS_BUFFER = 9;
    static constexpr int MaxTreeDepth = 20;
    enum Code { H_SPLIT = 0, V_SPLIT = 1, INTRA_NULL = 2, INTER_DATA = 3 };

    struct Plane {
        int width = 0, height = 0;
        std::ptrdiff_t pitch = 0;
        // Two pictures; row 0 of each is the prediction row above the picture (starts at 64).
        std::vector<std::uint8_t> buffers[2];
        std::uint8_t* pixels(int b) noexcept { return buffers[b].data() + pitch; }
    };

    struct Cell {
        int x, y, w, h;               // in 4x4 units
        bool vqTree;                  // false: motion tree, true: VQ tree
        const std::int8_t* mv;        // {dy, dx} or nullptr for INTRA
    };

    // Tree codes (2 bits, MSB first) and cell data (bytes) share one byte cursor: a tree
    // byte is consumed whole when its first code is read, data bytes follow it.
    struct Reader {
        const std::uint8_t* pos;
        const std::uint8_t* end;
        unsigned treeByte = 0;
        int treeBits = 0;
        bool code(int& out) noexcept {
            if (treeBits == 0) {
                if (pos >= end) return false;
                treeByte = *pos++;
                treeBits = 8;
            }
            treeBits -= 2;
            out = static_cast<int>((treeByte >> treeBits) & 3);
            return true;
        }
    };

    Status fail(const char* why) {
        m_error = why;
        return Status::Error;
    }
    bool failPlane(const char* why) {
        m_error = why;
        return false;
    }

    void allocate(int width, int height) {
        m_width = width;
        m_height = height;
        const int lw = (width + 1) & ~1, lh = (height + 1) & ~1;
        for (int p = 0; p < 3; ++p) {
            Plane& pl = m_planes[p];
            pl.width = p == 0 ? lw : (((lw >> 2) + 3) & ~3);
            pl.height = p == 0 ? lh : (((lh >> 2) + 3) & ~3);
            pl.pitch = (pl.width + 15) & ~15;
            for (auto& b : pl.buffers) {
                b.assign(static_cast<std::size_t>(pl.pitch) * (pl.height + 1), 0);
                std::memset(b.data(), 0x40, static_cast<std::size_t>(pl.pitch));
            }
        }
        m_shown = m_cur = 0;
    }

    bool decodePlane(Plane& pl, const std::uint8_t* data, std::size_t size, int strip) {
        if (size < 4)
            return failPlane("plane data too short");
        const std::uint32_t numVectors = indeo3_detail::rd32(data);
        if (numVectors > 256 || numVectors * 2 > size - 4)
            return failPlane("invalid motion vector count");
        m_plane = &pl;
        m_vectors = reinterpret_cast<const std::int8_t*>(data + 4);
        m_numVectors = numVectors;
        m_strip = strip;
        m_reader = Reader{data + 4 + numVectors * 2, data + size};
        return parseTree({0, 0, pl.width >> 2, pl.height >> 2, false, nullptr}, MaxTreeDepth);
    }

    static int splitSize(int s) noexcept { return s > 2 ? ((s + 2) >> 2) << 1 : 1; }

    // Parses the subtree of `c`. A split decodes the first part recursively and goes on
    // with the second part in this loop.
    bool parseTree(Cell c, int depth) {
        if (depth <= 0)
            return failPlane("cell tree too deep");
        for (;;) {
            int code;
            if (!m_reader.code(code))
                return failPlane("tree data exhausted");
            switch (code) {
            case H_SPLIT:
            case V_SPLIT: {
                Cell first = c;
                if (code == H_SPLIT) {
                    first.h = splitSize(c.h);
                    c.y += first.h;
                    c.h -= first.h;
                } else {
                    first.w = c.w > m_strip ? (c.w <= 2 * m_strip ? 1 : 2) * m_strip : splitSize(c.w);
                    c.x += first.w;
                    c.w -= first.w;
                }
                if (c.w <= 0 || c.h <= 0)
                    return failPlane("invalid split");
                if (!parseTree(first, depth - 1))
                    return false;
                break;
            }
            case INTRA_NULL:
                if (!c.vqTree) { // motion tree: INTRA cell, go on with its VQ tree
                    c.vqTree = true;
                    break;
                }
                // VQ tree: NULL cell, followed by a 2-bit type (0 = copy, 1 = "skip",
                // never used by encoders and handled as a copy)
                if (!m_reader.code(code) || code >= 2)
                    return failPlane("invalid VQ NULL code");
                return checkCell(c) && copyCell(c);
            case INTER_DATA:
                if (!c.vqTree) { // motion tree: INTER cell, one byte = motion vector index
                    if (m_reader.pos >= m_reader.end)
                        return failPlane("cell data exhausted");
                    const unsigned index = *m_reader.pos++;
                    if (index >= m_numVectors)
                        return failPlane("motion vector index out of range");
                    c.mv = m_vectors + 2 * index;
                    c.vqTree = true;
                    break;
                }
                return checkCell(c) && decodeCell(c);
            }
        }
    }

    bool checkCell(const Cell& c) {
        if (c.x + c.w > (m_plane->width >> 2) || c.y + c.h > (m_plane->height >> 2))
            return failPlane("cell outside the picture");
        return true;
    }

    // Source of the motion-compensated copy of `c` in the reference picture, or nullptr.
    const std::uint8_t* motionSource(const Cell& c) {
        const int dy = c.mv ? c.mv[0] : 0, dx = c.mv ? c.mv[1] : 0;
        // -1: the prediction row above the picture may be referenced
        if (c.y * 4 + dy < -1 || c.x * 4 + dx < 0 || (c.y + c.h) * 4 + dy > m_plane->height ||
            (c.x + c.w) * 4 + dx > m_plane->width) {
            failPlane("motion vector points outside the picture");
            return nullptr;
        }
        return m_plane->pixels(m_cur ^ 1) + (c.y * 4 + dy) * m_plane->pitch + c.x * 4 + dx;
    }

    bool copyCell(const Cell& c) {
        const std::uint8_t* src = motionSource(c);
        if (!src)
            return false;
        std::uint8_t* dst = m_plane->pixels(m_cur) + c.y * 4 * m_plane->pitch + c.x * 4;
        for (int y = 0; y < c.h * 4; ++y)
            std::memcpy(dst + y * m_plane->pitch, src + y * m_plane->pitch, static_cast<std::size_t>(c.w) * 4);
        return true;
    }

    bool decodeCell(const Cell& c) {
        using namespace indeo3_detail;
        const Tables& tabs = tables();
        Reader& rd = m_reader;
        if (rd.pos >= rd.end)
            return failPlane("cell data exhausted");
        const unsigned modeByte = *rd.pos++;
        const int mode = static_cast<int>(modeByte >> 4);
        unsigned vq = modeByte & 15;

        const std::ptrdiff_t pitch = m_plane->pitch;
        std::uint8_t* block = m_plane->pixels(m_cur) + c.y * 4 * pitch + c.x * 4;
        std::uint8_t* ref = nullptr; // prediction: row above (INTRA) or reference cell (INTER)
        if (!c.mv) {
            ref = block - pitch;
        } else if (mode >= 10) {
            if (!copyCell(c)) // modes 10/11 INTER add the deltas over the motion-compensated cell
                return false;
        } else {
            const std::uint8_t* src = motionSource(c);
            if (!src)
                return false;
            ref = const_cast<std::uint8_t*>(src);
        }

        unsigned primary, secondary;
        if (mode == 1 || mode == 4) { // two codebooks, alternating by row
            const unsigned q = m_altQuant[vq];
            primary = (q >> 4) + m_cbOffset;
            secondary = (q & 15) + m_cbOffset;
        } else {
            vq += m_cbOffset;
            primary = secondary = vq;
        }
        if (primary >= 24 || secondary >= 24)
            return failPlane("invalid codebook index");
        const VqTable* delta[2] = {&tabs.vq[secondary], &tabs.vq[primary]}; // even rows, odd rows
        const bool swap[2] = {tabs.vq[secondary].swapQuads, tabs.vq[primary].swapQuads};

        // Coarse codebooks: requantise the prediction row so the deltas cannot overflow.
        if (vq >= 8 && ref) {
            const auto& rq = tabs.requant[vq & 7];
            for (int x = 0; x < c.w * 4; ++x)
                ref[x] = rq[ref[x] & 127];
        }

        switch (mode) {
        case 0: case 1: // 4x4 blocks
            return cellData(c, block, ref, 0, 0, mode, delta, swap);
        case 3: case 4: // 4x8 blocks: odd rows coded, even rows interpolated (INTRA only)
            if (c.mv)
                return failPlane("mode 3/4 in an INTER cell");
            return cellData(c, block, ref, 0, 1, mode, delta, swap);
        case 10: // 8x8 blocks
            return cellData(c, block, ref, 1, 1, mode, delta, swap);
        case 11: // 4x8 blocks, INTER only
            if (!c.mv)
                return failPlane("mode 11 in an INTRA cell");
            return cellData(c, block, ref, 0, 1, mode, delta, swap);
        default:
            return failPlane("unsupported cell mode");
        }
    }

    bool cellData(const Cell& c, std::uint8_t* block, const std::uint8_t* refBlock, int hZoom, int vZoom, int mode,
                  const indeo3_detail::VqTable* const delta[2], const bool swap[2]) {
        using namespace indeo3_detail;
        const std::ptrdiff_t pitch = m_plane->pitch;
        const std::ptrdiff_t rowAdvance = (pitch << (2 + vZoom)) - c.w * 4;
        const bool intra = c.mv == nullptr;
        const bool intra10 = mode == 10 && intra;
        Reader& rd = m_reader;
        const std::uint8_t* const end = rd.end;
        if ((c.h & vZoom) || (c.w & hZoom))
            return failPlane("cell size not a multiple of the block size");

        // Mode 10 INTRA: fills n rows from the 8 pixels above; on the first row of the cell
        // the even pixels are doubled and the first row is interpolated.
        const auto fill8 = [&](std::uint8_t* dst, const std::uint8_t* ref, int rows, bool topOfCell) {
            std::uint8_t pix[8];
            std::memcpy(pix, ref, 8);
            if (topOfCell) {
                wr32(pix, evenPixels(rd32(pix)));
                wr32(pix + 4, evenPixels(rd32(pix + 4)));
                for (int i = 1; i < rows; ++i) std::memcpy(dst + i * pitch, pix, 8);
                average(dst, ref, dst + pitch, 8);
            } else {
                for (int i = 0; i < rows; ++i) std::memcpy(dst + i * pitch, pix, 8);
            }
        };
        const auto copyRows = [&](std::uint8_t* dst, const std::uint8_t* ref, int rows) {
            for (int i = 0; i < rows; ++i) std::memcpy(dst + i * pitch, ref + i * pitch, 4);
        };

        int rleBlocks = 0;
        bool skipFlag = false;
        bool firstRow = true;
        for (int y = 0; y < c.h; y += 1 + vZoom, firstRow = false) {
            for (int x = 0; x < c.w; x += 1 + hZoom) {
                std::uint8_t* dst = block;
                const std::uint8_t* ref = refBlock;
                if (rleBlocks > 0) { // repeated block
                    if (mode <= 4) {
                        if (!intra || !skipFlag) copyRows(dst, ref, 4 << vZoom);
                    } else if (intra10) {
                        fill8(dst, ref, 8, firstRow);
                    }
                    --rleBlocks;
                } else {
                    for (int line = 0; line < 4;) {
                        int numLines = 1;
                        const bool topOfCell = firstRow && line == 0;
                        const VqTable& tab = mode <= 4 ? *delta[line & 1] : *delta[1];
                        if (rd.pos >= end)
                            return failPlane("cell data exhausted");
                        unsigned code = *rd.pos++;
                        if (code < 248) {
                            unsigned d1, d2;
                            if (code < tab.numDyads) { // dyad: two bytes, one per pixel pair
                                if (rd.pos >= end)
                                    return failPlane("cell data exhausted");
                                d1 = *rd.pos++;
                                d2 = code;
                                if (d1 >= tab.numDyads)
                                    return failPlane("invalid dyad");
                            } else { // quad: both pairs in one byte
                                code -= tab.numDyads;
                                d1 = code / tab.quadBase;
                                d2 = code % tab.quadBase;
                                if (swap[line & 1]) std::swap(d1, d2);
                            }
                            if (mode <= 4) {
                                std::uint8_t* out = dst + (vZoom ? pitch : 0);
                                wr16(out, (rd16(ref) + tab.delta2[d1]) & 0x7F7F);
                                wr16(out + 2, (rd16(ref + 2) + tab.delta2[d2]) & 0x7F7F);
                                if (vZoom) { // the skipped row: copy at the top of the picture, else interpolate
                                    if (topOfCell && c.y == 0) std::memcpy(dst, out, 4);
                                    else average(dst, ref, out, 4);
                                }
                            } else if (intra10) {
                                std::uint8_t* out = dst + pitch;
                                std::uint32_t p0 = rd32(ref), p1 = rd32(ref + 4);
                                if (topOfCell) { p0 = evenPixels(p0); p1 = evenPixels(p1); }
                                wr32(out, (p0 + tab.delta4[d1]) & 0x7F7F7F7Fu);
                                wr32(out + 4, (p1 + tab.delta4[d2]) & 0x7F7F7F7Fu);
                                if (topOfCell && c.y == 0) std::memcpy(dst, out, 8);
                                else average(dst, ref, out, 8);
                            } else { // modes 10/11 INTER: deltas on both rows of the copied cell
                                for (std::uint8_t* row : {dst, dst + pitch}) {
                                    if (mode == 10) {
                                        wr32(row, (rd32(row) + tab.delta4[d1]) & 0x7F7F7F7Fu);
                                        wr32(row + 4, (rd32(row + 4) + tab.delta4[d2]) & 0x7F7F7F7Fu);
                                    } else {
                                        wr16(row, (rd16(row) + tab.delta2[d1]) & 0x7F7F);
                                        wr16(row + 2, (rd16(row + 2) + tab.delta2[d2]) & 0x7F7F);
                                    }
                                }
                            }
                        } else { // RLE escapes
                            bool copy = false;
                            switch (code) {
                            case 0xFC: // rest of this block and the next one unchanged
                                skipFlag = false;
                                rleBlocks = 1;
                                code = 0xFD;
                                [[fallthrough]];
                            case 0xFD: case 0xFE: case 0xFF: // copy up to row 4 / 3 / 2 of the block
                                numLines = 257 - static_cast<int>(code) - line;
                                if (numLines <= 0)
                                    return failPlane("invalid RLE code");
                                copy = true;
                                break;
                            case 0xFB: { // rest of the block + a run of blocks
                                if (rd.pos >= end)
                                    return failPlane("cell data exhausted");
                                const unsigned count = *rd.pos++;
                                rleBlocks = static_cast<int>(count & 0x1F) - 1;
                                if (count >= 64 || rleBlocks < 0)
                                    return failPlane("invalid RLE counter");
                                skipFlag = (count & 0x20) != 0;
                                numLines = 4 - line;
                                copy = mode >= 10 || !intra || !skipFlag;
                                break;
                            }
                            case 0xF9: // this block and the next one skipped
                                skipFlag = true;
                                rleBlocks = 1;
                                [[fallthrough]];
                            case 0xFA: // this block skipped
                                if (line)
                                    return failPlane("invalid RLE code");
                                numLines = 4;
                                copy = !intra && mode <= 4;
                                break;
                            default:
                                return failPlane("unsupported RLE code");
                            }
                            if (copy) {
                                if (mode <= 4) copyRows(dst, ref, numLines << vZoom);
                                else if (intra10) fill8(dst, ref, numLines << 1, topOfCell);
                            }
                        }
                        line += numLines;
                        const std::ptrdiff_t step = pitch * (numLines << vZoom);
                        dst += step;
                        if (ref) ref += step;
                    }
                }
                block += 4 << hZoom;
                if (refBlock) refBlock += 4 << hZoom;
            }
            block += rowAdvance;
            if (refBlock) refBlock += rowAdvance;
        }
        return true;
    }

    int m_width = 0, m_height = 0;
    Plane m_planes[3];
    int m_cur = 0, m_shown = 0;
    unsigned m_cbOffset = 0;
    std::uint8_t m_altQuant[16]{};
    std::string m_error;
    // per-plane decoding state
    Plane* m_plane = nullptr;
    const std::int8_t* m_vectors = nullptr;
    unsigned m_numVectors = 0;
    int m_strip = 40;
    Reader m_reader{nullptr, nullptr};
};

} // namespace gromada::video
