#pragma once
// Save states, part 2 (issue #14): the session chunk, the top-level format (header, chunks)
// and SaveStateManager, the entry point used by the libretro core and the tests.
// Included at the end of game/savestate.hpp; see that file and docs/savestates.md.

#include "game/savestate.hpp"

namespace gromada::game {

struct SessionStaged {
    // engine and script
    ss::EngineState engine;
    bool hasVm = false;
    std::unique_ptr<script::Interpreter> newVm; // when the saved script is not the running one
    script::VmState vm;
    // session scalars
    std::string map;
    std::vector<std::string> history;
    bool mapLoaded = false;
    struct OpState { std::uint8_t kind; std::string arg; int value; };
    std::vector<OpState> ops;
    OriginalInput input;
    input::Context lastContext = input::Context::Menu;
    std::array<bool, input::KeyCount> staleKey{};
    bool staleLeft = false, staleRight = false, prevAnyDown = false;
    int screenW = 640, screenH = 480;
    bool demoView = false;
    double realMs = 0, msDebt = 0, demoDebtMs = 0;
    std::uint64_t frames = 0, ticks = 0;
    std::optional<DemoPlayer> demo;
    std::string demoName, lastDemoLine;
    std::optional<std::vector<std::uint8_t>> recorder;
    std::string recordPath, recordingTo, lastMovie;
    bool movieActive = false;
    double moviePosition = 0;
    std::filesystem::path moviePath;
    struct SfxRequest { int nsfx, pan, attenuation; };
    struct SfxChannel { bool active, loaded; int nsfx; std::uint32_t time; };
    struct SfxBuffer { std::int32_t sound; const SoundData* data; int nsfx; std::uint32_t voice; int pan, attenuation; };
    std::array<SfxRequest, SoundChannels::QueueSize> sfxQueue{};
    std::array<SfxChannel, SoundChannels::ChannelCount> sfxChannels{};
    std::array<SfxBuffer, SoundChannels::ChannelCount> sfxBuffers{};   // Session::MixerSink
    std::string wavName;
    bool wavLoaded = false;
    std::uint32_t messageClock = 0;
    // controls (issue #16)
    struct FollowState { bool valid, hold; float fx, fy, vx, vy; int px, py, lastX, lastY; } follow{};
    struct HudState { int current, requested; float timer; } hud{-1, -1, 0};
    std::optional<std::deque<SoundData>> wavs; // reloaded PlayWAV sounds (names differ)
    std::vector<std::string> wavNames;
    std::array<int, 4> playlist{};
    bool playing = false;
    int scriptSfx = 100, scriptMusic = 100;
    bool scrollLock = false, stateBar = false, detail = true, exit = false;
    int cursorType = 0, net = 0;
    std::uint64_t scriptErrors = 0, warnings = 0, unknown = 0;
    std::optional<script::GlobalSnapshot> carry;
    std::deque<std::uint8_t> keyQueue;
    std::array<float, input::KeyCount> keyHeld{};
    struct FileState { std::string key, data; std::size_t pos; bool dirty; };
    std::map<int, FileState> files;
    std::string lastLine;
    int nextFile = 1;
    // ui
    std::vector<ScreenObject> objects;
    std::vector<int> menu;
    std::map<int, std::uint64_t> textSeq;
    MenuState menuState;
    std::vector<int> uiSounds;
    std::string message;
    int nextObject = 1;
    std::uint64_t seq = 0;
    std::uint32_t uiClock = 0, uiUpdateClock = 0;
    ui::TextManagerState texts;
    // output picture (only stored while a demo plays: a demo frame without a tick shows it)
    std::vector<std::uint32_t> xrgb;
    int xrgbW = 0, xrgbH = 0;
    // mixer
    bool hasMixer = false;
    MixerState mixer;
};

// Keeps compiled scripts and parsed demos so that loading a state of another map (rewind or
// run-ahead across a Load()) does not recompile every time.
// State kept by the frontend glue (the libretro core), saved with the session.
struct SaveStateExtras {
    input::Mapper* mapper = nullptr;   // the core's input mapper
    double* audioDebt = nullptr;       // the core's fractional audio sample carry
};

class SaveStateManager {
public:
    using Extras = SaveStateExtras;

    // Writes a complete state of `session` into `out` (resized to the state size). False (and
    // error()) during a movie or with an engine that has no save states.
    bool save(Session& session, std::vector<std::uint8_t>& out, const Extras& extras = {});
    // Restores a state. False (and error()) for a truncated, corrupted or foreign state; the
    // session is then left exactly as it was.
    bool load(Session& session, std::span<const std::uint8_t> data, const Extras& extras = {});

    [[nodiscard]] const std::string& error() const noexcept { return m_error; }
    [[nodiscard]] double lastSaveMs() const noexcept { return m_saveMs; }
    [[nodiscard]] double lastLoadMs() const noexcept { return m_loadMs; }

