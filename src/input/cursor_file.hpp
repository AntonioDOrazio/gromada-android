#pragma once
// Windows cursor files (.CUR) as shipped in Gromada's `cursores/` folder, and drawing
// them on the XRGB8888 framebuffer.
//
// Format (Win32 ICO/CUR container, all little endian):
//   ICONDIR       reserved u16 = 0, type u16 (2 = cursor, 1 = icon), count u16
//   ICONDIRENTRY  width u8 (0 = 256), height u8, colors u8, reserved u8,
//                 hotspotX u16, hotspotY u16 (planes/bpp for icons), size u32, offset u32
//   image         BITMAPINFOHEADER (40 bytes, height = 2 * image height), optional palette
//                 (B,G,R,0 quads), XOR bitmap and 1 bpp AND mask, rows bottom-up, each row
//                 padded to 4 bytes.
// Gromada's five cursors are 32x32 1 bpp (black/white palette), 326 bytes each.
// Supported here: 1/4/8 bpp paletted, 24 bpp, 32 bpp (alpha, or AND mask when alpha is 0).
// PNG-compressed entries (Vista icons) are not used by the game and are rejected.
//
// Pixel semantics follow Windows: screen = (screen AND mask) XOR color, so
//   AND 0          -> opaque color
//   AND 1, XOR 0   -> transparent
//   AND 1, XOR !=0 -> screen XOR color (white = invert)

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gromada::input {

struct CursorTexel {
    enum Mode : std::uint8_t { Blend, Xor };
    std::uint32_t rgb = 0;   // 0x00RRGGBB
    std::uint8_t alpha = 0;  // Blend: 0 = transparent, 255 = opaque
    Mode mode = Blend;
};

struct CursorImage {
    int width = 0, height = 0;
    int hotX = 0, hotY = 0;
    std::vector<CursorTexel> texels; // row-major, top-down

    [[nodiscard]] bool empty() const noexcept { return texels.empty(); }
    [[nodiscard]] const CursorTexel& at(int x, int y) const { return texels[static_cast<std::size_t>(y) * width + x]; }
};

namespace detail {
inline std::uint32_t rd16(std::span<const std::uint8_t> d, std::size_t o) { return d[o] | (d[o + 1] << 8); }
inline std::uint32_t rd32(std::span<const std::uint8_t> d, std::size_t o) {
    return d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (std::uint32_t{d[o + 3]} << 24);
}
} // namespace detail

// Parses a .CUR (or .ICO) file. Picks the entry closest to 32x32 (then the deepest color).
// Returns nullopt for anything malformed or unsupported; never reads out of bounds.
inline std::optional<CursorImage> parseCursor(std::span<const std::uint8_t> d) {
    using detail::rd16;
    using detail::rd32;
    if (d.size() < 6 || rd16(d, 0) != 0)
        return std::nullopt;
    const auto type = rd16(d, 2), count = rd16(d, 4);
    if ((type != 1 && type != 2) || count == 0 || d.size() < 6 + 16 * std::size_t{count})
        return std::nullopt;

    // Entry choice: prefer 32x32, then the one with the most colors.
    std::size_t best = 0;
    int bestScore = -1;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t e = 6 + 16 * i;
        const int w = d[e] ? d[e] : 256, h = d[e + 1] ? d[e + 1] : 256;
        const int score = 100000 - std::abs(w - 32) * 100 - std::abs(h - 32) * 100 + (d[e + 2] == 0 ? 50 : d[e + 2] / 8);
        if (score > bestScore) { bestScore = score; best = e; }
    }
    const std::size_t e = best;
    const std::uint32_t size = rd32(d, e + 8), offset = rd32(d, e + 12);
    if (offset > d.size() || size > d.size() - offset || size < 40)
        return std::nullopt;
    const auto img = d.subspan(offset, size);
    if (img.size() >= 8 && img[0] == 0x89 && img[1] == 'P' && img[2] == 'N' && img[3] == 'G')
        return std::nullopt; // PNG entries: not supported

    const std::uint32_t hdrSize = rd32(img, 0);
    if (hdrSize < 40 || hdrSize > img.size())
        return std::nullopt;
    const auto bw = static_cast<std::int32_t>(rd32(img, 4));
    const auto bh2 = static_cast<std::int32_t>(rd32(img, 8)); // XOR + AND
    const auto bpp = rd16(img, 14);
    const auto compression = rd32(img, 16);
    std::uint32_t colorsUsed = rd32(img, 32);
    if (bw <= 0 || bw > 256 || bh2 <= 0 || bh2 % 2 != 0 || bh2 / 2 > 256 || compression != 0)
        return std::nullopt;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32)
        return std::nullopt;
    const int w = bw, h = bh2 / 2;

    std::size_t pos = hdrSize;
    std::vector<std::uint32_t> palette;
    if (bpp <= 8) {
        const std::uint32_t maxColors = 1u << bpp;
        if (colorsUsed == 0 || colorsUsed > maxColors) colorsUsed = maxColors;
        if (pos + 4 * std::size_t{colorsUsed} > img.size())
            return std::nullopt;
        for (std::uint32_t i = 0; i < colorsUsed; ++i, pos += 4)
            palette.push_back((std::uint32_t{img[pos + 2]} << 16) | (img[pos + 1] << 8) | img[pos]);
        palette.resize(maxColors, 0);
    }
    const std::size_t xorStride = ((static_cast<std::size_t>(w) * bpp + 31) / 32) * 4;
    const std::size_t andStride = ((static_cast<std::size_t>(w) + 31) / 32) * 4;
    const std::size_t xorPos = pos, andPos = pos + xorStride * h;
    const bool hasAnd = andPos + andStride * h <= img.size();
    if (xorPos + xorStride * h > img.size() || (!hasAnd && bpp != 32))
        return std::nullopt;

    CursorImage out;
    out.width = w;
    out.height = h;
    out.hotX = std::clamp<int>(static_cast<int>(rd16(d, e + 4)), 0, w - 1);
    out.hotY = std::clamp<int>(static_cast<int>(rd16(d, e + 6)), 0, h - 1);
    if (type == 1) out.hotX = out.hotY = 0; // icons carry planes/bpp there, not a hotspot
    out.texels.resize(static_cast<std::size_t>(w) * h);

    bool anyAlpha = false;
    if (bpp == 32)
        for (int y = 0; y < h && !anyAlpha; ++y)
            for (int x = 0; x < w; ++x)
                if (img[xorPos + y * xorStride + x * 4 + 3]) { anyAlpha = true; break; }

    for (int y = 0; y < h; ++y) {
        const std::size_t srcRow = static_cast<std::size_t>(h - 1 - y); // bottom-up
        const auto* xr = img.data() + xorPos + srcRow * xorStride;
        const auto* ar = hasAnd ? img.data() + andPos + srcRow * andStride : nullptr;
        for (int x = 0; x < w; ++x) {
            std::uint32_t rgb = 0;
            std::uint8_t alpha = 255;
            switch (bpp) {
            case 1: rgb = palette[(xr[x / 8] >> (7 - x % 8)) & 1]; break;
            case 4: rgb = palette[(xr[x / 2] >> (x % 2 ? 0 : 4)) & 15]; break;
            case 8: rgb = palette[xr[x]]; break;
            case 24: rgb = (std::uint32_t{xr[x * 3 + 2]} << 16) | (xr[x * 3 + 1] << 8) | xr[x * 3]; break;
            case 32:
                rgb = (std::uint32_t{xr[x * 4 + 2]} << 16) | (xr[x * 4 + 1] << 8) | xr[x * 4];
                alpha = xr[x * 4 + 3];
                break;
            }
            CursorTexel& t = out.texels[static_cast<std::size_t>(y) * w + x];
            if (bpp == 32 && anyAlpha) {
                t = {rgb, alpha, CursorTexel::Blend};
                continue;
            }
            const bool andBit = ar && ((ar[x / 8] >> (7 - x % 8)) & 1);
            if (!andBit)
                t = {rgb, 255, CursorTexel::Blend};
            else if (rgb == 0)
                t = {0, 0, CursorTexel::Blend};
            else
                t = {rgb, 255, CursorTexel::Xor};
        }
    }
    return out;
}

