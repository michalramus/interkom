// Mixer: every client hears everyone but themselves.
//
//   client i --> RTPReceiver --add--> JitterBuffer i --+
//                                                      +--> Mixer (all)
//   Subtractor i = Mixer - JitterBuffer i --push--> RTPSender --> client i
//
// There is no audio device, so a TimerClock paces the mix. Its thread only
// wakes the mixing thread, which pulls every client's mix-minus once per tick.
// Everything configurable is a constexpr below.

#include "buffer/jitter/JitterBuffer.hpp"
#include "buffer/mix/Mixer.hpp"
#include "buffer/mix/Subtractor.hpp"
#include "clock/TimerClock.hpp"
#include "rtp/RTPReceiver.hpp"
#include "rtp/RTPSender.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <semaphore>
#include <thread>
#include <vector>

namespace {

struct ClientConfig {
    const char* address;       // where the client listens
    std::uint16_t receivePort; // server receives this client on it
    std::uint16_t clientPort;  // server sends this client's mix to it
};

constexpr std::array kClients{
    ClientConfig{"127.0.0.1", 5004, 5006},
    ClientConfig{"127.0.0.1", 5008, 5010},
};
constexpr std::uint32_t kSampleRate = 48000;
constexpr std::size_t kFramesPerChunk = 480; // 10 ms
constexpr std::size_t kJitterChunks = 2;     // starting depth, 20 ms

std::atomic_bool g_quit{false};

void on_signal(int /*sig*/) {
    g_quit.store(true, std::memory_order_relaxed);
}

struct Client {
    Client(const ClientConfig& config, const JitterConfig& jitterConfig, Mixer& mixer)
        : jitter(jitterConfig),
          receiver({.address = "0.0.0.0", .port = config.receivePort, .sampleRate = kSampleRate},
                   [this](std::uint32_t timestamp, std::uint16_t /*sequence*/,
                          const std::int16_t* samples,
                          std::size_t frames) { jitter.add(timestamp, samples, frames); }),
          sender({.address = config.address,
                  .port = config.clientPort,
                  .sampleRate = kSampleRate,
                  .framesPerChunk = kFramesPerChunk}),
          minus(&mixer, &jitter) {}

    JitterBuffer<> jitter;
    RTPReceiver receiver;
    RTPSender sender;
    Subtractor minus;
};

} // namespace

int main() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    JitterConfig jitterConfig;
    jitterConfig.framesPerChunk = kFramesPerChunk;
    jitterConfig.depthChunks = kJitterChunks;

    Mixer mixer{kFramesPerChunk};
    std::vector<std::unique_ptr<Client>> clients;
    for (const ClientConfig& config : kClients) {
        clients.push_back(std::make_unique<Client>(config, jitterConfig, mixer));
        mixer.addInput(&clients.back()->jitter);
    }

    // The timer thread only hands the tick over; mixing runs on its own thread.
    std::atomic<std::uint64_t> tick{0};
    std::binary_semaphore wake{0};
    std::atomic_bool stopping{false};
    std::thread mixing([&] {
        while (true) {
            wake.acquire();
            if (stopping.load(std::memory_order_relaxed)) {
                return;
            }
            const std::uint64_t now = tick.load(std::memory_order_acquire);
            for (auto& client : clients) {
                client->sender.push(client->minus.pull(now), kFramesPerChunk);
            }
        }
    });
    TimerClock clock{std::chrono::microseconds(kFramesPerChunk * 1'000'000 / kSampleRate),
                     [&](std::uint64_t now) {
                         tick.store(now, std::memory_order_release);
                         wake.release();
                     }};

    for (auto& client : clients) {
        if (!client->receiver.start() || !client->sender.start()) {
            return EXIT_FAILURE;
        }
    }
    clock.start();

    std::cout << "server: " << kClients.size() << " clients (ctrl-c to stop)\n";
    while (!g_quit.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        for (std::size_t i = 0; i < clients.size(); ++i) {
            const auto stats = clients[i]->jitter.stats();
            std::cout << "client " << i << ": rx " << stats.received << ", late " << stats.late
                      << ", concealed " << stats.concealed << ", dropped " << stats.dropped
                      << ", depth " << stats.depth << "\n";
        }
    }

    clock.stop();
    stopping.store(true, std::memory_order_relaxed);
    wake.release();
    mixing.join();
    for (auto& client : clients) {
        client->receiver.stop();
        client->sender.stop();
    }
    return EXIT_SUCCESS;
}
