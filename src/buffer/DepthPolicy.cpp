#include "buffer/DepthPolicy.hpp"

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

std::size_t AdaptiveDepth::shrink(std::size_t buffered) {
    if (buffered > config_.maxChunks) {
        return buffered - config_.maxChunks; // burst; don't wait a window
    }
    if (++pulls_ < config_.shrinkWindow) {
        return 0;
    }
    pulls_ = 0;
    if (samples_.size() < config_.latenessWindow || buffered <= config_.minChunks ||
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
