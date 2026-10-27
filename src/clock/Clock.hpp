#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

// Calls onTick once every period from its own thread. Ticks are scheduled
// against absolute deadlines, so a late wakeup does not shift later ones.
// onTick is meant to wake other threads (jitter buffer, senders, speakers),
// not to do the work itself.
class Clock {
  public:
    Clock(std::chrono::microseconds period, std::function<void()> onTick);
    ~Clock();
    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;

    void start();
    void stop();

  private:
    void run();

    std::chrono::microseconds period_;
    std::function<void()> onTick_;
    std::atomic_bool quit_{false};
    std::thread thread_;
};
