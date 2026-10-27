// Beltpack: mic to the server, the server's mix to the speaker.
//
//   Mic --push--> RTPSender ------------------------------> server
//   server --> RTPReceiver --add--> JitterBuffer --pull--> Speaker
//
// The speaker's device callback is the clock: each chunk it needs ticks a
// DeviceClock and pulls one chunk out of the jitter buffer, so playout runs on
// the device's own clock. Everything configurable is a constexpr below.

#include "audio/Mic.hpp"
#include "audio/Speaker.hpp"
#include "buffer/jitter/JitterBuffer.hpp"
#include "clock/DeviceClock.hpp"
#include "rtp/RTPReceiver.hpp"
#include "rtp/RTPSender.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {

constexpr char kServerAddress[] = "127.0.0.1";
constexpr std::uint16_t kSendPort = 5004;    // the server's receive port for us
constexpr std::uint16_t kReceivePort = 5006; // where the server sends our mix
constexpr std::uint32_t kSampleRate = 48000;
constexpr std::size_t kFramesPerChunk = 480; // 10 ms
constexpr std::size_t kJitterChunks = 2;     // starting depth, 20 ms

std::atomic_bool g_quit{false};

void on_signal(int /*sig*/) {
    g_quit.store(true, std::memory_order_relaxed);
}

} // namespace

int main() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const AudioConfig audio{
        .sampleRate = kSampleRate, .channels = 1, .framesPerChunk = kFramesPerChunk};
    JitterConfig jitterConfig;
    jitterConfig.framesPerChunk = kFramesPerChunk;
    jitterConfig.depthChunks = kJitterChunks;

    JitterBuffer<> jitter{jitterConfig};
    RTPReceiver receiver{{.address = "0.0.0.0", .port = kReceivePort, .sampleRate = kSampleRate},
                         [&](std::uint32_t timestamp, std::uint16_t /*sequence*/,
                             const std::int16_t* samples,
                             std::size_t frames) { jitter.add(timestamp, samples, frames); }};

    RTPSender sender{{.address = kServerAddress,
                      .port = kSendPort,
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

    std::cout << "client: sending to " << kServerAddress << ":" << kSendPort << ", receiving on "
              << kReceivePort << " (ctrl-c to stop)\n";
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
