#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Anything that hands out one chunk of mono L16 per tick: a jitter buffer, a
// message player, a mixer of other inputs.
//
// pull() caches the chunk per tick, so an input shared by several mixers (the
// usual intercom case: A's jitter buffer is in B's, C's and D's mix) is
// consumed once per tick and every mixer gets the same audio. All pulls of one
// tick must come from the same thread; pull() never waits.
class Input {
  public:
    explicit Input(std::size_t framesPerChunk);
    virtual ~Input() = default;
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;
    Input(Input&&) = delete;
    Input& operator=(Input&&) = delete;

    // Writes exactly frames() samples to out.
    void pull(std::uint64_t tick, std::int16_t* out);

    [[nodiscard]] std::size_t frames() const {
        return cache_.size();
    }

  protected:
    // Produces the next chunk; called at most once per tick.
    virtual void produce(std::int16_t* out) = 0;

    // The tick being produced, for inputs that pull other inputs.
    [[nodiscard]] std::uint64_t tick() const {
        return tick_;
    }

  private:
    std::vector<std::int16_t> cache_;
    std::uint64_t tick_ = 0;
    bool pulled_ = false;
};
