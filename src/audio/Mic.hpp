#pragma once

#include "audio/AudioConfig.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

// Captures the default input device. The backend (CoreAudio or PulseAudio) is
// picked at compile time; each lives in its own Mic_<backend>.cpp.
class Mic {
  public:
    // Runs on the audio thread, once per chunk. Must not block: copy and return.
    using Callback = std::function<void(const std::int16_t* samples, std::size_t frames)>;

    Mic(AudioConfig config, Callback callback);
    ~Mic();
    Mic(const Mic&) = delete;
    Mic& operator=(const Mic&) = delete;

    bool start();
    void stop();

    struct Impl; // backend state; public so backend callbacks can reach it

  private:
    std::unique_ptr<Impl> impl_;
};
