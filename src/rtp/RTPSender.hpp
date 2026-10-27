#pragma once

#include "rtp/RTPConfig.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <semaphore>
#include <thread>
#include <vector>

namespace uvgrtp {
class context;
class session;
class media_stream;
} // namespace uvgrtp

// Sends L16 audio over RTP from its own thread. push() is called from the mic
// callback: it copies the samples into a ring and wakes the sender thread, so
// the audio thread never touches the socket.
//
// Whatever the device hands to push(), packets carry exactly framesPerChunk
// samples, and their timestamps count samples (not wall time), so a jitter
// buffer can line them up chunk by chunk.
class RTPSender {
  public:
    explicit RTPSender(RTPConfig config);
    ~RTPSender();
    RTPSender(const RTPSender&) = delete;
    RTPSender& operator=(const RTPSender&) = delete;

    bool start();
    void stop();

    // Audio thread. Any number of samples; each full chunk becomes a packet.
    // Never blocks; drops a packet if the ring is full.
    void push(const std::int16_t* samples, std::size_t frames);

  private:
    static constexpr std::size_t kSlots = 8;
    static constexpr std::size_t kMaxSamples = 4096;

    struct Slot {
        std::array<std::uint8_t, kMaxSamples * 2> payload{}; // big-endian L16
        std::uint32_t timestamp = 0;
    };

    void run();

    RTPConfig config_;
    std::unique_ptr<uvgrtp::context> context_;
    uvgrtp::session* session_ = nullptr;
    uvgrtp::media_stream* stream_ = nullptr;

    // Single-producer single-consumer: the audio thread writes, run() reads.
    std::vector<Slot> slots_{kSlots};
    std::atomic<std::size_t> read_{0};
    std::atomic<std::size_t> write_{0};
    std::counting_semaphore<kSlots> ready_{0};
    std::size_t filled_ = 0;      // audio thread: samples in the slot being filled
    std::uint32_t timestamp_ = 0; // audio thread: first sample of that slot
    std::atomic_bool quit_{false};
    std::thread thread_;
};
