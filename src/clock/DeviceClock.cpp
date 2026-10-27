#include "clock/DeviceClock.hpp"

#include <utility>

DeviceClock::DeviceClock(std::function<void(std::uint64_t)> onTick) : onTick_(std::move(onTick)) {}

void DeviceClock::start() {
    running_.store(true, std::memory_order_relaxed);
}

void DeviceClock::stop() {
    running_.store(false, std::memory_order_relaxed);
}

void DeviceClock::tick() {
    if (running_.load(std::memory_order_relaxed)) {
        onTick_(tick_++);
    }
}
