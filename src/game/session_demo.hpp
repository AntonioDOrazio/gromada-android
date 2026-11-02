#pragma once
// Game session, demos: playback and recording of maps/demo*.dem (docs/re/runtime.md §8).
//
// Playback (Load("maps\\demoN.dem")): the map named in the demo is loaded with the recorded
// srand() seed (its startTimer equals the recorded start clock), every int cell of the
// freshly compiled script is overwritten with the recorded values, and then every game tick
// replays one record: the recorded clock (the session advances the engine by the recorded
// step) and the 12-byte input; FRead() returns the recorded lines and FOpen/FCreate return 0.
// A real key/button press, the end of the records or a Load() asked by the script end the
// demo: the map of the trailer (maps\mainmenu.map) is loaded.
// The original waits for real time to reach each recorded clock, so a demo plays at the
// recorded speed whatever the frame rate: DemoPlayer::due() tells the session when the next
// record is due.
//
// Recording (SaveDemo(file), commented out in MAPS.INI): the next map load writes the header,
// every tick its clock and input (plus the FRead lines), the map unload the end marker. The
// container is the one of the shipped demos (one section of type 7, see demo_file.hpp); the
// trailer is not written by the exe and is not written here.
//
// Header-only; depends on the demo parser and the engine contract.

#include "core/std.hpp"
#include "demo/demo_file.hpp"
#include "game/engine_api.hpp"

namespace gromada::game {

inline OriginalInput toOriginalInput(const demo::DemoInput& d) {
    OriginalInput r;
    r.mouseWorldX = d.worldMouseX;
    r.mouseWorldY = d.worldMouseY;
    r.mouseScreenX = d.screenMouseX;
    r.mouseScreenY = d.screenMouseY;
    r.bits = d.inputBits;
    r.key = d.key;
    return r;
}

class DemoPlayer {
public:
    friend struct SaveStateAccess; // save states (game/savestate.hpp)
    explicit DemoPlayer(demo::DemoFile file) : m_file{std::move(file)}, m_clock{m_file.startClock} {}

    [[nodiscard]] const demo::DemoFile& file() const noexcept { return m_file; }
    [[nodiscard]] bool finished() const noexcept { return m_tick >= m_file.ticks.size(); }
    [[nodiscard]] std::size_t tickIndex() const noexcept { return m_tick; }

    // Map loaded when the demo ends (trailer), maps\mainmenu.map when there is none.
    [[nodiscard]] std::string nextMap() const {
        std::string s;
        for (const char c : m_file.nextMap)
            if (static_cast<unsigned char>(c) >= 0x20) s += c;
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s.empty() ? std::string{"maps\\mainmenu.map"} : s;
    }

    // Milliseconds of game clock between the previous record and the next one.
    [[nodiscard]] int due() const {
        if (finished())
            return 0;
        return static_cast<int>(static_cast<std::int64_t>(m_file.ticks[m_tick].clock) - static_cast<std::int64_t>(m_clock));
    }

    // Consumes the next record: its input and clock.
    struct Step {
        OriginalInput input;
        std::uint32_t clock = 0;
    };
    Step next() {
        const auto& t = m_file.ticks.at(m_tick);
        m_current = m_tick++;
        m_clock = t.clock;
        return {toOriginalInput(t.input), t.clock};
    }

    // FRead during the current tick: the next recorded line (nullopt when the recording has
    // no more lines for this tick, i.e. the replay diverged).
    std::optional<std::string> fread() {
        if (m_fread < m_file.freads.size() && m_file.freads[m_fread].tick <= m_current)
            return m_file.freads[m_fread++].text;
        return std::nullopt;
    }

private:
    demo::DemoFile m_file;
    std::size_t m_tick = 0, m_current = 0, m_fread = 0;
    std::uint32_t m_clock = 0;
};

// Writes a demo in the format of the shipped ones.
class DemoRecorder {
public:
    friend struct SaveStateAccess; // save states (game/savestate.hpp)
    DemoRecorder(std::string mapName, std::uint32_t seed, std::uint32_t startClock, std::span<const std::int32_t> ints) {
        put32(static_cast<std::uint32_t>(mapName.size()));
        m_data.insert(m_data.end(), mapName.begin(), mapName.end());
        put32(seed);
        put32(startClock);
        for (const auto v : ints)
            put32(static_cast<std::uint32_t>(v));
    }

    void tick(std::uint32_t clock, const OriginalInput& in) {
        put32(clock);
        put16(static_cast<std::uint16_t>(in.mouseWorldX));
        put16(static_cast<std::uint16_t>(in.mouseWorldY));
        put16(static_cast<std::uint16_t>(in.mouseScreenX));
        put16(static_cast<std::uint16_t>(in.mouseScreenY));
        put16(in.bits);
        m_data.push_back(in.key);
        m_data.push_back(0);
    }

    void fread(std::string_view line) {
        put32(static_cast<std::uint32_t>(line.size()));
        m_data.insert(m_data.end(), line.begin(), line.end());
    }

    // The whole file: container header, data, end marker.
    [[nodiscard]] std::vector<std::uint8_t> finish() const {
        std::vector<std::uint8_t> out;
        auto p32 = [&](std::uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i))); };
        const std::size_t dataLen = m_data.size() + 4; // + end marker
        p32(1);                                         // one section
        out.push_back(7);                               // demo section
        p32(static_cast<std::uint32_t>(dataLen + 6));   // next: count + offset + data
        p32(1);                                         // element count (as in the shipped demos)
        out.push_back(0);                               // u16 data offset
        out.push_back(0);
        out.insert(out.end(), m_data.begin(), m_data.end());
        p32(0xFFFFFFFFu);
        return out;
    }

private:
    void put32(std::uint32_t v) { for (int i = 0; i < 4; ++i) m_data.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }
    void put16(std::uint16_t v) { m_data.push_back(static_cast<std::uint8_t>(v)); m_data.push_back(static_cast<std::uint8_t>(v >> 8)); }

    std::vector<std::uint8_t> m_data;
};

} // namespace gromada::game
