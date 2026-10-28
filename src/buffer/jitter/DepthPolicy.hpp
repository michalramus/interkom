#pragma once

#include "buffer/jitter/JitterTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

// A depth policy decides how much audio the jitter buffer holds. It never
// touches audio itself; the buffer asks it four things:
//
//   record(lateness)        every packet; chunks past its slot, <= 0 is on time
//   grow(chunks, buffered)  a late packet's slot was concealed: rewind to it?
//   hold(buffered)          playing the next chunk would leave this many: stay
//                           on it and play a concealed chunk instead?
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

    bool hold(std::size_t /*buffered*/) {
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
// Holding: the queue never runs below minChunks while there is audio to play.
// When playing the next chunk would leave fewer, the buffer plays a concealed
// chunk and stays where it is, one chunk deeper. A consumer that pulls in
// bursts (a sound server asking for a whole period at once) drains the queue
// by the burst size; holding grows the depth until a burst fits.
//
// Shrinking: every shrinkWindow pulls, whatever the queue kept above minChunks
// all window long (its low-water mark minus minChunks) was never needed, so it
// is dropped at once. Blind for now: it may land mid-word.
// A burst past maxChunks is trimmed straight back to the depth adapted to so
// far: depthChunks, plus every grow and hold, minus every shrink.
//
// Together the two also absorb clock drift: a slow sender shows up as
// lateness, a fast one as a growing early margin.
class AdaptiveDepth {
  public:
    explicit AdaptiveDepth(const JitterConfig& config);

    void record(std::int32_t lateness);
    bool grow(std::size_t chunks, std::size_t buffered);
    bool hold(std::size_t buffered);
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
    std::size_t target_; // depth adapted to so far, within [minChunks, maxChunks]
    std::size_t lowest_ =
        std::numeric_limits<std::size_t>::max(); // fewest chunks queued this shrink window
};
