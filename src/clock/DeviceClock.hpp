#pragma once

#include "clock/Clock.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <utility>

// Ticks when an audio device callback calls tick(), so the device clock paces
// everything. onTick runs on that callback's real-time thread: it must not
// block or allocate.
class DeviceClock final : public Clock {
  public:
    explicit DeviceClock(std::function<void(std::uint64_t)> onTick) : onTick_(std::move(onTick)) {}

    void start() override { running_.store(true, std::memory_order_relaxed); }
    void stop() override { running_.store(false, std::memory_order_relaxed); }

    // Device callback thread, once per chunk.
    void tick() {
        if (running_.load(std::memory_order_relaxed)) {
            onTick_(tick_++);
        }
    }

  private:
    std::function<void(std::uint64_t)> onTick_;
    std::atomic_bool running_{false};
    std::uint64_t tick_ = 0; // device callback thread only
};
