#pragma once

#include <cstddef>
#include <cstdint>

// Anything that hands out one chunk of mono L16 per tick: a jitter buffer, a
// message player, a mixer of other inputs.
//
// pull() returns a pointer to the chunk instead of copying it out. The memory
// belongs to the input and stays valid until the input produces again, which
// is the next tick: read it, do not keep it.
//
// pull() caches that pointer per tick, so an input shared by several mixers
// (the usual intercom case: A's jitter buffer is in B's, C's and D's mix) is
// consumed once per tick and every mixer reads the same audio. All pulls of one
// tick must come from the same thread; pull() never waits.
class Input {
  public:
    explicit Input(std::size_t framesPerChunk) : frames_(framesPerChunk) {}

    virtual ~Input() = default;
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;
    Input(Input&&) = delete;
    Input& operator=(Input&&) = delete;

    // Exactly frames() samples, valid until the next tick.
    const std::int16_t* pull(std::uint64_t tick);

    [[nodiscard]] std::size_t frames() const {
        return frames_;
    }

  protected:
    // Produces the next chunk into memory the input owns; called at most once
    // per tick.
    virtual const std::int16_t* produce() = 0;

    // The tick being produced, for inputs that pull other inputs.
    [[nodiscard]] std::uint64_t tick() const {
        return tick_;
    }

  private:
    std::size_t frames_;
    const std::int16_t* chunk_ = nullptr;
    std::uint64_t tick_ = 0;
};
