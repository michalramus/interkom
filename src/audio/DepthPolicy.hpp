#pragma once

#include "audio/JitterTypes.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// A depth policy decides how much audio the jitter buffer holds. It never
// touches audio itself; the buffer asks it three things:
//
//   record(lateness)        every packet; chunks past its slot, <= 0 is on time
//   grow(chunks, buffered)  a late packet's slot was concealed: rewind to it?
//   shrink(buffered)        once per pull: how many chunks to drop
//
// `buffered` is chunks queued ahead of the playout cursor. All three run on the
// consumer thread.

// Fixed depth. Late packets are discarded; if the sender's clock outruns ours
// and the queue climbs past maxChunks, it is trimmed back to depthChunks.
class StaticDepth {
  public:
    explicit StaticDepth(const JitterConfig& config) : config_(config) {}

    void record(std::int32_t /*lateness*/) {}

    bool grow(std::size_t /*chunks*/, std::size_t /*buffered*/) { return false; }

    std::size_t shrink(std::size_t buffered) {
        return buffered > config_.maxChunks ? buffered - config_.depthChunks : 0;
    }

  private:
    JitterConfig config_;
};

// Depth follows the 95th percentile of packet lateness over the last
// latenessWindow packets, within [minChunks, maxChunks].
//
// Growing: when a late packet arrives for a slot that was already concealed
// and the percentile says packets are late often enough, the buffer rewinds
// its cursor to that packet and plays it. The concealed chunk becomes the
// inserted delay, and every later packet has that much more time.
//
// Shrinking: every shrinkWindow pulls, if 95% of packets arrived at least one
// chunk early, drop one chunk. Blind for now: it may land mid-word.
//
// Together the two also absorb clock drift: a slow sender shows up as
// lateness, a fast one as a growing early margin.
class AdaptiveDepth {
  public:
    explicit AdaptiveDepth(const JitterConfig& config) : config_(config) {
        samples_.reserve(config_.latenessWindow);
        scratch_.reserve(config_.latenessWindow);
    }

    void record(std::int32_t lateness) {
        if (samples_.size() < config_.latenessWindow) {
            samples_.push_back(lateness);
        } else {
            samples_[next_] = lateness;
        }
        next_ = (next_ + 1) % config_.latenessWindow;
    }

    bool grow(std::size_t chunks, std::size_t buffered) {
        if (buffered + chunks > config_.maxChunks || percentile() <= 0) {
            return false;
        }
        shift(-static_cast<std::int32_t>(chunks));
        pulls_ = 0;
        return true;
    }

    std::size_t shrink(std::size_t buffered) {
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

  private:
    // Rounded up, so with 100 samples it takes 5 late ones to go positive.
    std::int32_t percentile() {
        if (samples_.empty()) {
            return 0;
        }
        scratch_.assign(samples_.begin(), samples_.end()); // reserved: no allocation
        const auto index = static_cast<std::ptrdiff_t>(
            std::ceil(0.95 * static_cast<double>(scratch_.size() - 1)));
        std::nth_element(scratch_.begin(), scratch_.begin() + index, scratch_.end());
        return scratch_[static_cast<std::size_t>(index)];
    }

    // The cursor moved, so every remembered packet would now be this much later.
    void shift(std::int32_t chunks) {
        for (auto& sample : samples_) {
            sample += chunks;
        }
    }

    JitterConfig config_;
    std::vector<std::int32_t> samples_;
    std::vector<std::int32_t> scratch_;
    std::size_t next_ = 0;
    std::size_t pulls_ = 0;
};
