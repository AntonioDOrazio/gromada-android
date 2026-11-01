#pragma once
// Software rendering of a Level into an RGBA8 framebuffer, then conversion to the
// XRGB8888 layout libretro expects. RetroArch takes it from there (Vulkan + shaders).

#include "core/std.hpp"
#include "engine/level.hpp"

namespace gromada {

class Renderer {
public:
    static constexpr int MaxWidth = 854;  // 16:9 at 480 lines
    static constexpr int Height = 480;    // original game height

    Renderer() : m_rgba(MaxWidth * Height), m_xrgb(MaxWidth * Height) {}

    void setWidth(int width) noexcept { m_width = std::clamp(width, 320, MaxWidth); }
    [[nodiscard]] int width() const noexcept { return m_width; }
    [[nodiscard]] int height() const noexcept { return Height; }
    [[nodiscard]] const std::uint32_t* xrgb() const noexcept { return m_xrgb.data(); }
    [[nodiscard]] std::size_t pitch() const noexcept { return static_cast<std::size_t>(m_width) * 4; }

    // camX/camY: top-left corner of the view in world pixels.
    void draw(const Level& level, int camX, int camY) {
        std::ranges::fill(m_rgba, RGBA8{{0, 0, 0}, 255});
        FramebufferRef fb{m_rgba.data(), Height, m_width};

        // Visible-set culling, then the viewer's painter order: z layer, then a
        // pseudo depth (y + height/10 + z).
        m_visible.clear();
        const auto entities = level.entities();
        for (std::uint32_t i = 0; i < entities.size(); ++i) {
            const Entity& e = entities[i];
            const auto& gfx = static_cast<const Vid&>(e.vid).graphics();
            const int sx = e.x - gfx.width / 2 - camX;
            const int sy = e.y - gfx.height / 2 - e.z - camY;
            if (sx + gfx.width <= 0 || sy + gfx.height <= 0 || sx >= m_width || sy >= Height)
                continue;
            m_visible.push_back({e.vid->z_layer, e.y + gfx.height / 10 + e.z, i});
        }
        std::ranges::sort(m_visible);

        for (const auto& item : m_visible) {
            const Entity& e = entities[item.index];
            const auto& gfx = static_cast<const Vid&>(e.vid).graphics();
            if (e.frame >= gfx.frames.size())
                continue;
            DrawSprite(gfx.frames[e.frame], e.x - gfx.width / 2 - camX, e.y - gfx.height / 2 - e.z - camY, fb);
        }

        // RGBA8 {r,g,b,a} -> 0x00RRGGBB
        const std::size_t count = static_cast<std::size_t>(m_width) * Height;
        for (std::size_t i = 0; i < count; ++i) {
            const auto& p = m_rgba[i];
            m_xrgb[i] = (std::uint32_t{p.r} << 16) | (std::uint32_t{p.g} << 8) | std::uint32_t{p.b};
        }
    }

private:
    struct DrawItem {
        std::uint8_t layer;
        int depth;
        std::uint32_t index;
        auto operator<=>(const DrawItem&) const = default;
    };

    int m_width = 640;
    std::vector<RGBA8> m_rgba;
    std::vector<std::uint32_t> m_xrgb;
    std::vector<DrawItem> m_visible;
};

} // namespace gromada
