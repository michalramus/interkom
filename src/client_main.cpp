// Beltpack: mic to the server, the server's mix to the speaker.
//
//   Mic --push--> RTPSender ------------------------------> server
//   server --> RTPReceiver --add--> JitterBuffer --pull--> Speaker
//
// The speaker's device callback is the clock: each chunk it needs ticks a
// DeviceClock and pulls one chunk out of the jitter buffer, so playout runs on
// the device's own clock. Addresses come from the command line, everything
// else is a constexpr below.
//
//   interkom_client <server-ip> <receive-port> <send-port>

#include "audio/Mic.hpp"
#include "audio/Speaker.hpp"
#include "buffer/jitter/JitterBuffer.hpp"
#include "clock/DeviceClock.hpp"
#include "rtp/RTPReceiver.hpp"
#include "rtp/RTPSender.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace {

constexpr std::uint32_t kSampleRate = 16000;
constexpr std::size_t kFramesPerChunk = 40; // 2.5 ms

// Jitter buffer, in chunks: start at 10 ms, adapt within 2.5..100 ms.
constexpr JitterConfig kJitter{
    .framesPerChunk = kFramesPerChunk,
    .depthChunks = 4,
    .minChunks = 1,
    .maxChunks = 40,
    .inboxChunks = 256,
    .storeChunks = 256,
    .latenessWindow = 2000, // 5 s
    .shrinkWindow = 2000,   // 5 s
    .resyncChunks = 200,    // 500 ms
    .fadeChunks = 12,       // 30 ms
};

std::atomic_bool g_quit{false};

void on_signal(int /*sig*/) {
    g_quit.store(true, std::memory_order_relaxed);
}

// 1..65535, the whole string, or nothing.
std::optional<std::uint16_t> parse_port(std::string_view text) {
    unsigned value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value == 0 || value > 65535) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
}

} // namespace

int main(int argc, char** argv) {
    const auto receivePort = argc == 4 ? parse_port(argv[2]) : std::nullopt;
    const auto sendPort = argc == 4 ? parse_port(argv[3]) : std::nullopt;
    if (!receivePort || !sendPort) {
        std::cerr << "usage: " << argv[0] << " <server-ip> <receive-port> <send-port>\n"
                  << "  receive-port  local port the server sends our mix to\n"
                  << "  send-port     server port our mic is sent to\n";
        return EXIT_FAILURE;
    }
    const std::string serverAddress = argv[1];

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const AudioConfig audio{
        .sampleRate = kSampleRate, .channels = 1, .framesPerChunk = kFramesPerChunk};

    JitterBuffer<> jitter{kJitter};
    RTPReceiver receiver{{.address = "0.0.0.0", .port = *receivePort, .sampleRate = kSampleRate},
                         [&](std::uint32_t timestamp, std::uint16_t /*sequence*/,
                             const std::int16_t* samples,
                             std::size_t frames) { jitter.add(timestamp, samples, frames); }};

    RTPSender sender{{.address = serverAddress,
                      .port = *sendPort,
                      .sampleRate = kSampleRate,
                      .framesPerChunk = kFramesPerChunk}};
    Mic mic{audio,
            [&](const std::int16_t* samples, std::size_t frames) { sender.push(samples, frames); }};

    // Speaker callback thread only.
    std::uint64_t tick = 0;
    DeviceClock clock{[&](std::uint64_t now) { tick = now; }};
    Speaker speaker{audio, [&] {
                        clock.tick();
                        return jitter.pull(tick);
                    }};

    clock.start();
    if (!receiver.start() || !sender.start() || !speaker.start() || !mic.start()) {
        return EXIT_FAILURE;
    }

    std::cout << "client: sending to " << serverAddress << ":" << *sendPort << ", receiving on "
              << *receivePort << " (ctrl-c to stop)\n";
    while (!g_quit.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const auto stats = jitter.stats();
        std::cout << "rx " << stats.received << ", late " << stats.late << ", concealed "
                  << stats.concealed << ", dropped " << stats.dropped << ", depth " << stats.depth
                  << "\n";
    }

    // Stop the producers before what they feed.
    mic.stop();
    speaker.stop();
    clock.stop();
    sender.stop();
    receiver.stop();
    return EXIT_SUCCESS;
}
