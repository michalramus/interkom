#pragma once

#include "buffer/JitterTypes.hpp"

#include <algorithm>
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
    void good(const std::int16_t* samples, std::size_t frames) {
        std::copy_n(samples, frames, last_.begin());
        haveLast_ = true;
        losses_ = 0;
    }

    void conceal(std::int16_t* out, std::size_t frames) {
        ++losses_;
        if (!haveLast_ || losses_ > kFadeChunks) {
            std::fill_n(out, frames, std::int16_t{0});
            return;
        }
        // Gain falls linearly across the whole fade, so chunk boundaries do
        // not step.
        const double start = 1.0 - static_cast<double>(losses_ - 1) / kFadeChunks;
        const double step = 1.0 / (static_cast<double>(kFadeChunks) * static_cast<double>(frames));
        for (std::size_t i = 0; i < frames; ++i) {
            const double gain = start - step * static_cast<double>(i);
            out[i] = static_cast<std::int16_t>(last_[i] * gain);
        }
    }

  private:
    static constexpr std::size_t kFadeChunks = 3;

    std::array<std::int16_t, kMaxFrames> last_{};
    bool haveLast_ = false;
    std::size_t losses_ = 0;
};
