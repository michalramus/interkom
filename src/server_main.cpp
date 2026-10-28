// Mixer: every client hears everyone but themselves.
//
//   client i --> RTPReceiver --add--> JitterBuffer i --+
//                                                      +--> Mixer (all)
//   Subtractor i = Mixer - JitterBuffer i --push--> RTPSender --> client i
//
// There is no audio device, so a TimerClock paces the mix. Its thread only
// wakes the mixing thread, which pulls every client's mix-minus once per tick.
// Clients come from the command line, everything else is a constexpr below.
//
//   interkom_server <ip:recv:send>...
//
// recv is the server port receiving that client's audio, send is the client's
// port the server sends its mix to.

#include "buffer/jitter/JitterBuffer.hpp"
#include "buffer/mix/Mixer.hpp"
#include "buffer/mix/Subtractor.hpp"
#include "clock/ThreadPriority.hpp"
#include "clock/TimerClock.hpp"
#include "rtp/RTPReceiver.hpp"
#include "rtp/RTPSender.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <semaphore>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

struct ClientConfig {
    std::string address;       // where the client listens
    std::uint16_t receivePort; // server receives this client on it
    std::uint16_t clientPort;  // server sends this client's mix to it
};
constexpr std::uint32_t kSampleRate = 48000;
constexpr std::size_t kFramesPerChunk = 480; // 10 ms
constexpr std::size_t kJitterChunks = 2;     // starting depth, 20 ms

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

// ip:recv:send. Split on the last two colons, so the address keeps any of its own.
std::optional<ClientConfig> parse_client(std::string_view text) {
    const std::size_t second = text.rfind(':');
    if (second == std::string_view::npos || second == 0) {
        return std::nullopt;
    }
    const std::size_t first = text.rfind(':', second - 1);
    if (first == std::string_view::npos || first == 0) {
        return std::nullopt;
    }
    const auto receivePort = parse_port(text.substr(first + 1, second - first - 1));
    const auto clientPort = parse_port(text.substr(second + 1));
    if (!receivePort || !clientPort) {
        return std::nullopt;
    }
    return ClientConfig{std::string(text.substr(0, first)), *receivePort, *clientPort};
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

int main(int argc, char** argv) {
    std::vector<ClientConfig> configs;
    const auto count = static_cast<std::size_t>(argc - 1);
    bool valid = count >= 1 && count <= Mixer::kMaxInputs;
    for (int i = 1; valid && i < argc; ++i) {
        const auto config = parse_client(argv[i]);
        valid = config.has_value();
        if (valid) {
            configs.push_back(*config);
        }
    }
    if (!valid) {
        std::cerr << "usage: " << argv[0] << " <ip:recv:send>...  (1 to " << Mixer::kMaxInputs
                  << " clients)\n"
                  << "  ip    client address\n"
                  << "  recv  server port receiving this client's audio\n"
                  << "  send  client port the mix is sent to\n";
        return EXIT_FAILURE;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    JitterConfig jitterConfig;
    jitterConfig.framesPerChunk = kFramesPerChunk;
    jitterConfig.depthChunks = kJitterChunks;

    Mixer mixer{kFramesPerChunk};
    std::vector<std::unique_ptr<Client>> clients;
    for (const ClientConfig& config : configs) {
        clients.push_back(std::make_unique<Client>(config, jitterConfig, mixer));
        mixer.addInput(&clients.back()->jitter);
    }

    // The timer thread only hands the tick over; mixing runs on its own thread.
    // A counting semaphore, so a wakeup that lands while the mixer is still
    // busy is not merged away; the mixer then catches up every tick it missed.
    std::atomic<std::uint64_t> tick{0};
    std::counting_semaphore<> wake{0};
    std::atomic_bool stopping{false};
    std::thread mixing([&] {
        raiseThreadPriority();
        std::uint64_t next = 0; // first tick not mixed yet
        while (true) {
            wake.acquire();
            if (stopping.load(std::memory_order_relaxed)) {
                return;
            }
            const std::uint64_t now = tick.load(std::memory_order_acquire);
            for (; next <= now; ++next) {
                for (auto& client : clients) {
                    client->sender.push(client->minus.pull(next), kFramesPerChunk);
                }
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

    std::cout << "server: " << configs.size() << " clients (ctrl-c to stop)\n";
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
