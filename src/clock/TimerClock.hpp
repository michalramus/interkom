#pragma once

#include "clock/Clock.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

// Calls onTick once every period from its own thread. Ticks are scheduled
// against absolute deadlines, so a late wakeup does not shift later ones.
class TimerClock final : public Clock {
  public:
    TimerClock(std::chrono::microseconds period, std::function<void()> onTick);
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
    std::function<void()> onTick_;
    std::atomic_bool quit_{false};
    std::thread thread_;
};
