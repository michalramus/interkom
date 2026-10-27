#pragma once

#include "clock/Clock.hpp"

#include <atomic>
#include <cstdint>
#include <functional>

// Ticks when an audio device callback calls tick(), so the device clock paces
// everything. onTick runs on that callback's real-time thread: it must not
// block or allocate.
class DeviceClock final : public Clock {
  public:
    explicit DeviceClock(std::function<void(std::uint64_t)> onTick);

    void start() override;
    void stop() override;

    // Device callback thread, once per chunk.
    void tick();

  private:
    std::function<void(std::uint64_t)> onTick_;
    std::atomic_bool running_{false};
    std::uint64_t tick_ = 0; // device callback thread only
};