inline std::optional<CursorImage> loadCursorFile(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return std::nullopt;
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(f), {}};
    if (bytes.size() > (1u << 20))
        return std::nullopt;
    return parseCursor(bytes);
}

// Draws `img` with its hotspot at (x, y) on an XRGB8888 framebuffer, clipped.
inline void drawCursor(const CursorImage& img, int x, int y, std::uint32_t* fb, int width, int height,
                       std::size_t pitchBytes) {
    if (img.empty() || !fb)
        return;
    const int x0 = x - img.hotX, y0 = y - img.hotY;
    for (int cy = std::max(0, -y0); cy < img.height && y0 + cy < height; ++cy) {
        auto* row = reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(fb) + (y0 + cy) * pitchBytes);
        for (int cx = std::max(0, -x0); cx < img.width && x0 + cx < width; ++cx) {
            const CursorTexel& t = img.at(cx, cy);
            std::uint32_t& dst = row[x0 + cx];
            if (t.mode == CursorTexel::Xor) {
                dst = (dst ^ t.rgb) & 0xFFFFFFu;
            } else if (t.alpha == 255) {
                dst = t.rgb;
            } else if (t.alpha) {
                const std::uint32_t a = t.alpha, ia = 255 - a;
                auto ch = [&](int s) { return (((t.rgb >> s) & 255) * a + ((dst >> s) & 255) * ia + 127) / 255; };
                dst = (ch(16) << 16) | (ch(8) << 8) | ch(0);
            }
        }
    }
}

