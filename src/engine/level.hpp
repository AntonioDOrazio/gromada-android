#pragma once
// Runtime representation of a loaded level: a flat list of entities built from the
// map objects plus their linked child objects (vid->linkedObjectVid chains).
// Header-only; included by the single core translation unit.

#include "core/std.hpp"
#include "libgromada/libgromada_all.hpp"

namespace gromada {

struct Entity {
    VidRef vid;
    int x = 0, y = 0, z = 0;          // world position (linked children already offset)
    std::uint8_t direction = 0;
    Action action = Action::act_stand;
    std::uint32_t frame = 0;
    Stopwatch stopwatch;
};

class Level {
public:
    Level(const GameResources& resources, const Map& map) : m_header{map.header} {
        m_entities.reserve(map.objects.size() * 2);
        std::uint32_t seed = 0x9E3779B9u;
        for (const auto& obj : map.objects) {
            const auto vid = resources.getVid(obj.nvid);
            spawn(resources, vid, obj.x, obj.y, obj.z, obj.direction, seed, 0);
        }
    }

    [[nodiscard]] const MapHeaderRawData& header() const noexcept { return m_header; }
    [[nodiscard]] std::span<Entity> entities() noexcept { return m_entities; }
    [[nodiscard]] std::span<const Entity> entities() const noexcept { return m_entities; }

    // Advances sprite animations by dt seconds (animation timing comes from the
    // graphics' frameDuration, in milliseconds, exactly like the original data).
    void animate(float dt) {
        for (auto& e : m_entities) {
            const Vid& vid = e.vid;
            const auto& gfx = vid.graphics();
            if (gfx.frameDuration > 0)
                e.frame += e.stopwatch.advance(dt, gfx.frameDuration * 0.001f);

            if (const auto range = getAnimationFrameRange(vid, e.action, e.direction)) {
                const auto len = static_cast<std::uint32_t>(range->second - range->first + 1);
                e.frame = static_cast<std::uint32_t>(range->first) + e.frame % len;
            } else {
                e.frame = 0;
            }
        }
    }

private:
    void spawn(const GameResources& resources, const VidRef& vid, int x, int y, int z, std::uint8_t dir,
               std::uint32_t& seed, int depth) {
        if (!vid || depth >= 16)
            return;
        seed = seed * 1664525u + 1013904223u; // cheap LCG: desynchronise identical animations
        m_entities.push_back(Entity{
            .vid = vid,
            .x = x, .y = y, .z = z,
            .direction = dir,
            .frame = seed >> 8,
        });

        if (vid->linkedObjectVid > 0) {
            spawn(resources, resources.getVid(vid->linkedObjectVid),
                  x + vid->linkX, y + vid->linkY, z + vid->linkZ, dir, seed, depth + 1);
        }
    }

    MapHeaderRawData m_header;
    std::vector<Entity> m_entities;
};

} // namespace gromada
