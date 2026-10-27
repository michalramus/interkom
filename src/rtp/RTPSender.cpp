// Based on examples/rtp/uvgrtp_send.cpp, with the handoff from rtp.hpp.

#include "rtp/RTPSender.hpp"

#include <iostream>
#include <uvgrtp/lib.hh>

RTPSender::RTPSender(RTPConfig config) : config_(std::move(config)) {}

RTPSender::~RTPSender() {
    stop();
}

bool RTPSender::start() {
    context_ = std::make_unique<uvgrtp::context>();
    session_ = context_->create_session(config_.address);
    if (session_ == nullptr) {
        std::cerr << "create_session failed\n";
        return false;
    }
    stream_ = session_->create_stream(config_.port, RTP_FORMAT_L16_MONO, RCE_SEND_ONLY);
    if (stream_ == nullptr) {
        std::cerr << "create_stream failed\n";
        return false;
    }
    // L16_MONO is RFC 3551 payload type 11, which fixes the clock at 44.1 kHz.
    stream_->configure_ctx(RCC_CLOCK_RATE, static_cast<int>(config_.sampleRate));

    quit_.store(false, std::memory_order_relaxed);
    thread_ = std::thread(&RTPSender::run, this);
    return true;
}

void RTPSender::stop() {
    if (thread_.joinable()) {
        quit_.store(true, std::memory_order_relaxed);
        ready_.release(); // wake run() so it sees quit_
        thread_.join();
    }
    if (context_ != nullptr) {
        if (session_ != nullptr) {
            context_->destroy_session(session_);
        }
        context_.reset();
        session_ = nullptr;
        stream_ = nullptr;
    }
}

void RTPSender::push(const std::int16_t* samples, std::size_t frames) {
    const std::size_t w = write_.load(std::memory_order_relaxed);
    const std::size_t next = (w + 1) % kSlots;
    if (next == read_.load(std::memory_order_acquire) || frames > kMaxSamples) {
        return; // ring full or chunk too big: drop it
    }

    // L16 is big-endian on the wire.
    Slot& slot = slots_[w];
    for (std::size_t i = 0; i < frames; ++i) {
        slot.payload[i * 2] = static_cast<std::uint8_t>(samples[i] >> 8);
        slot.payload[i * 2 + 1] = static_cast<std::uint8_t>(samples[i]);
    }
    slot.bytes = frames * 2;

    write_.store(next, std::memory_order_release);
    ready_.release();
}

void RTPSender::run() {
    while (true) {
        ready_.acquire();
        if (quit_.load(std::memory_order_relaxed)) {
            return;
        }
        const std::size_t r = read_.load(std::memory_order_relaxed);
        Slot& slot = slots_[r];
        if (stream_->push_frame(slot.payload.data(), slot.bytes, RTP_NO_FLAGS) != RTP_OK) {
            std::cerr << "push_frame failed\n";
        }
        read_.store((r + 1) % kSlots, std::memory_order_release);
    }
}