// The game's five cursors, by the name used in gromada.exe (`cursores\<name>.cur`).
enum class CursorShape : std::uint8_t { Arrow, Move, NoMove, Fight, Cycle, Count };

inline constexpr std::array<std::string_view, static_cast<std::size_t>(CursorShape::Count)> cursorFileNames{
    "arrow.cur", "move.cur", "nomove.cur", "fight.cur", "cycle.cur"};

namespace detail {
inline bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::ranges::equal(a, b, [](unsigned char x, unsigned char y) {
               return std::tolower(x) == std::tolower(y);
           });
}
// Case-insensitive child lookup: the game names `cursores\arrow.cur`, the CD ships ARROW.CUR,
// and Android file systems are case-sensitive.
inline std::optional<std::filesystem::path> findChild(const std::filesystem::path& dir, std::string_view name) {
    std::error_code ec;
    if (std::filesystem::exists(dir / name, ec))
        return dir / name;
    for (std::filesystem::directory_iterator it{dir, ec}, end; !ec && it != end; it.increment(ec))
        if (iequals(it->path().filename().string(), name))
            return it->path();
    return std::nullopt;
}

// Fallback when the game folder has no cursors: a plain arrow (our own drawing), hotspot at the tip.
inline CursorImage builtinArrow() {
    static constexpr std::string_view rows[] = {
        "o", "oo", "o#o", "o##o", "o###o", "o####o", "o#####o", "o######o", "o#######o",
        "o########o", "o#####oooo", "o##o##o", "o#o o##o", "oo  o##o", "o    o##o", "     o##o", "      oo"};
    CursorImage img;
    img.width = 11;
    img.height = static_cast<int>(std::size(rows));
    img.texels.resize(static_cast<std::size_t>(img.width) * img.height);
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < static_cast<int>(rows[y].size()); ++x) {
            const char c = rows[y][x];
            if (c == 'o') img.texels[y * img.width + x] = {0x000000, 255, CursorTexel::Blend};
            if (c == '#') img.texels[y * img.width + x] = {0xFFFFFF, 255, CursorTexel::Blend};
        }
    return img;
}
} // namespace detail

class CursorSet {
public:
    CursorSet() { reset(); }

    // Loads `<gameDir>/cursores/*.cur` (any case). Missing or broken files fall back to the
    // built-in arrow. Returns how many original cursors were loaded (0..5).
    int load(const std::filesystem::path& gameDir) {
        reset();
        int loaded = 0;
        const auto dir = detail::findChild(gameDir, "cursores");
        if (!dir)
            return 0;
        for (std::size_t i = 0; i < m_images.size(); ++i)
            if (const auto file = detail::findChild(*dir, cursorFileNames[i]))
                if (auto img = loadCursorFile(*file)) {
                    m_images[i] = std::move(*img);
                    ++loaded;
                }
        return loaded;
    }

    void reset() { m_images.fill(detail::builtinArrow()); }
    [[nodiscard]] const CursorImage& get(CursorShape s) const { return m_images[static_cast<std::size_t>(s)]; }

private:
    std::array<CursorImage, static_cast<std::size_t>(CursorShape::Count)> m_images;
};

} // namespace gromada::input