    // Script of `ini` for the session (cache, else compiled like Session::compileScript).
    script::Script script(Session& session, const std::string& ini);
    void remember(const std::string& ini, const script::Script& s) {
        if (m_scripts.size() >= 8 && !m_scripts.contains(ini)) m_scripts.erase(m_scripts.begin());
        m_scripts.insert_or_assign(ini, s);
    }
    std::shared_ptr<const demo::DemoFile> demoFile(Session& session, const std::string& name);

private:
    std::map<std::string, script::Script> m_scripts;
    std::map<std::string, std::shared_ptr<const demo::DemoFile>> m_demos;
    std::string m_error;
    double m_saveMs = 0, m_loadMs = 0;
};

// Checksum of a state: the header (with its checksum field as zero), then the payload.
inline std::uint64_t stateChecksum(std::span<const std::uint8_t> header, std::span<const std::uint8_t> payload) {
    std::array<std::uint8_t, SaveStateHeaderSize> h{};
    std::memcpy(h.data(), header.data(), SaveStateHeaderSize);
    std::memset(h.data() + 24, 0, 8);
    return state::checksum64(payload, state::checksum64(h));
}

// Script of a map: maps\x.map -> maps\x.ini (as Session::loadMapAndScript).
inline std::string scriptNameOfMap(const std::string& name) {
    const std::filesystem::path p{name};
    return name.substr(0, name.size() - std::min(name.size(), p.extension().string().size())) + ".ini";
}

// ---------------------------------------------------------------------------- session chunk
inline std::int32_t SaveStateAccess::soundRef(const Session& s, const SoundData* p) {
    if (!p) return -1;
    if (!s.m_sounds.empty() && p >= s.m_sounds.data() && p < s.m_sounds.data() + s.m_sounds.size())
        return static_cast<std::int32_t>(p - s.m_sounds.data()) + 1;
    for (std::size_t i = 0; i < s.m_wavs.size(); ++i)
        if (&s.m_wavs[i] == p) return static_cast<std::int32_t>(s.m_sounds.size() + i + 1);
    return -1;
}

inline void SaveStateAccess::saveSession(state::Writer& w, const Session& s) {
    const std::size_t chunk = w.beginChunk(SessionTag);
    w.str(s.m_map);
    w.count(s.m_history.size());
    for (const auto& h : s.m_history) w.str(h);
    w.boolean(s.m_mapLoaded);
    w.count(s.m_ops.size());
    for (const auto& op : s.m_ops) { w.u8(op.kind); w.str(op.arg); w.i32(op.value); }
    ss::writeOriginalInput(w, s.m_input);
    w.u8(static_cast<std::uint8_t>(s.m_lastContext));
    for (const bool b : s.m_staleKey) w.boolean(b);
    w.boolean(s.m_staleLeft); w.boolean(s.m_staleRight); w.boolean(s.m_prevAnyDown);
    w.i32(s.m_outW); w.i32(s.m_outH); w.i32(s.m_screenW); w.i32(s.m_screenH);
    w.boolean(s.m_demoView);
    w.f64(s.m_realMs); w.f64(s.m_msDebt); w.f64(s.m_demoDebtMs);
    w.u64(s.m_frames); w.u64(s.m_ticks);
    w.boolean(s.m_demo.has_value());
    if (s.m_demo) {
        const DemoPlayer& d = *s.m_demo;
        w.str(s.m_demoName);
        w.u64(d.m_file.ticks.size());
        w.u64(d.m_tick); w.u64(d.m_current); w.u64(d.m_fread);
        w.u32(d.m_clock);
    }
    w.str(s.m_lastDemoLine);
    w.boolean(s.m_recorder.has_value());
    if (s.m_recorder) w.blob(s.m_recorder->m_data);
    w.str(s.m_recordPath); w.str(s.m_recordingTo); w.str(s.m_lastMovie);
    w.boolean(s.m_movieActive);
    if (s.m_movieActive) w.f64(*s.m_movies->position());
    // sound-effect scheduler: pending requests and the 8 channels (voice ids of the mixer)
    for (const auto& q : s.m_sfx.m_queue) { w.i32(q.nsfx); w.i32(q.pan); w.i32(q.attenuation); }
    for (const auto& c : s.m_sfx.m_channels) { w.boolean(c.active); w.boolean(c.loaded); w.i32(c.nsfx); w.u32(c.time); }
    w.str(s.m_sfx.m_wavName);
    for (const auto& b : s.m_sfxSink.buffers) {   // one DirectSound buffer per channel
        w.i32(b.sound ? soundRef(s, b.sound) : 0);
        w.i32(b.nsfx); w.u32(b.voice); w.i32(b.pan); w.i32(b.attenuation);
    }
    w.boolean(s.m_wavLoaded);
    w.u32(s.m_messageClock);
    {
        const FollowCamera& f = s.m_follow;   // its Params are configuration
        w.boolean(f.m_valid); w.boolean(f.m_hold);
        w.f32(f.m_fx); w.f32(f.m_fy); w.f32(f.m_vx); w.f32(f.m_vy);
        w.i32(f.m_px); w.i32(f.m_py); w.i32(f.m_lastX); w.i32(f.m_lastY);
        const WeaponHud& h = s.m_weaponHud;
        w.i32(h.m_current); w.i32(h.m_requested); w.f32(h.m_timer);
    }
    w.count(s.m_wavNames.size());
    for (const auto& n : s.m_wavNames) w.str(n);
    for (const int t : s.m_playlist) w.i32(t);
    w.boolean(s.m_playing);
    w.i32(s.m_scriptSfx); w.i32(s.m_scriptMusic);
    w.boolean(s.m_scrollLock); w.boolean(s.m_stateBar); w.boolean(s.m_detail); w.boolean(s.m_exit);
    w.i32(s.m_cursorType); w.i32(s.m_net);
    w.u64(s.m_scriptErrors); w.u64(s.m_warnings); w.u64(s.m_unknown);
    w.boolean(s.m_carry.has_value());
    if (s.m_carry) {
        script::writeCells(w, s.m_carry->cells);
        w.u32(s.m_carry->globalCount);
    }
    // input record builder (the repeat timing is configuration)
    w.count(s.m_inputBuilder.m_queue.size());
    for (const auto k : s.m_inputBuilder.m_queue) w.u8(k);
    for (const float h : s.m_inputBuilder.m_held) w.f32(h);
    // files opened by the scripts (the files on disk are not part of the state)
    const SaveFiles& f = s.m_files;
    w.count(f.m_open.size());
    for (const auto& [h, file] : f.m_open) {
        w.i32(h); w.str(file.key); w.str(file.data); w.u64(file.pos); w.boolean(file.dirty);
    }
    w.str(f.m_lastLine);
    w.i32(f.m_next);
    // screen objects and texts
    const SessionUi& u = s.m_ui;
    w.count(u.m_objects.size());
    for (const ScreenObject& o : u.m_objects) {
        w.i32(o.handle); w.u16(o.nvid); w.i32(o.x); w.i32(o.y); w.i32(o.z);
        w.i32(o.dirIndex); w.i32(o.action); w.i32(o.layer); w.u8(static_cast<std::uint8_t>(o.list));
        w.boolean(o.button); w.u64(o.seq); w.i32(o.frame); w.u32(o.lastTact); w.boolean(o.soundDone);
    }
    w.array(std::span<const int>{u.m_menu});
    w.count(u.m_textSeq.size());
    for (const auto& [h, q] : u.m_textSeq) { w.i32(h); w.u64(q); }
    w.i32(u.m_menuState.selected); w.i32(u.m_menuState.dir); w.boolean(u.m_menuState.click);
    w.i32(u.m_menuState.selectedHandle);
    w.array(std::span<const int>{u.m_sounds});
    w.str(u.m_message);
    w.i32(u.m_nextObject);
    w.u64(u.m_seq);
    w.u32(u.m_clock); w.u32(u.m_updateClock);   // m_rand is the engine's rand() (configuration)
    ui::SaveStateAccess::save(w, u.m_texts);
    // script identity (the VM chunk follows the session chunk)
    w.boolean(static_cast<bool>(s.m_vm));
    if (s.m_vm) {
        w.str(scriptNameOfMap(s.m_map));
        script::writeIdentity(w, script::vmIdentityOf(s.m_vm->script()));
    }
    const bool picture = s.m_demo.has_value();
    w.boolean(picture);
    if (picture) {
        w.i32(s.m_outW); w.i32(s.m_outH);
        w.bytes(s.m_xrgb.data(), s.m_xrgb.size() * 4);
    }
    w.boolean(s.m_mixer != nullptr);
    w.endChunk(chunk);
    if (s.m_vm) script::SaveStateAccess::save(w, *s.m_vm);
    saveEngine(w, *dynamic_cast<const GameEngine*>(s.m_engine.get()));
    if (s.m_mixer)
        gromada::SaveStateAccess::save(w, *s.m_mixer, [&](const SoundData* p) { return soundRef(s, p); });
}

inline SessionStaged SaveStateAccess::decodeSession(state::Reader& outer, Session& s, SaveStateManager& mgr) {
    SessionStaged st;
    state::Reader r = outer.chunk(SessionTag);
    st.map = r.str(4096);
    const std::size_t nHistory = r.count(4, 1u << 20);
    for (std::size_t i = 0; i < nHistory; ++i) st.history.push_back(r.str(4096));
    st.mapLoaded = r.boolean();
    const std::size_t nOps = r.count(9, 1u << 16);
    for (std::size_t i = 0; i < nOps; ++i) {
        SessionStaged::OpState op;
        op.kind = r.ranged<std::uint8_t>(0, 3, "posted operation");
        op.arg = r.str(1u << 16);
        op.value = r.i32();
        st.ops.push_back(std::move(op));
    }
    st.input = ss::readOriginalInput(r);
    st.lastContext = static_cast<input::Context>(r.ranged<std::uint8_t>(0, 2, "input context"));
    for (bool& b : st.staleKey) b = r.boolean();
    st.staleLeft = r.boolean(); st.staleRight = r.boolean(); st.prevAnyDown = r.boolean();
    (void)r.i32(); (void)r.i32();   // output size when saved (informational)
    st.screenW = r.ranged<std::int32_t>(1, 4096, "script screen");
    st.screenH = r.ranged<std::int32_t>(1, 4096, "script screen");
    st.demoView = r.boolean();
    st.realMs = r.finite64(); st.msDebt = r.finite64(); st.demoDebtMs = r.finite64();
    st.frames = r.u64(); st.ticks = r.u64();
    if (r.boolean()) {
        st.demoName = r.str(4096);
        const std::uint64_t nTicks = r.u64();
        auto file = mgr.demoFile(s, st.demoName);
        if (!file || file->ticks.size() != nTicks) throw state::Error("save state: the demo differs from the saved one");
        DemoPlayer d{*file};
        const std::uint64_t tick = r.u64(), current = r.u64(), fread = r.u64();
        if (tick > nTicks || current > nTicks || fread > file->freads.size()) throw state::Error("save state: bad demo position");
        d.m_tick = static_cast<std::size_t>(tick);
        d.m_current = static_cast<std::size_t>(current);
        d.m_fread = static_cast<std::size_t>(fread);
        d.m_clock = r.u32();
        st.demo.emplace(std::move(d));
    }
    st.lastDemoLine = r.str(1u << 16);
    if (r.boolean()) st.recorder = r.blob(1u << 26);
    st.recordPath = r.str(4096); st.recordingTo = r.str(4096); st.lastMovie = r.str(4096);
    st.movieActive = r.boolean();
    if (st.movieActive) {
        st.moviePosition = r.finite64();
        if (st.moviePosition < 0 || st.moviePosition > 1e6) throw state::Error("save state: bad movie position");
        const auto path = script::resolveCaseInsensitive(s.m_config.gameDir, st.lastMovie);
        if (!s.m_movies || !path) throw state::Error("save state: the movie " + st.lastMovie + " cannot be played");
        st.moviePath = *path;
    }
    for (auto& q : st.sfxQueue) {
        q.nsfx = r.i32();
        q.pan = r.ranged<std::int32_t>(-10000, 10000, "sound pan");
        q.attenuation = r.ranged<std::int32_t>(std::numeric_limits<std::int32_t>::min() / 2, 0, "sound attenuation");
    }
    for (auto& c : st.sfxChannels) {
        c.active = r.boolean();
        c.loaded = r.boolean();
        c.nsfx = r.i32();
        c.time = r.u32();
    }
    st.wavName = r.str(4096);
    for (auto& b : st.sfxBuffers) {
        b.sound = r.i32();
        b.data = nullptr;   // resolved with the PlayWAV sounds below
        b.nsfx = r.i32(); b.voice = r.u32(); b.pan = r.i32(); b.attenuation = r.i32();
    }
    for (std::size_t i = 0; i < st.sfxChannels.size(); ++i)
        if (st.sfxChannels[i].loaded != (st.sfxBuffers[i].sound != 0)) throw state::Error("save state: sound channel without its buffer");
    st.wavLoaded = r.boolean();
    st.messageClock = r.u32();
    {
        auto& f = st.follow;
        f.valid = r.boolean(); f.hold = r.boolean();
        f.fx = r.finite32(); f.fy = r.finite32(); f.vx = r.finite32(); f.vy = r.finite32();
        f.px = r.i32(); f.py = r.i32(); f.lastX = r.i32(); f.lastY = r.i32();
        st.hud.current = r.i32(); st.hud.requested = r.i32(); st.hud.timer = r.finite32();
    }
    const std::size_t nWavs = r.count(4, 1u << 12);
    for (std::size_t i = 0; i < nWavs; ++i) st.wavNames.push_back(r.str(4096));
    for (int& t : st.playlist) t = r.i32();
    st.playing = r.boolean();
    st.scriptSfx = r.ranged<std::int32_t>(0, 100, "sound volume");
    st.scriptMusic = r.ranged<std::int32_t>(0, 100, "music volume");
    st.scrollLock = r.boolean(); st.stateBar = r.boolean(); st.detail = r.boolean(); st.exit = r.boolean();
    st.cursorType = r.i32(); st.net = r.i32();
    st.scriptErrors = r.u64(); st.warnings = r.u64(); st.unknown = r.u64();
    if (r.boolean()) {
        script::GlobalSnapshot carry;
        carry.cells = script::readCells(r, script::kCellCount);
        carry.globalCount = r.ranged<std::uint32_t>(0, script::kCellCount, "carried cells");
        st.carry = std::move(carry);
    }
    const std::size_t nKeys = r.count(1, 64);
    for (std::size_t i = 0; i < nKeys; ++i) st.keyQueue.push_back(r.u8());
    for (float& h : st.keyHeld) h = r.finite32();
    const std::size_t nFiles = r.count(21, 1u << 10);
    for (std::size_t i = 0; i < nFiles; ++i) {
        const int h = r.i32();
        SessionStaged::FileState f;
        f.key = r.str(4096);
        // a key that is not a clean relative path could make FClose write outside the save folder
        if (SaveFiles::sanitize(f.key) != f.key) throw state::Error("save state: bad file name");
        f.data = r.str(1u << 26);
        f.pos = static_cast<std::size_t>(r.u64());
        f.dirty = r.boolean();
        if (!st.files.emplace(h, std::move(f)).second) throw state::Error("save state: duplicate file handle");
    }
    st.lastLine = r.str(1u << 16);
    st.nextFile = r.i32();
    const std::size_t nObjects = r.count(46, 1u << 16);
    for (std::size_t i = 0; i < nObjects; ++i) {
        ScreenObject o;
        o.handle = r.i32();
        o.nvid = r.u16();
        if (o.nvid >= s.m_vids.size()) throw state::Error("save state: screen object vid out of range");
        o.x = r.i32(); o.y = r.i32(); o.z = r.i32();
        o.dirIndex = r.i32();
        o.action = r.ranged<std::int32_t>(0, 15, "screen object action");
        o.layer = r.ranged<std::int32_t>(0, ui::MenuLayerCount - 1, "screen object layer");
        o.list = static_cast<UiList>(r.ranged<std::uint8_t>(0, 1, "screen object list"));
        o.button = r.boolean();
        o.seq = r.u64();
        o.frame = r.i32();
        o.lastTact = r.u32();
        o.soundDone = r.boolean();
        st.objects.push_back(o);
    }
    st.menu = r.array<int>(1u << 16);
    const std::size_t nSeq = r.count(12, 1u << 16);
    for (std::size_t i = 0; i < nSeq; ++i) {
        const int h = r.i32();
        st.textSeq[h] = r.u64();
    }
    st.menuState.selected = r.i32(); st.menuState.dir = r.i32(); st.menuState.click = r.boolean();
    st.menuState.selectedHandle = r.i32();
    st.uiSounds = r.array<int>(1u << 16);
    st.message = r.str(1u << 16);
    st.nextObject = r.i32();
    st.seq = r.u64();
    st.uiClock = r.u32(); st.uiUpdateClock = r.u32();
    st.texts = ui::SaveStateAccess::decode(r);
    st.hasVm = r.boolean();
    std::string ini;
    script::VmIdentity identity;
    if (st.hasVm) {
        ini = r.str(4096);
        identity = script::readIdentity(r);
    }
    if (r.boolean()) {
        st.xrgbW = r.ranged<std::int32_t>(1, 4096, "picture width");
        st.xrgbH = r.ranged<std::int32_t>(1, 4096, "picture height");
        const std::size_t n = static_cast<std::size_t>(st.xrgbW) * static_cast<std::size_t>(st.xrgbH);
        if (n > r.remaining() / 4) throw state::Error("save state: truncated (picture)");
        st.xrgb.resize(n);
        r.bytes(st.xrgb.data(), n * 4);
    }
    st.hasMixer = r.boolean();
    r.expectEnd("session");

    // PlayWAV sounds: kept when the same files are loaded, otherwise read again.
    if (st.wavNames != s.m_wavNames) {
        std::deque<SoundData> wavs;
        for (const auto& name : st.wavNames) {
            const auto path = script::resolveCaseInsensitive(s.m_config.gameDir, name);
            if (!path) throw state::Error("save state: PlayWAV file not found: " + name);
            try {
                std::ifstream in{*path, std::ios::binary};
                const auto size = std::filesystem::file_size(*path);
                BinaryStreamReader reader{in, static_cast<std::size_t>(size)};
                wavs.emplace_back(reader);
            } catch (const std::exception& e) {
                throw state::Error("save state: PlayWAV file unreadable: " + name);
            }
        }
        st.wavs = std::move(wavs);
    }

    // The script: the running interpreter when it runs the same script, else compiled again.
    if (st.hasVm) {
        if (s.m_vm && script::vmIdentityOf(s.m_vm->script()) == identity) {
            st.vm = script::SaveStateAccess::decode(outer, s.m_vm->script());
        } else {
            script::Script compiled = mgr.script(s, ini);
            if (script::vmIdentityOf(compiled) != identity) throw state::Error("save state: the script " + ini + " differs from the saved one");
            st.vm = script::SaveStateAccess::decode(outer, compiled);
            st.newVm = std::make_unique<script::Interpreter>(std::move(compiled), s, script::ExecConfig{});
        }
    }
    const auto* engine = dynamic_cast<const GameEngine*>(s.m_engine.get());
    if (!engine) throw state::Error("save state: the engine has no save states");
    st.engine = decodeEngine(outer, *engine);
    // Sounds referenced by the channel buffers and the mixer voices (fw.res, then PlayWAV).
    const std::deque<SoundData>& wavs = st.wavs ? *st.wavs : s.m_wavs;
    if (st.wavLoaded && wavs.empty()) throw state::Error("save state: PlayWAV slot without a sound");
    const auto lookup = [&](std::int32_t id) -> const SoundData* {
        if (id >= 1 && static_cast<std::size_t>(id) <= s.m_sounds.size()) return &s.m_sounds[static_cast<std::size_t>(id - 1)];
        const std::int64_t extra = std::int64_t{id} - static_cast<std::int64_t>(s.m_sounds.size()) - 1;
        if (extra >= 0 && static_cast<std::size_t>(extra) < wavs.size()) return &wavs[static_cast<std::size_t>(extra)];
        return nullptr;
    };
    for (auto& b : st.sfxBuffers)
        if (b.sound && !(b.data = lookup(b.sound))) throw state::Error("save state: unknown sound in a channel buffer");
    if (st.hasMixer) st.mixer = gromada::SaveStateAccess::decode(outer, lookup);
    return st;
}

inline void SaveStateAccess::applySession(Session& s, SessionStaged&& st) {
    // Movie: the same one keeps playing from the saved clock; another one is opened and wound
    // forward to it (the mixer's stream position is restored with the mixer below).
    if (!st.movieActive || !s.m_movieActive || s.m_lastMovie != st.lastMovie) {
        s.stopMovie();
        if (st.movieActive) {
            if (s.m_movies->open(st.moviePath)) {
                s.m_movieActive = true;
                if (s.m_mixer) s.m_mixer->setStream(s.m_movies->audio());
            } else {
                s.log(LogLevel::Warn, std::format("load state: movie {} cannot be opened, skipped", st.lastMovie));
            }
        }
    }
    if (s.m_movieActive) s.m_movies->seek(st.moviePosition);
    applyEngine(*dynamic_cast<GameEngine*>(s.m_engine.get()), std::move(st.engine));
    if (!st.hasVm) {
        s.m_vm.reset();
    } else {
        if (st.newVm) s.m_vm = std::move(st.newVm);
        script::SaveStateAccess::apply(*s.m_vm, std::move(st.vm));
    }
    s.m_map = std::move(st.map);
    s.m_history = std::move(st.history);
    s.m_mapLoaded = st.mapLoaded;
    s.m_ops.clear();
    for (auto& op : st.ops) s.m_ops.push_back({static_cast<Session::Op::Kind>(op.kind), std::move(op.arg), op.value});
    s.m_input = st.input;
    s.m_lastContext = st.lastContext;
    s.m_staleKey = st.staleKey;
    s.m_staleLeft = st.staleLeft; s.m_staleRight = st.staleRight; s.m_prevAnyDown = st.prevAnyDown;
    s.m_screenW = st.screenW; s.m_screenH = st.screenH;
    s.m_demoView = st.demoView;
    s.m_realMs = st.realMs; s.m_msDebt = st.msDebt; s.m_demoDebtMs = st.demoDebtMs;
    s.m_frames = st.frames; s.m_ticks = st.ticks;
    s.m_demo = std::move(st.demo);
    s.m_demoName = std::move(st.demoName);
    s.m_lastDemoLine = std::move(st.lastDemoLine);
    s.m_recorder.reset();
    if (st.recorder) {
        s.m_recorder.emplace(std::string{}, 0u, 0u, std::span<const std::int32_t>{});
        s.m_recorder->m_data = std::move(*st.recorder);
    }
    s.m_recordPath = std::move(st.recordPath);
    s.m_recordingTo = std::move(st.recordingTo);
    s.m_lastMovie = std::move(st.lastMovie);
    for (std::size_t i = 0; i < st.sfxQueue.size(); ++i)
        s.m_sfx.m_queue[i] = {st.sfxQueue[i].nsfx, st.sfxQueue[i].pan, st.sfxQueue[i].attenuation};
    for (std::size_t i = 0; i < st.sfxChannels.size(); ++i) {
        const auto& c = st.sfxChannels[i];
        const auto& b = st.sfxBuffers[i];
        s.m_sfx.m_channels[i] = {c.active, c.loaded, c.nsfx, c.time};
        // The voice id is kept as saved. The mixer is restored with the same voices and the
        // same next id, so the id is either a restored voice or one that already ended, which
        // the mixer treats like the game does (not playing; Play starts the sound again).
        s.m_sfxSink.buffers[i] = {b.data, b.nsfx, b.voice, b.pan, b.attenuation};
    }
    s.m_sfx.m_wavName = std::move(st.wavName);
    s.m_wavLoaded = st.wavLoaded;
    s.m_messageClock = st.messageClock;
    {
        FollowCamera& f = s.m_follow;
        f.m_valid = st.follow.valid; f.m_hold = st.follow.hold;
        f.m_fx = st.follow.fx; f.m_fy = st.follow.fy; f.m_vx = st.follow.vx; f.m_vy = st.follow.vy;
        f.m_px = st.follow.px; f.m_py = st.follow.py; f.m_lastX = st.follow.lastX; f.m_lastY = st.follow.lastY;
        s.m_weaponHud.m_current = st.hud.current; s.m_weaponHud.m_requested = st.hud.requested;
        s.m_weaponHud.m_timer = st.hud.timer;
    }
    if (st.wavs) s.m_wavs = std::move(*st.wavs);
    s.m_wavNames = std::move(st.wavNames);
    s.m_playlist = st.playlist;
    s.m_playing = st.playing;
    s.m_scriptSfx = st.scriptSfx; s.m_scriptMusic = st.scriptMusic;
    s.m_scrollLock = st.scrollLock; s.m_stateBar = st.stateBar; s.m_detail = st.detail; s.m_exit = st.exit;
    s.m_cursorType = st.cursorType; s.m_net = st.net;
    s.m_scriptErrors = static_cast<std::size_t>(st.scriptErrors);
    s.m_warnings = static_cast<std::size_t>(st.warnings);
    s.m_unknown = static_cast<std::size_t>(st.unknown);
    s.m_carry = std::move(st.carry);
    s.m_inputBuilder.m_queue = std::move(st.keyQueue);
    s.m_inputBuilder.m_held = st.keyHeld;
    SaveFiles& f = s.m_files;
    f.m_open.clear();
    for (auto& [h, file] : st.files) f.m_open.emplace(h, SaveFiles::File{std::move(file.key), std::move(file.data), file.pos, file.dirty});
    f.m_lastLine = std::move(st.lastLine);
    f.m_next = st.nextFile;
    SessionUi& u = s.m_ui;
    u.m_objects = std::move(st.objects);
    u.m_menu = std::move(st.menu);
    u.m_textSeq = std::move(st.textSeq);
    u.m_menuState = st.menuState;
    u.m_sounds = std::move(st.uiSounds);
    u.m_message = std::move(st.message);
    u.m_nextObject = st.nextObject;
    u.m_seq = st.seq;
    u.m_clock = st.uiClock; u.m_updateClock = st.uiUpdateClock;
    ui::SaveStateAccess::apply(u.m_texts, std::move(st.texts));
    if (s.m_mixer && st.hasMixer) gromada::SaveStateAccess::apply(*s.m_mixer, std::move(st.mixer));
    s.updateView();
    s.applyVolumes();
    if (!st.xrgb.empty() && st.xrgbW == s.m_outW && st.xrgbH == s.m_outH) s.m_xrgb = std::move(st.xrgb);
}

// ---------------------------------------------------------------------------- manager
// Compiles a script like Session::compileScript.
inline script::Script SaveStateAccess::compileScript(Session& s, const std::string& ini) {
    if (!script::resolveCaseInsensitive(s.m_config.gameDir, ini)) throw state::Error("save state: script " + ini + " not found");
    script::CompileOptions opts;
    if (s.m_config.predeclareBuiltins) opts.predeclared = &s.m_builtins;
    try {
        return script::Script::compile(s.m_loader, ini, opts);
    } catch (const std::exception& e) {
        throw state::Error("save state: script " + ini + ": " + e.what());
    }
}

// The demo being played when it is `name` (current = true), else the file read again.
inline std::shared_ptr<const demo::DemoFile> SaveStateAccess::readDemoFile(Session& s, const std::string& name, bool& current) {
    current = s.m_demo && s.m_demoName == name;
    if (current) return std::make_shared<const demo::DemoFile>(s.m_demo->file());
    std::optional<std::filesystem::path> path = script::resolveCaseInsensitive(s.m_config.gameDir, name);
    if (!path && !s.m_config.saveDir.empty()) path = script::resolveCaseInsensitive(s.m_config.saveDir, name);
    if (!path) return nullptr;
    std::ifstream in{*path, std::ios::binary};
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{in}, {}};
    auto parsed = demo::parseDemo(bytes);
    if (!parsed) return nullptr;
    return std::make_shared<const demo::DemoFile>(std::move(*parsed));
}

