// Movie module tests that need no game data (they run in CI): AVI demuxing of synthetic
// files, MS Video 1 and Indeo 3 decoding of hand-built frames, the mixer's streaming source
// and the player's clock / A-V sync at 60 and 120 fps.
#include "engine/audio.hpp"
#include "video/movie.hpp"
#include "test.hpp"

#include <cstdio>

using namespace gromada::video;

namespace {

// --- synthetic AVI writer ------------------------------------------------------------------

struct Bytes {
    std::vector<std::uint8_t> v;
    void u8(unsigned x) { v.push_back(static_cast<std::uint8_t>(x)); }
    void u16(unsigned x) { u8(x); u8(x >> 8); }
    void u32(std::uint32_t x) { u16(x & 0xFFFF); u16(x >> 16); }
    void id(const char* s) { for (int i = 0; i < 4; ++i) u8(static_cast<unsigned char>(s[i])); }
    void bytes(const std::vector<std::uint8_t>& b) { v.insert(v.end(), b.begin(), b.end()); }
    void patch32(std::size_t at, std::uint32_t x) { for (int i = 0; i < 4; ++i) v[at + i] = static_cast<std::uint8_t>(x >> (8 * i)); }
    // chunk / list helpers: begin returns the size field position
    std::size_t begin(const char* fourcc, const char* type = nullptr) {
        id(fourcc);
        const std::size_t at = v.size();
        u32(0);
        if (type) id(type);
        return at;
    }
    void end(std::size_t at) {
        patch32(at, static_cast<std::uint32_t>(v.size() - at - 4));
        if (v.size() & 1) u8(0);
    }
};

struct SynthMovie {
    int width = 16, height = 16, frames = 30;
    int audioRate = 22050;
    bool idx1 = true, recLists = false;
    std::vector<int> dropFrames; // zero-sized video chunks
};

// Frame k: a single colour, RGB555 (k + 1) in blue, coded with one-colour blocks.
std::vector<std::uint8_t> cramSolidFrame(int width, int height, int k) {
    Bytes b;
    const unsigned color = 0x8000u | static_cast<unsigned>((k + 1) & 31); // bit 15 set: one-colour block
    for (int i = 0; i < (width / 4) * (height / 4); ++i) b.u16(color);
    return b.v;
}

// The soundtrack encodes time: during video frame k both channels hold 128 + k (u8).
std::vector<std::uint8_t> timeCodedAudio(int rate, int fps, std::size_t from, std::size_t count) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = from; i < from + count; ++i) {
        const auto k = static_cast<std::uint8_t>(128 + (i * fps / rate) % 100);
        out.push_back(k);
        out.push_back(k);
    }
    return out;
}

