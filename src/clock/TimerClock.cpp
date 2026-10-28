#include "clock/TimerClock.hpp"

#include "clock/ThreadPriority.hpp"

TimerClock::TimerClock(std::chrono::microseconds period, std::function<void(std::uint64_t)> onTick)
    : period_(period), onTick_(std::move(onTick)) {}

TimerClock::~TimerClock() {
    stop();
}

void TimerClock::start() {
    quit_.store(false, std::memory_order_relaxed);
    thread_ = std::thread(&TimerClock::run, this);
}

void TimerClock::stop() {
    if (thread_.joinable()) {
        quit_.store(true, std::memory_order_relaxed);
        thread_.join();
    }
}

void TimerClock::run() {
    raiseThreadPriority();
    auto deadline = std::chrono::steady_clock::now();
    while (!quit_.load(std::memory_order_relaxed)) {
        deadline += period_;
        std::this_thread::sleep_until(deadline);
        onTick_(tick_++);
    }
}
