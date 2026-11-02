#pragma once
// MoviePlayer of the game session (PlayAVI) on top of src/video: the core's implementation.
// Kept apart from session.hpp so the session (and its tests) do not need the video decoders.

#include "game/session.hpp"
#include "video/movie.hpp"

namespace gromada::game {

class VideoMoviePlayer final : public MoviePlayer {
public:
    bool open(const std::filesystem::path& file) override {
        try {
            m_movie = std::make_unique<video::Movie>(file);
            return true;
        } catch (const std::exception&) {
            m_movie.reset();
            return false;
        }
    }
    bool step(double dt) override {
        if (!m_movie || m_movie->finished())
            return false;
        m_movie->step(dt);
        return !m_movie->finished();
    }
    void blit(std::uint32_t* xrgb, int width, int height, std::size_t pitchPixels) override {
        if (m_movie)
            m_movie->blit(xrgb, width, height, pitchPixels);
    }
    AudioStream* audio() override { return m_movie ? m_movie->audio() : nullptr; }
    void close() override { m_movie.reset(); }

private:
    std::unique_ptr<video::Movie> m_movie;
};

} // namespace gromada::game
