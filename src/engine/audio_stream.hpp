#pragma once
// Streaming PCM source pulled by gromada::Mixer (e.g. a movie soundtrack), at any sample
// rate: the mixer resamples it to the output rate. Kept in its own header so producers
// (src/video) depend only on this interface, not on the mixer.

#include <cstddef>
#include <cstdint>

namespace gromada {

class AudioStream {
public:
    virtual ~AudioStream() = default;
    // Sample rate of the frames returned by read(), in Hz.
    [[nodiscard]] virtual int sampleRate() const = 0;
    // Writes up to `frames` interleaved stereo int16 frames; returns how many were written.
    // Returning fewer than requested means the stream has ended.
    virtual std::size_t read(std::int16_t* stereo, std::size_t frames) = 0;
    // Moves to source frame `frame` (save states); false if the stream cannot.
    virtual bool seek(std::uint64_t frame) { (void)frame; return false; }
};

} // namespace gromada
