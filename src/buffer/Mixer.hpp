#pragma once

#include "buffer/Input.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

// Sums its inputs into one chunk. Being an Input itself, mixers nest.
//
// Mix-minus is one Mixer per output whose list leaves out the listener's own
// input. Inputs shared between those mixers are pulled with the same tick, so
// Input's cache makes each one produce once per tick.
//
// addInput() and removeInput() run on one control thread and only queue a
// command in a lock-free single-producer single-consumer queue; produce()
// applies them before mixing, so it never locks or allocates. Inputs are not
// owned: keep one alive until a tick has been produced after removing it.
class Mixer final : public Input {
  public:
    static constexpr std::size_t kMaxInputs = 32;

    explicit Mixer(std::size_t framesPerChunk);

    // Control thread. False if the command queue is full; try again later.
    bool addInput(Input* input);
    bool removeInput(Input* input);

  protected:
    void produce(std::int16_t* out) override;

  private:
    struct Command {
        Input* input = nullptr;
        bool add = false;
    };

    bool push(Command command);
    void apply();

    // Single-producer single-consumer: the control thread writes, produce() reads.
    std::array<Command, 64> commands_{};
    std::atomic<std::size_t> read_{0};
    std::atomic<std::size_t> write_{0};

    // Consumer thread only.
    std::array<Input*, kMaxInputs> inputs_{};
    std::size_t count_ = 0;
    std::vector<std::int32_t> sum_;
    std::vector<std::int16_t> chunk_;
};
