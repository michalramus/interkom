#pragma once

// A source of ticks, once per chunk. Each tick calls the onTick given to the
// concrete clock with a counter that rises by one per tick: the tick to pass to
// Input::pull(), so an input shared by several mixers is consumed once. onTick
// is meant to wake other threads (mixer, senders) or pull one chunk, not to do
// heavy work.
//
// Two sources, swappable behind this interface:
//   TimerClock   its own thread on steady_clock; for a mixer with no device
//   DeviceClock  ticked by an audio device callback; for a beltpack, so the
//                speaker's own clock drives playout and there is no second
//                clock to drift against
class Clock {
  public:
    Clock() = default;
    virtual ~Clock() = default;
    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;
    Clock(Clock&&) = delete;
    Clock& operator=(Clock&&) = delete;

    virtual void start() = 0;
    virtual void stop() = 0;
};
