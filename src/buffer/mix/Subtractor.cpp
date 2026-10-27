#include "buffer/mix/Subtractor.hpp"

#include <algorithm>
#include <limits>

Subtractor::Subtractor(Mixer* sum, Input* own)
    : Input(sum->frames()), sum_(sum), own_(own), out_(sum->frames()) {}

const std::int16_t* Subtractor::produce() {
    sum_->pull(tick()); // makes sure rawSum() is this tick's
    const std::int32_t* sum = sum_->rawSum();
    const std::int16_t* own = own_->pull(tick());

    constexpr std::int32_t low = std::numeric_limits<std::int16_t>::min();
    constexpr std::int32_t high = std::numeric_limits<std::int16_t>::max();
    for (std::size_t s = 0; s < out_.size(); ++s) {
        out_[s] = static_cast<std::int16_t>(std::clamp(sum[s] - own[s], low, high));
    }
    return out_.data();
}