void writeAvi(const std::filesystem::path& path, const SynthMovie& m) {
    struct Entry { const char* id; std::uint32_t flags, offset, size; };
    std::vector<Entry> index;
    const std::size_t audioFrames = static_cast<std::size_t>(m.frames) * m.audioRate / 15;

    Bytes f;
    const auto riff = f.begin("RIFF", "AVI ");
    const auto hdrl = f.begin("LIST", "hdrl");
    const auto avih = f.begin("avih");
    f.u32(1000000 / 15); f.u32(0); f.u32(0); f.u32(0x10); f.u32(m.frames); f.u32(0); f.u32(2); f.u32(0);
    f.u32(m.width); f.u32(m.height); f.u32(0); f.u32(0); f.u32(0); f.u32(0);
    f.end(avih);
    {   // video stream
        const auto strl = f.begin("LIST", "strl");
        const auto strh = f.begin("strh");
        f.id("vids"); f.id("MSVC"); f.u32(0); f.u32(0); f.u32(0); f.u32(1); f.u32(15); f.u32(0); f.u32(m.frames);
        f.u32(0); f.u32(10000); f.u32(0); f.u16(0); f.u16(0); f.u16(m.width); f.u16(m.height);
        f.end(strh);
        const auto strf = f.begin("strf");
        f.u32(40); f.u32(m.width); f.u32(m.height); f.u16(1); f.u16(16); f.id("CRAM");
        f.u32(m.width * m.height * 2); f.u32(0); f.u32(0); f.u32(0); f.u32(0);
        f.end(strf);
        f.end(strl);
    }
    {   // audio stream: PCM u8 stereo
        const auto strl = f.begin("LIST", "strl");
        const auto strh = f.begin("strh");
        f.id("auds"); f.u32(0); f.u32(0); f.u32(0); f.u32(0); f.u32(2); f.u32(m.audioRate * 2); f.u32(0);
        f.u32(static_cast<std::uint32_t>(audioFrames)); f.u32(0); f.u32(10000); f.u32(2); f.u32(0); f.u32(0);
        f.end(strh);
        const auto strf = f.begin("strf");
        f.u16(1); f.u16(2); f.u32(m.audioRate); f.u32(m.audioRate * 2); f.u16(2); f.u16(8);
        f.end(strf);
        f.end(strl);
    }
    f.end(hdrl);

    const auto movi = f.begin("LIST", "movi");
    const std::size_t moviBase = movi + 4; // the 'movi' fourcc: idx1 offsets are relative to it
    const auto chunk = [&](const char* id, const std::vector<std::uint8_t>& data, bool key) {
        index.push_back({id, key ? 0x10u : 0u, static_cast<std::uint32_t>(f.v.size() - moviBase), static_cast<std::uint32_t>(data.size())});
        const auto c = f.begin(id);
        f.bytes(data);
        f.end(c);
    };
    // Audio is interleaved one second ahead, in chunks of half a second (like the game's files).
    const std::size_t audioChunk = static_cast<std::size_t>(m.audioRate) / 2;
    std::size_t audioDone = 0;
    for (int k = 0; k < m.frames; ++k) {
        const std::size_t audioWanted = std::min(audioFrames, (static_cast<std::size_t>(k) + 15) * m.audioRate / 15);
        std::size_t rec = 0;
        if (m.recLists) rec = f.begin("LIST", "rec ");
        while (audioDone < audioWanted) {
            const std::size_t n = std::min(audioChunk, audioFrames - audioDone);
            chunk("01wb", timeCodedAudio(m.audioRate, 15, audioDone, n), true);
            audioDone += n;
        }
        const bool drop = std::ranges::find(m.dropFrames, k) != m.dropFrames.end();
        chunk("00dc", drop ? std::vector<std::uint8_t>{} : cramSolidFrame(m.width, m.height, k), !drop);
        if (m.recLists) f.end(rec);
    }
    f.end(movi);
    if (m.idx1) {
        const auto idx = f.begin("idx1");
        for (const auto& e : index) { f.id(e.id); f.u32(e.flags); f.u32(e.offset); f.u32(e.size); }
        f.end(idx);
    }
    f.end(riff);
    std::ofstream out{path, std::ios::binary};
    out.write(reinterpret_cast<const char*>(f.v.data()), static_cast<std::streamsize>(f.v.size()));
}

std::filesystem::path tempPath(const char* name) {
    return std::filesystem::temp_directory_path() / std::format("gromada_video_test_{}_{}", name, static_cast<long>(std::rand()));
}

// --- tests -----------------------------------------------------------------------------------

