// Based on examples/rtp/uvgrtp_recv.cpp.

#include "rtp/RTPReceiver.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <uvgrtp/lib.hh>

struct RTPReceiver::Hook {
    Callback callback;
    std::array<std::int16_t, 4096> samples{}; // uvgRTP receive thread only
};

namespace {

// Runs on uvgRTP's receive thread. L16 is big-endian on the wire.
void on_frame(void* arg, uvgrtp::frame::rtp_frame* frame) {
    auto* hook = static_cast<RTPReceiver::Hook*>(arg);
    const std::size_t frames = std::min(frame->payload_len / 2, hook->samples.size());
    for (std::size_t i = 0; i < frames; ++i) {
        hook->samples[i] =
            static_cast<std::int16_t>(frame->payload[i * 2] << 8 | frame->payload[i * 2 + 1]);
    }
    hook->callback(frame->header.timestamp, frame->header.seq, hook->samples.data(), frames);
    uvgrtp::frame::dealloc_frame(frame);
}

} // namespace

RTPReceiver::RTPReceiver(RTPConfig config, Callback callback)
    : config_(std::move(config)), hook_(std::make_unique<Hook>()) {
    hook_->callback = std::move(callback);
}

RTPReceiver::~RTPReceiver() {
    stop();
}

bool RTPReceiver::start() {
    context_ = std::make_unique<uvgrtp::context>();
    session_ = context_->create_session(config_.address);
    if (session_ == nullptr) {
        std::cerr << "create_session failed\n";
        return false;
    }
    uvgrtp::media_stream* stream =
        session_->create_stream(config_.port, RTP_FORMAT_L16_MONO, RCE_RECEIVE_ONLY);
    if (stream == nullptr) {
        std::cerr << "create_stream failed\n";
        return false;
    }
    // L16_MONO is RFC 3551 payload type 11, which fixes the clock at 44.1 kHz.
    stream->configure_ctx(RCC_CLOCK_RATE, static_cast<int>(config_.sampleRate));

    if (stream->install_receive_hook(hook_.get(), on_frame) != RTP_OK) {
        std::cerr << "install_receive_hook failed\n";
        return false;
    }
    return true;
}

void RTPReceiver::stop() {
    if (context_ == nullptr) {
        return;
    }
    // Destroying the session stops and joins uvgRTP's receive thread, so the
    // hook cannot fire after this returns.
    if (session_ != nullptr) {
        context_->destroy_session(session_);
    }
    context_.reset();
    session_ = nullptr;
}
