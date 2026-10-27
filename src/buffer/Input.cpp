#include "buffer/Input.hpp"

#include <algorithm>

Input::Input(std::size_t framesPerChunk) : cache_(framesPerChunk) {}

void Input::pull(std::uint64_t tick, std::int16_t* out) {
    if (!pulled_ || tick != tick_) {
        tick_ = tick;
        pulled_ = true;
        produce(cache_.data());
    }
    std::copy(cache_.begin(), cache_.end(), out);
}