void testAviParsing() {
    const auto path = tempPath("avi");
    writeAvi(path, {.frames = 20, .dropFrames = {5}});
    AviFile avi{path};
    CHECK(avi.indexFromIdx1());
    CHECK_EQ(avi.streams().size(), std::size_t{2});
    const int vs = avi.findStream(fourcc("vids")), as = avi.findStream(fourcc("auds"));
    CHECK_EQ(vs, 0);
    CHECK_EQ(as, 1);
    const auto& v = avi.streams()[vs];
    CHECK_EQ(v.video.compression, fourcc("CRAM"));
    CHECK_EQ(v.video.width, 16);
    CHECK_EQ(v.video.bitCount, std::uint16_t{16});
    CHECK_EQ(v.rate, 15u);
    CHECK_EQ(v.chunks.size(), std::size_t{20});
    CHECK_EQ(v.chunks[5].size, 0u);
    CHECK(!v.chunks[5].keyframe && v.chunks[4].keyframe);
    const auto& a = avi.streams()[as];
    CHECK_EQ(a.audio.sampleRate, 22050u);
    CHECK_EQ(a.audio.channels, std::uint16_t{2});
    CHECK_EQ(a.audio.bitsPerSample, std::uint16_t{8});
    std::size_t audioBytes = 0;
    for (const auto& c : a.chunks) audioBytes += c.size;
    CHECK_EQ(audioBytes, std::size_t{20 * 22050 / 15 * 2});

    // idx1 and the movi walk must agree
    const auto scanned = avi.scanMovi();
    for (std::size_t s = 0; s < 2; ++s) {
        const auto& idx = avi.streams()[s].chunks;
        CHECK_EQ(scanned[s].size(), idx.size());
        for (std::size_t i = 0; i < std::min(idx.size(), scanned[s].size()); ++i)
            CHECK(scanned[s][i].offset == idx[i].offset && scanned[s][i].size == idx[i].size && scanned[s][i].id == idx[i].id);
    }
    std::vector<std::uint8_t> buf;
    CHECK(avi.read(v.chunks[3], buf));
    CHECK(buf == cramSolidFrame(16, 16, 3));
    std::filesystem::remove(path);

    // no idx1, chunks grouped in 'rec ' lists: the movi walk finds the same frames
    const auto path2 = tempPath("avi_rec");
    writeAvi(path2, {.frames = 20, .idx1 = false, .recLists = true});
    AviFile avi2{path2};
    CHECK(!avi2.indexFromIdx1());
    CHECK_EQ(avi2.streams()[0].chunks.size(), std::size_t{20});
    CHECK(avi2.read(avi2.streams()[0].chunks[19], buf));
    CHECK(buf == cramSolidFrame(16, 16, 19));
    std::filesystem::remove(path2);

    // not an AVI
    const auto path3 = tempPath("bad");
    { std::ofstream o{path3, std::ios::binary}; o << "RIFF\x04\0\0\0WAVE"; }
    bool threw = false;
    try { AviFile bad{path3}; } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    std::filesystem::remove(path3);
}

// pixel (x, y) of block (bx, by) of an 8x8 picture, top-down
template <typename T>
T at(std::span<const T> pix, int bx, int by, int x, int y) { return pix[(by * 4 + y) * 8 + bx * 4 + x]; }

void testMsVideo1() {
    // 16-bit, 8x8 picture = 2x2 blocks; blocks are coded bottom row first, left to right.
    Bytes f;
    f.u16(0x801F);                              // bottom-left: one colour (0x001F)
    f.u16(0x0001); f.u16(0x7C00); f.u16(0x03E0); // bottom-right: 2 colours, only bit 0 set
    f.u16(0x0421);                              // top-left: 8 colours (bit 15 of the first colour), flags 0x0421
    f.u16(0x8000 | 0x0100); f.u16(0x0101);      //  quadrant colours: bottom-left pair
    f.u16(0x0200); f.u16(0x0201);               //  bottom-right pair
    f.u16(0x0300); f.u16(0x0301);               //  top-left pair
    f.u16(0x0400); f.u16(0x0401);               //  top-right pair
    f.u16(0x8401);                              // top-right: skip 1 block
    MsVideo1Decoder d{8, 8, 16};
    CHECK(d.decode(f.v));
    const auto px = d.rgb555();
    // bottom-left block (bx 0, by 1): uniform
    CHECK_EQ(at(px, 0, 1, 0, 0), std::uint16_t{0x001F});
    CHECK_EQ(at(px, 0, 1, 3, 3), std::uint16_t{0x001F});
    // bottom-right: bit 0 = bottom-left pixel of the block -> first colour, others second
    CHECK_EQ(at(px, 1, 1, 0, 3), std::uint16_t{0x7C00});
    CHECK_EQ(at(px, 1, 1, 1, 3), std::uint16_t{0x03E0});
    CHECK_EQ(at(px, 1, 1, 0, 0), std::uint16_t{0x03E0});
    // top-left: flags 0x0421 -> bits 0, 5, 10: (x0,row0), (x1,row1), (x2,row2) from the bottom
    CHECK_EQ(at(px, 0, 0, 0, 3), std::uint16_t{0x0100}); // bottom-left quadrant, set -> first (bit 15 dropped)
    CHECK_EQ(at(px, 0, 0, 1, 3), std::uint16_t{0x0101}); // bottom-left quadrant, clear -> second
    CHECK_EQ(at(px, 0, 0, 1, 2), std::uint16_t{0x0100}); // bit 5
    CHECK_EQ(at(px, 0, 0, 2, 1), std::uint16_t{0x0400}); // bit 10: top-right quadrant, set
    CHECK_EQ(at(px, 0, 0, 3, 3), std::uint16_t{0x0201}); // bottom-right quadrant, clear
    CHECK_EQ(at(px, 0, 0, 3, 0), std::uint16_t{0x0401}); // top-right quadrant, clear
    CHECK_EQ(at(px, 1, 0, 0, 0), std::uint16_t{0});      // skipped: untouched
    // next frame: skip everything -> unchanged
    Bytes g;
    g.u16(0x8404);
    CHECK(d.decode(g.v));
    CHECK_EQ(at(d.rgb555(), 0, 1, 2, 2), std::uint16_t{0x001F});
    // truncated data is reported
    CHECK(!d.decode(std::vector<std::uint8_t>{0x01, 0x00, 0x00}));

    // 8-bit palettised: 1 colour (0x80..0x83 / 0x88..0x8F), 2 colours (< 0x80), 8 colours (>= 0x90)
    Bytes p;
    p.u8(7); p.u8(0x80);                          // bottom-left: index 7
    p.u8(0x01); p.u8(0x00); p.u8(10); p.u8(11);   // bottom-right: 2 colours, bit 0
    p.u8(0xFF); p.u8(0x90);                       // top-left: 8 colours, flags 0x90FF
    for (int i = 0; i < 8; ++i) p.u8(20 + i);
    p.u8(3); p.u8(0x8F);                          // top-right: index 3
    MsVideo1Decoder d8{8, 8, 8};
    CHECK(d8.decode(p.v));
    const auto ix = d8.indices();
    CHECK_EQ(at(ix, 0, 1, 2, 2), std::uint8_t{7});
    CHECK_EQ(at(ix, 1, 1, 0, 3), std::uint8_t{10});
    CHECK_EQ(at(ix, 1, 1, 3, 0), std::uint8_t{11});
    CHECK_EQ(at(ix, 0, 0, 0, 3), std::uint8_t{20}); // flags low byte 0xFF: bottom two rows set
    CHECK_EQ(at(ix, 0, 0, 3, 2), std::uint8_t{22});
    CHECK_EQ(at(ix, 0, 0, 0, 0), std::uint8_t{24}); // 0x90: bit 12 set (top row, x 0), top-left pair
    CHECK_EQ(at(ix, 0, 0, 1, 0), std::uint8_t{25}); // bit 13 clear
    CHECK_EQ(at(ix, 1, 0, 1, 1), std::uint8_t{3});
}

