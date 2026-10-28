#pragma once

#include "buffer/jitter/JitterTypes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

// Packet loss concealment: repeat the last good chunk, fading it out over a few
// losses, then silence. Cheap and good enough to hide a single lost packet.
//
// TODO: ITU-T G.711 Appendix I (pitch-period repetition) sounds much better on
// speech; it would slot in here with the same two methods.
class RepeatFadePlc {
  public:
    explicit RepeatFadePlc(const JitterConfig& config) : fadeChunks_(config.fadeChunks) {}

    // A real chunk is played: keeps a copy for the next loss and returns it, so
    // that copy is also the chunk the jitter buffer hands out.
    const std::int16_t* good(const std::int16_t* samples, std::size_t frames);

    // A chunk is missing: returns a stand-in, valid until the next call.
    const std::int16_t* conceal(std::size_t frames);

  private:
    std::size_t fadeChunks_;
    std::array<std::int16_t, kMaxFrames> last_{};
    std::array<std::int16_t, kMaxFrames> concealed_{};
    bool haveLast_ = false;
    std::size_t losses_ = 0;
};
