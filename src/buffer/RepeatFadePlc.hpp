#pragma once

#include "buffer/JitterTypes.hpp"

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
    explicit RepeatFadePlc(const JitterConfig& /*config*/) {}

    // A real chunk was played; remember it for the next loss.
    void good(const std::int16_t* samples, std::size_t frames);

    void conceal(std::int16_t* out, std::size_t frames);

  private:
    static constexpr std::size_t kFadeChunks = 3;

    std::array<std::int16_t, kMaxFrames> last_{};
    bool haveLast_ = false;
    std::size_t losses_ = 0;
};