void testIndeo3Tables() {
    const auto& t = indeo3_detail::tables();
    static constexpr unsigned dyads[24] = {195, 159, 133, 115, 101, 93, 87, 77, 195, 159, 133, 115,
                                           101, 93, 87, 77, 128, 79, 79, 79, 79, 79, 79, 79};
    for (int i = 0; i < 24; ++i) {
        const auto& tab = t.vq[i];
        CHECK_EQ(tab.numDyads, dyads[i]);
        CHECK_EQ(tab.swapQuads, i >= 16);
        // every quad code must expand to valid dyads
        const unsigned quads = 248 - tab.numDyads;
        CHECK((quads - 1) / tab.quadBase < tab.numDyads && tab.quadBase <= tab.numDyads);
    }
    // first dyads of table 0: (0,0), (2,2), (-2,-2), (-1,3)
    CHECK_EQ(t.vq[0].delta2[0], 0);
    CHECK_EQ(t.vq[0].delta2[1], 2 + 2 * 256);
    CHECK_EQ(t.vq[0].delta2[2], -2 - 2 * 256);
    CHECK_EQ(t.vq[0].delta2[3], -1 + 3 * 256);
    CHECK_EQ(t.vq[0].delta4[3], static_cast<std::uint32_t>(-1 - 256 + 3 * 65536 + 3 * 16777216));
    for (const auto& section : t.requant)
        for (const auto v : section) CHECK(v < 128);
    CHECK_EQ(t.requant[1][7], std::uint8_t{10});
    CHECK_EQ(t.requant[3][0], std::uint8_t{4});
}

