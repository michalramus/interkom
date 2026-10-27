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
// the callback the moment a packet lands, already decoded to host-order
// samples; the callback feeds a jitter buffer and must not block.
class RTPReceiver {
  public:
    // samples are valid only for the duration of the call.
    using Callback = std::function<void(std::uint32_t timestamp, std::uint16_t sequence,
                                        const std::int16_t* samples, std::size_t frames)>;

    RTPReceiver(RTPConfig config, Callback callback);
    ~RTPReceiver();
    RTPReceiver(const RTPReceiver&) = delete;
    RTPReceiver& operator=(const RTPReceiver&) = delete;

    bool start();
    void stop();

    struct Hook; // decode state; public so uvgRTP's hook can reach it

  private:
    RTPConfig config_;
    std::unique_ptr<Hook> hook_;
    std::unique_ptr<uvgrtp::context> context_;
    uvgrtp::session* session_ = nullptr;
};
