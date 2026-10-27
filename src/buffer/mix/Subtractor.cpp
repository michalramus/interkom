#include "buffer/mix/Subtractor.hpp"

#include <algorithm>
#include <limits>

Subtractor::Subtractor(Mixer* sum, Input* own)
    : Input(sum->frames()), sum_(sum), own_(own), chunk_(sum->frames()) {}

void Subtractor::produce(std::int16_t* out) {
    sum_->pull(tick(), chunk_.data()); // makes sure rawSum() is this tick's
    const std::int32_t* sum = sum_->rawSum();
    own_->pull(tick(), chunk_.data());

    constexpr std::int32_t low = std::numeric_limits<std::int16_t>::min();
    constexpr std::int32_t high = std::numeric_limits<std::int16_t>::max();
    for (std::size_t s = 0; s < chunk_.size(); ++s) {
        out[s] = static_cast<std::int16_t>(std::clamp(sum[s] - chunk_[s], low, high));
    }
}