// A 16x16 Indeo 3 frame built by hand (all planes INTRA, mode 0).
std::vector<std::uint8_t> indeo3Frame(unsigned flags, const std::vector<std::uint8_t>& y, const std::vector<std::uint8_t>& c) {
    Bytes bs; // bitstream header + alt quant, then planes Y, V, U
    const std::uint32_t yOff = 48, vOff = yOff + static_cast<std::uint32_t>(y.size()), uOff = vOff + static_cast<std::uint32_t>(c.size());
    const std::uint32_t size = uOff + static_cast<std::uint32_t>(c.size());
    bs.u16(32); bs.u16(flags); bs.u32(size * 8); bs.u8(0); bs.u8(0); bs.u16(0);
    bs.u16(16); bs.u16(16); bs.u32(yOff); bs.u32(vOff); bs.u32(uOff); bs.u32(0);
    for (int i = 0; i < 16; ++i) bs.u8(0x02 + 0x12 * i);
    bs.bytes(y); bs.bytes(c); bs.bytes(c);
    Bytes f;
    const std::uint32_t frameNum = 7, word2 = 0, osSize = size;
    f.u32(frameNum); f.u32(word2); f.u32(frameNum ^ word2 ^ osSize ^ 0x46524D48u); f.u32(osSize);
    f.bytes(bs.v);
    return f.v;
}

void testIndeo3Synthetic() {
    // Y: tree INTRA + VQ DATA (0xB0); mode 0, codebook 0; block 0: row 0 = dyad (right pair
    // code 1 = +2, left pair code 2 = -2), rows 1-3 copied (0xFD); blocks 1-15: 0xFB run.
    const std::vector<std::uint8_t> y = {0, 0, 0, 0, 0xB0, 0x00, 0x01, 0x02, 0xFD, 0xFB, 0x0F};
    const std::vector<std::uint8_t> c = {0, 0, 0, 0, 0xB0, 0x00, 0xFD}; // chroma 4x4: copy rows
    Indeo3Decoder d;
    CHECK(d.decode(indeo3Frame(0x0004, y, c)) == Indeo3Decoder::Status::Decoded);
    CHECK_EQ(d.width(), 16);
    const auto Y = d.plane(0), U = d.plane(1);
    for (int row = 0; row < 16; ++row) {
        CHECK_EQ(int(Y.data[row * Y.pitch + 0]), 62); // 64 - 2
        CHECK_EQ(int(Y.data[row * Y.pitch + 1]), 62);
        CHECK_EQ(int(Y.data[row * Y.pitch + 2]), 66); // 64 + 2
        CHECK_EQ(int(Y.data[row * Y.pitch + 3]), 66);
        CHECK_EQ(int(Y.data[row * Y.pitch + 4]), 64);
        CHECK_EQ(int(Y.data[row * Y.pitch + 15]), 64);
    }
    CHECK_EQ(int(U.data[3 * U.pitch + 3]), 64);
    // Second frame into buffer 1: INTRA -> VQ NULL (0xA0) copies every plane from buffer 0.
    const std::vector<std::uint8_t> nul = {0, 0, 0, 0, 0xA0};
    CHECK(d.decode(indeo3Frame(0x0200, nul, nul)) == Indeo3Decoder::Status::Decoded);
    const auto Y2 = d.plane(0);
    CHECK(Y2.data != Y.data);
    CHECK_EQ(int(Y2.data[9 * Y2.pitch + 1]), 62);
    CHECK_EQ(int(Y2.data[9 * Y2.pitch + 3]), 66);
    // corrupted header: checksum
    auto bad = indeo3Frame(0x0004, y, c);
    bad[8] ^= 1;
    CHECK(d.decode(bad) == Indeo3Decoder::Status::Error);
    // truncated plane data: error, previous picture kept
    auto cut = indeo3Frame(0x0004, {0, 0, 0, 0, 0xB0, 0x00, 0x01}, c);
    CHECK(d.decode(cut) == Indeo3Decoder::Status::Error);
    CHECK_EQ(int(d.plane(0).data[2]), 66);
}

// Stereo ramp: frame i = (i * 8, -i * 8).
struct RampStream final : gromada::AudioStream {
    std::size_t pos = 0, length;
    explicit RampStream(std::size_t n) : length{n} {}
    int sampleRate() const override { return 22050; }
    std::size_t read(std::int16_t* out, std::size_t frames) override {
        std::size_t n = 0;
        for (; n < frames && pos < length; ++n, ++pos) {
            out[2 * n] = static_cast<std::int16_t>(pos * 8);
            out[2 * n + 1] = static_cast<std::int16_t>(-static_cast<int>(pos) * 8);
        }
        return n;
    }
};

