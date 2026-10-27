#pragma once

#include "rtp/RTPConfig.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace uvgrtp {
class context;
class session;
} // namespace uvgrtp

// Receives L16 audio over RTP. uvgRTP runs its own receive thread and calls
// the callback the moment a packet lands; the callback is meant to wake the
// jitter buffer thread, so it must not block.
class RTPReceiver {
  public:
    // payload is big-endian L16, valid only for the duration of the call.
    using Callback = std::function<void(std::uint32_t timestamp, std::uint16_t sequence,
                                        const std::uint8_t* payload, std::size_t bytes)>;

    RTPReceiver(RTPConfig config, Callback callback);
    ~RTPReceiver();
    RTPReceiver(const RTPReceiver&) = delete;
    RTPReceiver& operator=(const RTPReceiver&) = delete;

    bool start();
    void stop();

  private:
    RTPConfig config_;
    Callback callback_;
    std::unique_ptr<uvgrtp::context> context_;
    uvgrtp::session* session_ = nullptr;
};
