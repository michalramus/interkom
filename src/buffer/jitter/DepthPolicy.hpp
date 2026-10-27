#pragma once

#include "buffer/jitter/JitterTypes.hpp"

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

    bool grow(std::size_t /*chunks*/, std::size_t /*buffered*/) {
        return false;
    }

    std::size_t shrink(std::size_t buffered);

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
    explicit AdaptiveDepth(const JitterConfig& config);

    void record(std::int32_t lateness);
    bool grow(std::size_t chunks, std::size_t buffered);
    std::size_t shrink(std::size_t buffered);

  private:
    // Rounded up, so with 100 samples it takes 5 late ones to go positive.
    std::int32_t percentile();

    // The cursor moved, so every remembered packet would now be this much later.
    void shift(std::int32_t chunks);

    JitterConfig config_;
    std::vector<std::int32_t> samples_;
    std::vector<std::int32_t> scratch_;
    std::size_t next_ = 0;
    std::size_t pulls_ = 0;
};
