// Minimal uvgRTP sender: the least code that puts an L16 audio frame on the
// wire. No audio device, no jitter buffer -- just the library's API.
//
// Receive it with uvgrtp_recv.cpp.

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <numbers>
#include <thread>
#include <uvgrtp/lib.hh>

namespace {

constexpr char kDestination[] = "127.0.0.1";
constexpr std::uint16_t kPort = 5004;
constexpr std::uint32_t kSampleRate = 48000;
constexpr std::size_t kFramesPerPacket = 480;                           // 10 ms
constexpr std::size_t kPayloadBytes = kFramesPerPacket * sizeof(short); // 960
constexpr int kPackets = 300;                                           // 3 seconds

} // namespace

int main() {
    uvgrtp::context context;
    uvgrtp::session* session = context.create_session(kDestination);
    if (session == nullptr) {
        std::cerr << "create_session failed\n";
        return EXIT_FAILURE;
    }

    uvgrtp::media_stream* stream =
        session->create_stream(kPort, RTP_FORMAT_L16_MONO, RCE_SEND_ONLY);
    if (stream == nullptr) {
        std::cerr << "create_stream failed\n";
        return EXIT_FAILURE;
    }

    // L16_MONO is RFC 3551 payload type 11, which fixes the clock at 44.1 kHz.
    // Without this the RTP timestamps are wrong for a 48 kHz stream.
    stream->configure_ctx(RCC_CLOCK_RATE, kSampleRate);

    // A 440 Hz sine, so there is something recognisable to look at on the
    // other end. L16 is big-endian on the wire.
    std::array<std::uint8_t, kPayloadBytes> payload{};
    double phase = 0.0;
    constexpr double step = 2.0 * std::numbers::pi * 440.0 / kSampleRate;

    std::cout << "sending " << kPackets << " L16 packets to " << kDestination << ":" << kPort
              << "\n";
    const auto start = std::chrono::steady_clock::now();
    for (int packet = 0; packet < kPackets; ++packet) {
        for (std::size_t i = 0; i < kFramesPerPacket; ++i) {
            const auto sample = static_cast<std::int16_t>(std::sin(phase) * 0.25 * 32767.0);
            payload[i * 2] = static_cast<std::uint8_t>(sample >> 8);
            payload[i * 2 + 1] = static_cast<std::uint8_t>(sample);
            phase += step;
        }

        if (stream->push_frame(payload.data(), payload.size(), RTP_NO_FLAGS) != RTP_OK) {
            std::cerr << "push_frame failed\n";
            break;
        }

        // A real sender is paced by its audio device; this one just sleeps.
        std::this_thread::sleep_until(start + std::chrono::milliseconds(10 * (packet + 1)));
    }

    context.destroy_session(session);
    return EXIT_SUCCESS;
}
