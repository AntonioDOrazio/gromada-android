#pragma once
// Save states of the on-screen texts (issue #14, docs/savestates.md): every text with its
// typewriter progress, the pending typewriter sounds and the next handle. The timing lookup
// (font vids) is static data and is not stored.
// decode() validates into a staged value, apply() cannot fail. Header-only.

#include "core/state_io.hpp"
#include "ui/text_manager.hpp"

namespace gromada::ui {

struct TextManagerState {
    std::vector<ScreenText> texts;
    std::vector<int> sounds;
    int nextHandle = 1;
};

struct SaveStateAccess {
    static void save(state::Writer& w, const TextManager& m) {
        w.count(m.m_texts.size());
        for (const ScreenText& t : m.m_texts) {
            w.i32(t.handle);
            w.i32(t.x);
            w.i32(t.y);
            w.i32(t.font);
            w.str(t.bytes);
            w.u64(t.visible);
            w.f32(t.pause);
            w.f32(t.clock);
        }
        w.array(std::span<const int>{m.m_sounds});
        w.i32(m.m_nextHandle);
    }

    static TextManagerState decode(state::Reader& r) {
        TextManagerState s;
        const std::size_t n = r.count(36, 1u << 16);
        s.texts.resize(n);
        for (ScreenText& t : s.texts) {
            t.handle = r.i32();
            t.x = r.i32();
            t.y = r.i32();
            t.font = r.i32();
            t.bytes = r.str(1u << 22);
            const std::uint64_t visible = r.u64();
            if (visible > t.bytes.size()) throw state::Error("save state: text visible count out of range");
            t.visible = static_cast<std::size_t>(visible);
            t.pause = r.finite32();
            t.clock = r.finite32();
            if (t.handle <= 0) throw state::Error("save state: bad text handle");
        }
        s.sounds = r.array<int>(1u << 16);
        s.nextHandle = r.i32();
        for (const ScreenText& t : s.texts)
            if (t.handle >= s.nextHandle) throw state::Error("save state: text handle beyond the next handle");
        return s;
    }

    static void apply(TextManager& m, TextManagerState&& s) {
        m.m_texts = std::move(s.texts);
        m.m_sounds = std::move(s.sounds);
        m.m_nextHandle = s.nextHandle;
    }
};

} // namespace gromada::ui
