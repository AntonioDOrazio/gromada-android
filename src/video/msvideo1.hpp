#pragma once
// Microsoft Video 1 ("CRAM" / "MSVC" / "WHAM") decoder, 8-bit palettised and 16-bit RGB555.
// Written from the public description of the format (MultimediaWiki, "Microsoft Video 1").
//
// The picture is coded in 4x4 blocks, left to right, block rows from the BOTTOM of the
// picture (DIB order). Each block starts with a little-endian 16-bit word:
//   - 0x84xx..0x87xx: skip ((word - 0x8400) blocks keep the previous frame's content);
//   - 16-bit mode: bit 15 clear = 2 or 8 colour block, flags = word (bit 0 = bottom-left
//     pixel, rows bottom-up), followed by colour words; the 8 colour variant (bit 15 of the
//     first colour set) has a colour pair per 2x2 quadrant. Bit 15 set = 1 colour block
//     (the word itself is the colour);
//   - 8-bit mode: high byte < 0x80 = 2 colours, >= 0x90 = 8 colours, otherwise 1 colour
//     (the low byte is the palette index).
// A set flag bit selects the FIRST colour of the pair.
//
// Output: top-down 16-bit RGB555 (16-bit mode) or palette indices (8-bit mode).

#include <cstdint>
#include <span>
#include <vector>

namespace gromada::video {

class MsVideo1Decoder {
public:
    // bitsPerPixel: 8 (palettised) or 16 (RGB555).
    MsVideo1Decoder(int width, int height, int bitsPerPixel)
        : m_width{width}, m_height{height}, m_8bit{bitsPerPixel == 8} {
        if (m_8bit) m_pixels8.assign(static_cast<std::size_t>(width) * height, 0);
        else m_pixels16.assign(static_cast<std::size_t>(width) * height, 0);
    }

    [[nodiscard]] int width() const noexcept { return m_width; }
    [[nodiscard]] int height() const noexcept { return m_height; }
    [[nodiscard]] bool palettised() const noexcept { return m_8bit; }
    [[nodiscard]] std::span<const std::uint16_t> rgb555() const noexcept { return m_pixels16; }
    [[nodiscard]] std::span<const std::uint8_t> indices() const noexcept { return m_pixels8; }

    // Decodes one frame over the previous one (skipped blocks keep their content).
    // Returns false if the data ends before the last block (what was decoded is kept).
    bool decode(std::span<const std::uint8_t> data) {
        return m_8bit ? decodeBlocks<true>(data) : decodeBlocks<false>(data);
    }

private:
    template <bool Pal>
    bool decodeBlocks(std::span<const std::uint8_t> data) {
        const int blocksWide = m_width / 4, blocksHigh = m_height / 4;
        std::size_t p = 0;
        int skip = 0;
        const auto need = [&](std::size_t n) { return p + n <= data.size(); };
        for (int by = blocksHigh - 1; by >= 0; --by) {
            for (int bx = 0; bx < blocksWide; ++bx) {
                if (skip > 0) {
                    --skip;
                    continue;
                }
                if (!need(2))
                    return false;
                const std::uint8_t a = data[p], b = data[p + 1];
                p += 2;
                // (Some encoders end the frame with a 0x0000 word; it only ever appears
                // where no block is left to decode, so it needs no special case.)
                if ((b & 0xFC) == 0x84) {
                    skip = ((b - 0x84) << 8) + a - 1; // this block is the first skipped one
                    continue;
                }
                const unsigned flags = static_cast<unsigned>(b) << 8 | a;
                std::uint16_t colors[8];
                if constexpr (Pal) {
                    if (b < 0x80) {
                        if (!need(2)) return false;
                        colors[0] = data[p]; colors[1] = data[p + 1];
                        p += 2;
                        fill2(bx, by, flags, colors);
                    } else if (b >= 0x90) {
                        if (!need(8)) return false;
                        for (int i = 0; i < 8; ++i) colors[i] = data[p + i];
                        p += 8;
                        fill8(bx, by, flags, colors);
                    } else {
                        fill1(bx, by, a);
                    }
                } else {
                    if (b < 0x80) {
                        if (!need(4)) return false;
                        colors[0] = static_cast<std::uint16_t>(data[p] | data[p + 1] << 8);
                        colors[1] = static_cast<std::uint16_t>(data[p + 2] | data[p + 3] << 8);
                        p += 4;
                        if (colors[0] & 0x8000) {
                            if (!need(12)) return false;
                            for (int i = 2; i < 8; ++i, p += 2)
                                colors[i] = static_cast<std::uint16_t>(data[p] | data[p + 1] << 8);
                            fill8(bx, by, flags, colors);
                        } else {
                            fill2(bx, by, flags, colors);
                        }
                    } else {
                        fill1(bx, by, static_cast<std::uint16_t>(flags));
                    }
                }
            }
        }
        return true;
    }

    template <typename F>
    void forBlock(int bx, int by, F&& pixel) {
        // Block row `by` counts from the top of the output picture; inside the block,
        // y = 0 is the bottom row (bitstream order).
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                pixel(static_cast<std::size_t>(by * 4 + 3 - y) * m_width + bx * 4 + x, x, y);
    }
    void put(std::size_t i, std::uint16_t c) {
        if (m_8bit) m_pixels8[i] = static_cast<std::uint8_t>(c);
        else m_pixels16[i] = static_cast<std::uint16_t>(c & 0x7FFF);
    }
    void fill1(int bx, int by, std::uint16_t c) {
        forBlock(bx, by, [&](std::size_t i, int, int) { put(i, c); });
    }
    void fill2(int bx, int by, unsigned flags, const std::uint16_t* c) {
        forBlock(bx, by, [&](std::size_t i, int x, int y) { put(i, c[(flags >> (y * 4 + x) & 1) ^ 1]); });
    }
    void fill8(int bx, int by, unsigned flags, const std::uint16_t* c) {
        forBlock(bx, by, [&](std::size_t i, int x, int y) {
            const int quadrant = (y & 2) << 1 | (x & 2); // pairs: bottom-left, bottom-right, top-left, top-right
            put(i, c[quadrant + ((flags >> (y * 4 + x) & 1) ^ 1)]);
        });
    }

    int m_width, m_height;
    bool m_8bit;
    std::vector<std::uint16_t> m_pixels16;
    std::vector<std::uint8_t> m_pixels8;
};

} // namespace gromada::video
