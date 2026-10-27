#include "buffer/Input.hpp"

const std::int16_t* Input::pull(std::uint64_t tick) {
    if (chunk_ == nullptr || tick != tick_) {
        tick_ = tick;
        chunk_ = produce();
    }
    return chunk_;
}
