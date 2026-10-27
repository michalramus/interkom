#include "buffer/source/CommunicateBuffer.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

CommunicateBuffer::CommunicateBuffer(std::uint32_t sampleRate, std::size_t framesPerChunk)
    : Input(framesPerChunk), messages_(kMessages + 1) {
    for (std::size_t id = 1; id <= kMessages; ++id) {
        const double step =
            2.0 * std::numbers::pi * (200.0 * static_cast<double>(id + 1)) / sampleRate;
        auto& message = messages_[id];
        message.resize(sampleRate); // one second
        for (std::size_t i = 0; i < message.size(); ++i) {
            message[i] =
                static_cast<std::int16_t>(std::sin(step * static_cast<double>(i)) * 0.25 * 32767.0);
        }
    }
}

void CommunicateBuffer::select(std::size_t id) {
    next_.store(id < messages_.size() ? id : 0, std::memory_order_release);
}

void CommunicateBuffer::produce(std::int16_t* out) {
    if (const std::size_t id = next_.exchange(kNone, std::memory_order_acquire); id != kNone) {
        current_ = id;
        count_ = 0;
    }

    const auto& message = messages_[current_];
    const std::size_t count = std::min(frames(), message.size() - count_);
    std::copy_n(message.begin() + static_cast<std::ptrdiff_t>(count_), count, out);
    std::fill(out + count, out + frames(), std::int16_t{0});
    count_ += count;

    if (count_ == message.size()) {
        current_ = 0;
        count_ = 0;
    }
}
