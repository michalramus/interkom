#include "buffer/mix/Mixer.hpp"

#include <algorithm>
#include <limits>

Mixer::Mixer(std::size_t framesPerChunk)
    : Input(framesPerChunk), sum_(framesPerChunk), chunk_(framesPerChunk) {}

bool Mixer::addInput(Input* input) {
    return push({input, true});
}

bool Mixer::removeInput(Input* input) {
    return push({input, false});
}

bool Mixer::push(Command command) {
    const std::size_t w = write_.load(std::memory_order_relaxed);
    const std::size_t next = (w + 1) % commands_.size();
    if (next == read_.load(std::memory_order_acquire)) {
        return false;
    }
    commands_[w] = command;
    write_.store(next, std::memory_order_release);
    return true;
}

void Mixer::apply() {
    std::size_t r = read_.load(std::memory_order_relaxed);
    while (r != write_.load(std::memory_order_acquire)) {
        const Command command = commands_[r];
        Input** end = inputs_.begin() + count_;
        Input** found = std::find(inputs_.begin(), end, command.input);
        if (command.add && found == end && count_ < kMaxInputs) {
            inputs_[count_++] = command.input;
        } else if (!command.add && found != end) {
            *found = inputs_[--count_]; // order does not matter in a sum
        }
        r = (r + 1) % commands_.size();
        read_.store(r, std::memory_order_release);
    }
}

void Mixer::produce(std::int16_t* out) {
    apply();

    std::fill(sum_.begin(), sum_.end(), 0);
    for (std::size_t i = 0; i < count_; ++i) {
        inputs_[i]->pull(tick(), chunk_.data());
        for (std::size_t s = 0; s < sum_.size(); ++s) {
            sum_[s] += chunk_[s];
        }
    }

    constexpr std::int32_t low = std::numeric_limits<std::int16_t>::min();
    constexpr std::int32_t high = std::numeric_limits<std::int16_t>::max();
    for (std::size_t s = 0; s < sum_.size(); ++s) {
        out[s] = static_cast<std::int16_t>(std::clamp(sum_[s], low, high));
    }
}