void testMixerStream() {
    gromada::Mixer mixer{44100};
    std::vector<std::int16_t> out(2 * 735);
    mixer.render(out.data(), 735); // no stream: silence, as before
    CHECK(std::ranges::all_of(out, [](std::int16_t s) { return s == 0; }));

    RampStream ramp{2000};
    mixer.setStream(&ramp);
    CHECK(mixer.streamPlaying());
    mixer.render(out.data(), 735);
    CHECK(std::abs(mixer.streamPosition() - 367.5) < 1e-9);
    bool ok = true;
    for (int k = 0; k < 735; ++k) { // 22050 -> 44100: output k = source k/2, linear
        const double want = 8.0 * k / 2 * 32767 / 32768;
        ok &= std::abs(out[2 * k] - want) <= 1.0 && std::abs(out[2 * k + 1] + want) <= 1.0;
    }
    CHECK(ok);
    for (int i = 0; i < 6; ++i) mixer.render(out.data(), 735);
    CHECK(!mixer.streamPlaying()); // 2000 source frames = 4000 output frames < 7 * 735
    mixer.setStream(nullptr);
    mixer.render(out.data(), 735);
    CHECK(std::ranges::all_of(out, [](std::int16_t s) { return s == 0; }));
}

// Plays a synthetic movie like the core does and checks clock, duration and A/V sync.
void testPlayerSync(int fps) {
    const auto path = tempPath("player");
    const int frames = 30;
    writeAvi(path, {.frames = frames, .dropFrames = {7, 8}});
    Movie movie{path};
    CHECK_EQ(movie.frameCount(), std::size_t{30});
    CHECK(movie.audio() != nullptr);
    gromada::Mixer mixer{44100};
    mixer.setStream(movie.audio());
    std::vector<std::int16_t> out;
    double debt = 0;
    long steps = 0;
    int syncErrors = 0, frameErrors = 0;
    while (!movie.finished() && steps < 10000) {
        movie.step(1.0 / fps);
        // picture: solid colour k + 1 (blue), frames 7 and 8 are drop frames (frame 6 stays)
        const long shown = movie.currentFrame();
        const long expected = steps * 15 / fps;
        const long expectedPicture = (expected == 7 || expected == 8) ? 6 : expected;
        const int blue = static_cast<int>(movie.picture()[0] & 0xFF);
        if (shown != expected || blue != ((expectedPicture + 1) << 3 | (expectedPicture + 1) >> 2)) ++frameErrors;
        // audio played during this output frame starts with the time code of frame `shown`
        debt += 44100.0 / fps;
        const auto n = static_cast<std::size_t>(debt);
        debt -= n;
        out.resize(2 * n);
        const double clock = mixer.streamPosition() / 22050.0;
        mixer.render(out.data(), n);
        const long audioFrame = std::lround(out[0] / (256.0 * 32767 / 32768));
        if (audioFrame != shown || static_cast<long>(std::floor(clock * 15 + 1e-9)) != shown) ++syncErrors;
        ++steps;
    }
    std::fprintf(stderr, "player %d fps: %ld steps (%.3f s), frame errors %d, sync errors %d\n", fps, steps,
                 steps / double(fps), frameErrors, syncErrors);
    CHECK_EQ(steps, static_cast<long>(frames) * fps / 15); // 2 s
    CHECK_EQ(frameErrors, 0);
    CHECK_EQ(syncErrors, 0);
    CHECK(std::abs(mixer.streamPosition() - frames * 22050 / 15.0) < 1.0);

    // blit: 16x16 picture into 854x480 -> 480x480 centred, black bars
    std::vector<std::uint32_t> fb(854 * 480, 0xFFFFFFFF);
    movie.blit(fb.data(), 854, 480, 854);
    CHECK_EQ(fb[0], 0u);
    CHECK_EQ(fb[186], 0u);
    CHECK_EQ(fb[187], movie.picture()[0]);
    CHECK_EQ(fb[479 * 854 + 187 + 479], movie.picture()[255]);
    CHECK_EQ(fb[479 * 854 + 187 + 480], 0u);
    std::filesystem::remove(path);
}

} // namespace

int main() {
    std::srand(static_cast<unsigned>(std::time(nullptr)));
    testAviParsing();
    testMsVideo1();
    testIndeo3Tables();
    testIndeo3Synthetic();
    testMixerStream();
    testPlayerSync(60);
    testPlayerSync(120);
    return gromada_test::finish();
}
