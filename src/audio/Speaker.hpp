#pragma once

#include "audio/AudioConfig.hpp"

#include <cstdint>
#include <functional>
#include <memory>

// Plays mono L16 on the default output device. The backend (CoreAudio or
// PulseAudio) is picked at compile time; each lives in its own
// Speaker_<backend>.cpp.
//
// The device clocks the speaker: its thread calls source once per
// framesPerChunk samples it needs, and the speaker converts that chunk straight
// into the device's buffer, which is the only copy. If the device asks for a
// different amount, the rest of the last chunk is played first.
class Speaker {
  public:
    // Runs on the audio thread. Returns framesPerChunk mono samples, valid until
    // the next call (an Input::pull() fits). Must not block.
    using Source = std::function<const std::int16_t*()>;

    Speaker(AudioConfig config, Source source);
    ~Speaker();
    Speaker(const Speaker&) = delete;
    Speaker& operator=(const Speaker&) = delete;

    bool start();
    void stop();

    struct Impl; // backend state; public so backend callbacks can reach it

  private:
    std::unique_ptr<Impl> impl_;
};
