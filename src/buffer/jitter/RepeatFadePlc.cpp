#include "buffer/jitter/RepeatFadePlc.hpp"

#include <algorithm>

const std::int16_t* RepeatFadePlc::good(const std::int16_t* samples, std::size_t frames) {
    std::copy_n(samples, frames, last_.begin());
    haveLast_ = true;
    losses_ = 0;
    return last_.data();
}

const std::int16_t* RepeatFadePlc::conceal(std::size_t frames) {
    ++losses_;
    if (!haveLast_ || losses_ > kFadeChunks) {
        std::fill_n(concealed_.begin(), frames, std::int16_t{0});
        return concealed_.data();
    }
    // Gain falls linearly across the whole fade, so chunk boundaries do not
    // step.
    const double start = 1.0 - static_cast<double>(losses_ - 1) / kFadeChunks;
    const double step = 1.0 / (static_cast<double>(kFadeChunks) * static_cast<double>(frames));
    for (std::size_t i = 0; i < frames; ++i) {
        const double gain = start - step * static_cast<double>(i);
        concealed_[i] = static_cast<std::int16_t>(last_[i] * gain);
    }
    return concealed_.data();
}
