#pragma once

#include "clock/Clock.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>

// Calls onTick once every period from its own thread. Ticks are scheduled
// against absolute deadlines, so a late wakeup does not shift later ones.
class TimerClock final : public Clock {
  public:
    TimerClock(std::chrono::microseconds period, std::function<void(std::uint64_t)> onTick);
    ~TimerClock() override;
    TimerClock(const TimerClock&) = delete;
    TimerClock& operator=(const TimerClock&) = delete;
    TimerClock(TimerClock&&) = delete;
    TimerClock& operator=(TimerClock&&) = delete;

    void start() override;
    void stop() override;

  private:
    void run();

    std::chrono::microseconds period_;
    std::function<void(std::uint64_t)> onTick_;
    std::atomic_bool quit_{false};
    std::uint64_t tick_ = 0; // timer thread only
    std::thread thread_;
};
