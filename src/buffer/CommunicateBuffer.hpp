#pragma once

#include "buffer/Input.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

// Plays prerecorded messages. select() picks one from any thread; produce()
// plays it chunk by chunk and falls back to 0 (silence) when it ends.
// Selecting again, even the same message, restarts from the beginning.
//
// For now the messages are generated tones: message n is a one second sine at
// (n + 1) * 200 Hz.
class CommunicateBuffer final : public Input {
  public:
    static constexpr std::size_t kMessages = 4; // ids 1..kMessages; 0 is silence

    CommunicateBuffer(std::uint32_t sampleRate, std::size_t framesPerChunk);

    // Any thread. An unknown id is silence.
    void select(std::size_t id);

  protected:
    void produce(std::int16_t* out) override;

  private:
    static constexpr std::size_t kNone = ~std::size_t{0}; // nothing selected

    std::vector<std::vector<std::int16_t>> messages_; // [0] is empty: silence
    std::atomic<std::size_t> next_{kNone};

    // Consumer thread only.
    std::size_t current_ = 0;
    std::size_t count_ = 0; // samples of current_ already played
};