inline script::Script SaveStateManager::script(Session& session, const std::string& ini) {
    if (const auto it = m_scripts.find(ini); it != m_scripts.end()) return it->second;
    script::Script compiled = SaveStateAccess::compileScript(session, ini);
    remember(ini, compiled);
    return compiled;
}

inline std::shared_ptr<const demo::DemoFile> SaveStateManager::demoFile(Session& session, const std::string& name) {
    if (const auto it = m_demos.find(name); it != m_demos.end()) return it->second;
    bool current = false;
    auto file = SaveStateAccess::readDemoFile(session, name, current);
    if (!file) return nullptr;
    if (m_demos.size() >= 4) m_demos.erase(m_demos.begin());
    m_demos.insert_or_assign(name, file);
    return file;
}

inline bool SaveStateManager::save(Session& session, std::vector<std::uint8_t>& out, const Extras& extras) {
    const auto t0 = std::chrono::steady_clock::now();
    m_error.clear();
    const auto* engine = dynamic_cast<const GameEngine*>(&session.engine());
    if (!engine) {
        m_error = "the engine has no save states";
        return false;
    }
    if (session.moviePlaying() && !SaveStateAccess::moviePosition(session)) {
        m_error = "the movie player has no playback position: no save state while a movie plays";
        return false;
    }
    try {
        if (session.interpreter()) remember(scriptNameOfMap(session.currentMap()), session.interpreter()->script());
        state::Writer w{out};
        std::array<std::uint8_t, SaveStateHeaderSize> header{};
        w.bytes(header.data(), header.size());
        const std::size_t core = w.beginChunk(state::fourcc("CORE"));
        w.f64(extras.audioDebt ? *extras.audioDebt : 0.0);
        w.boolean(extras.mapper != nullptr);
        if (extras.mapper) input::SaveStateAccess::save(w, *extras.mapper);
        w.endChunk(core);
        SaveStateAccess::saveSession(w, session);
        const auto payload = w.written(SaveStateHeaderSize);
        // header
        std::uint8_t* h = w.mutableResult().data();
        std::memcpy(h, SaveStateMagic.data(), 8);
        const auto put32 = [&](std::size_t at, std::uint32_t v) { std::memcpy(h + at, &v, 4); };
        const auto put64 = [&](std::size_t at, std::uint64_t v) { std::memcpy(h + at, &v, 8); };
        put32(8, SaveStateVersion);
        put32(12, static_cast<std::uint32_t>(SaveStateHeaderSize));
        put64(16, payload.size());
        put32(56, 0);   // flags
        put32(60, 0);   // reserved
        put64(32, SaveStateAccess::fingerprint(session, *engine));
        put32(40, static_cast<std::uint32_t>(session.width()));
        put32(44, static_cast<std::uint32_t>(session.height()));
        put64(48, SaveStateAccess::frames(session));
        put64(24, stateChecksum({h, SaveStateHeaderSize}, payload));
        out.resize(w.size());
    } catch (const std::exception& e) {
        m_error = e.what();
        return false;
    }
    m_saveMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

inline bool SaveStateManager::load(Session& session, std::span<const std::uint8_t> data, const Extras& extras) {
    const auto t0 = std::chrono::steady_clock::now();
    m_error.clear();
    const auto* engine = dynamic_cast<const GameEngine*>(&session.engine());
    if (!engine) {
        m_error = "the engine has no save states";
        return false;
    }
    try {
        if (data.size() < SaveStateHeaderSize) throw state::Error("not a Gromada save state (too short)");
        if (std::memcmp(data.data(), SaveStateMagic.data(), 8) != 0) throw state::Error("not a Gromada save state");
        const auto get32 = [&](std::size_t at) { std::uint32_t v; std::memcpy(&v, data.data() + at, 4); return v; };
        const auto get64 = [&](std::size_t at) { std::uint64_t v; std::memcpy(&v, data.data() + at, 8); return v; };
        if (get32(8) != SaveStateVersion) throw state::Error(std::format("save state version {} (this core reads {})", get32(8), SaveStateVersion));
        if (get32(12) != SaveStateHeaderSize) throw state::Error("bad save state header");
        const std::uint64_t size = get64(16);
        if (size > data.size() - SaveStateHeaderSize) throw state::Error("truncated save state");
        const auto payload = data.subspan(SaveStateHeaderSize, static_cast<std::size_t>(size));
        if (stateChecksum(data.first(SaveStateHeaderSize), payload) != get64(24)) throw state::Error("corrupted save state (checksum mismatch)");
        if (get32(56) != 0 || get32(60) != 0) throw state::Error("save state with unknown flags");
        if (get64(32) != SaveStateAccess::fingerprint(session, *engine))
            throw state::Error("save state of different game data");

        state::Reader r{payload};
        state::Reader core = r.chunk(state::fourcc("CORE"));
        const double audioDebt = core.finite64();
        std::optional<input::MapperState> mapper;
        if (core.boolean()) {
            auto decoded = input::SaveStateAccess::decode(core);
            if (extras.mapper) mapper = std::move(decoded);
        }
        core.expectEnd("core");
        SessionStaged staged = SaveStateAccess::decodeSession(r, session, *this);
        r.expectEnd("save state");

        // commit (cannot fail from here)
        if (mapper) input::SaveStateAccess::apply(*extras.mapper, std::move(*mapper));
        if (extras.audioDebt) *extras.audioDebt = audioDebt;
        SaveStateAccess::applySession(session, std::move(staged));
    } catch (const std::exception& e) {
        m_error = e.what();
        return false;
    }
    m_loadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

// ---------------------------------------------------------------------------- tripwire
// The save states list every field of these classes by hand. A field added to one of them
// changes its size and stops the build here (only on desktop x86-64 + libc++, where the sizes
// were measured): add the field to the state (game/savestate*.hpp or the module's *_state.hpp,
// see docs/savestates.md "Aggiungere un campo"), bump SaveStateVersion if the format changes,
// then update the size below. GROMADA_NO_SAVESTATE_TRIPWIRE turns the check off.
#if defined(__x86_64__) && defined(_LIBCPP_VERSION) && !defined(GROMADA_NO_SAVESTATE_TRIPWIRE)
#define GROMADA_SAVESTATE_TRIPWIRE(T, n) \
    static_assert(sizeof(T) == (n), "save states: " #T " changed size; serialize the new field (docs/savestates.md) and update this check")
GROMADA_SAVESTATE_TRIPWIRE(World, 2848);
GROMADA_SAVESTATE_TRIPWIRE(GameEngine, 3184);
GROMADA_SAVESTATE_TRIPWIRE(Session, 2768);
GROMADA_SAVESTATE_TRIPWIRE(SessionUi, 352);
GROMADA_SAVESTATE_TRIPWIRE(SaveFiles, 128);
GROMADA_SAVESTATE_TRIPWIRE(InputRecordBuilder, 104);
GROMADA_SAVESTATE_TRIPWIRE(DemoPlayer, 160);
GROMADA_SAVESTATE_TRIPWIRE(DemoRecorder, 24);
GROMADA_SAVESTATE_TRIPWIRE(SoundChannels, 608);
GROMADA_SAVESTATE_TRIPWIRE(Sprite, 168);
GROMADA_SAVESTATE_TRIPWIRE(VidInfo, 288);
GROMADA_SAVESTATE_TRIPWIRE(Player, 36);
GROMADA_SAVESTATE_TRIPWIRE(Group, 32);
GROMADA_SAVESTATE_TRIPWIRE(CommandStack, 32);
GROMADA_SAVESTATE_TRIPWIRE(ScreenObject, 64);
GROMADA_SAVESTATE_TRIPWIRE(MenuState, 16);
GROMADA_SAVESTATE_TRIPWIRE(script::Interpreter, 320);
GROMADA_SAVESTATE_TRIPWIRE(script::StringMemory, 24);
GROMADA_SAVESTATE_TRIPWIRE(ui::TextManager, 112);
GROMADA_SAVESTATE_TRIPWIRE(ui::ScreenText, 56);
GROMADA_SAVESTATE_TRIPWIRE(input::Mapper, 488);
GROMADA_SAVESTATE_TRIPWIRE(input::PcState, 112);
GROMADA_SAVESTATE_TRIPWIRE(input::RawInput, 112);
GROMADA_SAVESTATE_TRIPWIRE(Mixer, 248);
GROMADA_SAVESTATE_TRIPWIRE(ScreenEffects, 112);
GROMADA_SAVESTATE_TRIPWIRE(FollowCamera, 56);
GROMADA_SAVESTATE_TRIPWIRE(WeaponHud, 12);
inline void SaveStateAccess::tripwire() {
    GROMADA_SAVESTATE_TRIPWIRE(SoundChannels::Request, 12);
    GROMADA_SAVESTATE_TRIPWIRE(SoundChannels::Channel, 12);
    GROMADA_SAVESTATE_TRIPWIRE(SaveFiles::File, 64);
    GROMADA_SAVESTATE_TRIPWIRE(Session::MixerSink::Buffer, 24);
}
#undef GROMADA_SAVESTATE_TRIPWIRE
#endif

} // namespace gromada::game
