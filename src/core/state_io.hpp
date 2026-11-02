#pragma once
// Binary writer/reader for save states (issue #14, docs/savestates.md).
//
// - Writer: appends little-endian fixed-width fields to a growable byte buffer; chunks are
//   {u32 tag, u32 size, payload} with the size patched when the chunk ends.
// - Reader: bounds-checked reads over a byte span; every error (truncation, bad tag, absurd
//   counts) throws state::Error, so a corrupted or foreign state is rejected without
//   touching memory outside the span or allocating unbounded amounts.
// - crc32: integrity check of the payload (IEEE 802.3 polynomial, table driven).
//
// Plain fixed-width encoding on purpose: offsets stay stable from one state to the next,
// which keeps the frontend's rewind deltas small. Header-only; depends on the standard
// library only.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace gromada::state {

static_assert(std::endian::native == std::endian::little, "save states assume a little-endian target");

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

constexpr std::uint32_t fourcc(const char (&s)[5]) noexcept {
    return std::uint32_t(std::uint8_t(s[0])) | std::uint32_t(std::uint8_t(s[1])) << 8 |
           std::uint32_t(std::uint8_t(s[2])) << 16 | std::uint32_t(std::uint8_t(s[3])) << 24;
}

inline std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc = 0) noexcept {
    static constexpr auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (const std::uint8_t b : data) crc = table[(crc ^ b) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

// 64-bit FNV-1a (identity of scripts and data files, not a security hash).
inline std::uint64_t fnv1a(std::span<const std::uint8_t> data, std::uint64_t h = 0xcbf29ce484222325ull) noexcept {
    for (const std::uint8_t b : data) {
        h ^= b;
        h *= 0x100000001b3ull;
    }
    return h;
}
inline std::uint64_t fnv1a(std::string_view s, std::uint64_t h = 0xcbf29ce484222325ull) noexcept {
    return fnv1a(std::span{reinterpret_cast<const std::uint8_t*>(s.data()), s.size()}, h);
}

class Writer {
public:
    // Writes from the start of `out`, which is used as growable storage: it is never shrunk,
    // so a buffer reused from one state to the next is not reallocated. result() is the data.
    explicit Writer(std::vector<std::uint8_t>& out) : m_buf{out} {}

    [[nodiscard]] std::size_t size() const noexcept { return m_pos; }
    [[nodiscard]] std::span<const std::uint8_t> result() const noexcept { return {m_buf.data(), m_pos}; }
    [[nodiscard]] std::span<std::uint8_t> mutableResult() noexcept { return {m_buf.data(), m_pos}; }

    template <class T>
        requires(std::is_arithmetic_v<T> || std::is_enum_v<T>)
    void put(T v) {
        std::uint8_t* p = grow(sizeof(T));
        std::memcpy(p, &v, sizeof(T));
    }
    void u8(std::uint8_t v) { put(v); }
    void u16(std::uint16_t v) { put(v); }
    void u32(std::uint32_t v) { put(v); }
    void u64(std::uint64_t v) { put(v); }
    void i8(std::int8_t v) { put(v); }
    void i16(std::int16_t v) { put(v); }
    void i32(std::int32_t v) { put(v); }
    void i64(std::int64_t v) { put(v); }
    void f32(float v) { put(std::bit_cast<std::uint32_t>(v)); }
    void f64(double v) { put(std::bit_cast<std::uint64_t>(v)); }
    void boolean(bool v) { u8(v ? 1 : 0); }
    void bytes(const void* data, std::size_t n) {
        if (!n) return;
        std::memcpy(grow(n), data, n);
    }
    void count(std::size_t n) {
        if (n > std::numeric_limits<std::uint32_t>::max()) throw Error("save state: count too large");
        u32(static_cast<std::uint32_t>(n));
    }
    void str(std::string_view s) {
        count(s.size());
        bytes(s.data(), s.size());
    }
    void blob(std::span<const std::uint8_t> b) {
        count(b.size());
        bytes(b.data(), b.size());
    }
    // Array of trivially copyable values, raw (count first).
    template <class T>
        requires std::is_trivially_copyable_v<T>
    void array(std::span<const T> a) {
        count(a.size());
        bytes(a.data(), a.size() * sizeof(T));
    }

    // Chunks: begin returns a token for end.
    std::size_t beginChunk(std::uint32_t tag) {
        u32(tag);
        const std::size_t at = m_pos;
        u32(0);
        return at;
    }
    void endChunk(std::size_t token) {
        const std::size_t n = m_pos - token - 4;
        if (n > std::numeric_limits<std::uint32_t>::max()) throw Error("save state: chunk too large");
        const auto v = static_cast<std::uint32_t>(n);
        std::memcpy(m_buf.data() + token, &v, 4);
    }
    // Overwrites a u32 written earlier at `at`.
    void patch32(std::size_t at, std::uint32_t v) { std::memcpy(m_buf.data() + at, &v, 4); }
    std::span<const std::uint8_t> written(std::size_t from) const { return {m_buf.data() + from, m_pos - from}; }

private:
    std::uint8_t* grow(std::size_t n) {
        if (m_pos + n > m_buf.size()) m_buf.resize(std::max<std::size_t>(m_pos + n, m_buf.size() * 2 + 4096));
        std::uint8_t* p = m_buf.data() + m_pos;
        m_pos += n;
        return p;
    }

    std::vector<std::uint8_t>& m_buf;
    std::size_t m_pos = 0;
};

class Reader {
public:
    Reader() = default;
    explicit Reader(std::span<const std::uint8_t> data) : m_data{data} {}

    [[nodiscard]] std::size_t remaining() const noexcept { return m_data.size() - m_pos; }
    [[nodiscard]] std::size_t position() const noexcept { return m_pos; }
    [[nodiscard]] bool atEnd() const noexcept { return m_pos == m_data.size(); }
    void expectEnd(const char* what) const {
        if (!atEnd()) throw Error(std::string{"save state: trailing bytes in "} + what);
    }

    template <class T>
        requires(std::is_arithmetic_v<T> || std::is_enum_v<T>)
    T get() {
        T v;
        std::memcpy(&v, take(sizeof(T)), sizeof(T));
        return v;
    }
    std::uint8_t u8() { return get<std::uint8_t>(); }
    std::uint16_t u16() { return get<std::uint16_t>(); }
    std::uint32_t u32() { return get<std::uint32_t>(); }
    std::uint64_t u64() { return get<std::uint64_t>(); }
    std::int8_t i8() { return get<std::int8_t>(); }
    std::int16_t i16() { return get<std::int16_t>(); }
    std::int32_t i32() { return get<std::int32_t>(); }
    std::int64_t i64() { return get<std::int64_t>(); }
    float f32() { return std::bit_cast<float>(u32()); }
    double f64() { return std::bit_cast<double>(u64()); }
    bool boolean() {
        const std::uint8_t v = u8();
        if (v > 1) throw Error("save state: bad boolean");
        return v != 0;
    }
    // A finite float/double (NaN or infinity in a timer would never count down).
    float finite32() {
        const float v = f32();
        if (!(v == v) || v > 3.0e38f || v < -3.0e38f) throw Error("save state: non-finite value");
        return v;
    }
    double finite64() {
        const double v = f64();
        if (!(v == v) || v > 1.0e300 || v < -1.0e300) throw Error("save state: non-finite value");
        return v;
    }
    // Integer in [lo, hi].
    template <class T>
    T ranged(T lo, T hi, const char* what) {
        const T v = get<T>();
        if (v < lo || v > hi) throw Error(std::string{"save state: value out of range: "} + what);
        return v;
    }
    void bytes(void* out, std::size_t n) {
        if (!n) return;
        std::memcpy(out, take(n), n);
    }
    std::span<const std::uint8_t> view(std::size_t n) { return {take(n), n}; }
    // Element count, checked against the bytes left (each element needs at least
    // `minElementBytes`) and an absolute limit.
    std::size_t count(std::size_t minElementBytes, std::size_t limit = 1u << 26) {
        const std::uint32_t n = u32();
        if (n > limit) throw Error("save state: count over limit");
        if (minElementBytes && n > remaining() / minElementBytes) throw Error("save state: truncated (count)");
        return n;
    }
    std::string str(std::size_t limit = 1u << 24) {
        const std::size_t n = count(1, limit);
        const auto v = view(n);
        return std::string{reinterpret_cast<const char*>(v.data()), n};
    }
    std::vector<std::uint8_t> blob(std::size_t limit = 1u << 26) {
        const std::size_t n = count(1, limit);
        const auto v = view(n);
        return {v.begin(), v.end()};
    }
    template <class T>
        requires std::is_trivially_copyable_v<T>
    std::vector<T> array(std::size_t limit = 1u << 26) {
        const std::size_t n = count(sizeof(T), limit);
        std::vector<T> out(n);
        bytes(out.data(), n * sizeof(T));
        return out;
    }

    // The next chunk, which must carry `tag`.
    Reader chunk(std::uint32_t tag) {
        const std::uint32_t t = u32();
        if (t != tag) throw Error("save state: unexpected chunk");
        const std::uint32_t n = u32();
        return Reader{view(n)};
    }

private:
    const std::uint8_t* take(std::size_t n) {
        if (n > remaining()) throw Error("save state: truncated");
        const std::uint8_t* p = m_data.data() + m_pos;
        m_pos += n;
        return p;
    }

    std::span<const std::uint8_t> m_data;
    std::size_t m_pos = 0;
};

} // namespace gromada::state
