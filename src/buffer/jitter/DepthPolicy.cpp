#include "buffer/jitter/DepthPolicy.hpp"

#include <algorithm>
#include <cmath>

std::size_t StaticDepth::shrink(std::size_t buffered) {
    return buffered > config_.maxChunks ? buffered - config_.depthChunks : 0;
}

AdaptiveDepth::AdaptiveDepth(const JitterConfig& config) : config_(config) {
    samples_.reserve(config_.latenessWindow);
    scratch_.reserve(config_.latenessWindow);
}

void AdaptiveDepth::record(std::int32_t lateness) {
    if (samples_.size() < config_.latenessWindow) {
        samples_.push_back(lateness);
    } else {
        samples_[next_] = lateness;
    }
    next_ = (next_ + 1) % config_.latenessWindow;
}

bool AdaptiveDepth::grow(std::size_t chunks, std::size_t buffered) {
    if (buffered + chunks > config_.maxChunks || percentile() <= 0) {
        return false;
    }
    shift(-static_cast<std::int32_t>(chunks));
    pulls_ = 0;
    return true;
}

bool AdaptiveDepth::hold(std::size_t buffered) {
    if (buffered >= config_.minChunks) {
        return false;
    }
    pulls_ = 0;
    lowest_ = std::numeric_limits<std::size_t>::max();
    return true;
}

std::size_t AdaptiveDepth::shrink(std::size_t buffered) {
    if (buffered > config_.maxChunks) {
        // Burst: back to the target at once, not just under the ceiling.
        const std::size_t chunks = buffered - config_.depthChunks;
        shift(static_cast<std::int32_t>(chunks));
        pulls_ = 0;
        lowest_ = std::numeric_limits<std::size_t>::max();
        return chunks;
    }
    lowest_ = std::min(lowest_, buffered);
    if (++pulls_ < config_.shrinkWindow) {
        return 0;
    }
    const std::size_t lowest = lowest_;
    pulls_ = 0;
    lowest_ = std::numeric_limits<std::size_t>::max();
    if (samples_.size() < config_.latenessWindow || lowest <= config_.minChunks ||
        percentile() > -1) {
        return 0;
    }
    shift(1);
    return 1;
}

std::int32_t AdaptiveDepth::percentile() {
    if (samples_.empty()) {
        return 0;
    }
    scratch_.assign(samples_.begin(), samples_.end()); // reserved: no allocation
    const auto index =
        static_cast<std::ptrdiff_t>(std::ceil(0.95 * static_cast<double>(scratch_.size() - 1)));
    std::nth_element(scratch_.begin(), scratch_.begin() + index, scratch_.end());
    return scratch_[static_cast<std::size_t>(index)];
}

void AdaptiveDepth::shift(std::int32_t chunks) {
    for (auto& sample : samples_) {
        sample += chunks;
    }
}
