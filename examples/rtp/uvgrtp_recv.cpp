// Minimal uvgRTP receiver: the least code that takes an L16 audio frame off
// the wire. No audio device, no jitter buffer -- it just reports what arrived.
//
// Feed it with uvgrtp_send.cpp.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <thread>
#include <uvgrtp/lib.hh>

namespace {

constexpr char kLocalAddress[] = "0.0.0.0"; // RCE_RECEIVE_ONLY: where to bind
constexpr std::uint16_t kPort = 5004;
constexpr std::uint32_t kSampleRate = 48000;

std::atomic_bool g_quit{false};
std::atomic<std::uint64_t> g_packets{0};

void on_signal(int /*sig*/) {
    g_quit.store(true, std::memory_order_relaxed);
}

// Called on uvgRTP's own receive thread, so it must not block. A real receiver
// would convert the payload and hand it to a jitter buffer here; uvgRTP has no
// jitter buffer of its own, and neither does any other RTP library.
void on_frame(void* /*arg*/, uvgrtp::frame::rtp_frame* frame) {
    if (g_packets.fetch_add(1, std::memory_order_relaxed) == 0) {
        std::cout << "first frame: pt=" << static_cast<int>(frame->header.payload)
                  << " seq=" << frame->header.seq << " ts=" << frame->header.timestamp
                  << " ssrc=" << frame->header.ssrc << " payload=" << frame->payload_len
                  << " bytes\n";
    }

    // frame->payload / frame->payload_len is the L16 audio, big-endian.
    uvgrtp::frame::dealloc_frame(frame);
}

} // namespace

int main() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    uvgrtp::context context;
    uvgrtp::session* session = context.create_session(kLocalAddress);
    if (session == nullptr) {
        std::cerr << "create_session failed\n";
        return EXIT_FAILURE;
    }

    uvgrtp::media_stream* stream =
        session->create_stream(kPort, RTP_FORMAT_L16_MONO, RCE_RECEIVE_ONLY);
    if (stream == nullptr) {
        std::cerr << "create_stream failed\n";
        return EXIT_FAILURE;
    }

    // L16_MONO is RFC 3551 payload type 11, which fixes the clock at 44.1 kHz.
    stream->configure_ctx(RCC_CLOCK_RATE, kSampleRate);

    if (stream->install_receive_hook(nullptr, on_frame) != RTP_OK) {
        std::cerr << "install_receive_hook failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "listening on " << kLocalAddress << ":" << kPort << " (ctrl-c to stop)\n";
    while (!g_quit.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cout << "received " << g_packets.load(std::memory_order_relaxed) << " packets\n";
    }

    // Destroying the session stops and joins uvgRTP's receive thread; there is
    // no uninstall for the hook.
    context.destroy_session(session);
    return EXIT_SUCCESS;
}
