// PulseAudio -> RTP: captures the default source and sends it as L16.
//
// The read callback only queues samples; rtp::Sender's own thread does the
// socket write, so the mainloop thread never waits on the network.
//
// Receive it with the matching example, or with:
//   ffplay -protocol_whitelist file,rtp,udp -i examples/rtp/l16.sdp

#include "rtp.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <pulse/pulseaudio.h>
#include <thread>

namespace {

// Where to send. Change this to the other machine's address; loopback lets a
// single box run the send and receive examples against each other.
constexpr const char* kDestination = "127.0.0.1";

std::atomic_bool g_quit{false};

void on_signal(int /*sig*/) {
    g_quit.store(true, std::memory_order_relaxed);
}

// Called by the mainloop thread whenever captured audio is available.
void on_stream_read(pa_stream* stream, std::size_t /*nbytes*/, void* userdata) {
    auto* sender = static_cast<rtp::Sender*>(userdata);

    // Drain every queued fragment; peek can also report a hole (data == nullptr).
    while (pa_stream_readable_size(stream) > 0) {
        const void* data = nullptr;
        std::size_t size = 0;
        if (pa_stream_peek(stream, &data, &size) < 0) {
            std::cerr << "pa_stream_peek failed\n";
            return;
        }
        if (size == 0) {
            return; // nothing buffered
        }
        if (data != nullptr) {
            sender->send(static_cast<const std::int16_t*>(data), size / sizeof(std::int16_t));
        }
        // A hole is dropped rather than padded: the receiver's own sequence
        // gap handling restores the timeline from the packets it does get.
        pa_stream_drop(stream);
    }
}

void on_stream_state(pa_stream* stream, void* userdata) {
    auto* mainloop = static_cast<pa_threaded_mainloop*>(userdata);
    switch (pa_stream_get_state(stream)) {
    case PA_STREAM_READY:
    case PA_STREAM_FAILED:
    case PA_STREAM_TERMINATED:
        pa_threaded_mainloop_signal(mainloop, 0);
        break;
    default:
        break;
    }
}

void on_context_state(pa_context* context, void* userdata) {
    auto* mainloop = static_cast<pa_threaded_mainloop*>(userdata);
    switch (pa_context_get_state(context)) {
    case PA_CONTEXT_READY:
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
        pa_threaded_mainloop_signal(mainloop, 0);
        break;
    default:
        break;
    }
}

} // namespace

int main() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    rtp::Sender sender{kDestination};
    if (!sender.ok()) {
        std::cerr << "rtp sender: " << sender.error() << "\n";
        return EXIT_FAILURE;
    }

    pa_threaded_mainloop* mainloop = pa_threaded_mainloop_new();
    pa_context* context = pa_context_new(pa_threaded_mainloop_get_api(mainloop), "interkom-rtp-tx");
    pa_context_set_state_callback(context, on_context_state, mainloop);

    pa_threaded_mainloop_lock(mainloop);
    if (pa_threaded_mainloop_start(mainloop) < 0) {
        std::cerr << "cannot start mainloop\n";
        return EXIT_FAILURE;
    }
    if (pa_context_connect(context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
        std::cerr << "pa_context_connect: " << pa_strerror(pa_context_errno(context)) << "\n";
        pa_threaded_mainloop_unlock(mainloop);
        return EXIT_FAILURE;
    }

    // Block until the server either accepts us or refuses.
    while (true) {
        const pa_context_state_t state = pa_context_get_state(context);
        if (state == PA_CONTEXT_READY) {
            break;
        }
        if (!PA_CONTEXT_IS_GOOD(state)) {
            std::cerr << "context failed: " << pa_strerror(pa_context_errno(context)) << "\n";
            pa_threaded_mainloop_unlock(mainloop);
            return EXIT_FAILURE;
        }
        pa_threaded_mainloop_wait(mainloop);
    }

    const pa_sample_spec spec{PA_SAMPLE_S16LE, rtp::kSampleRate,
                              static_cast<std::uint8_t>(rtp::kChannels)};
    pa_stream* stream = pa_stream_new(context, "rtp-send", &spec, nullptr);
    pa_stream_set_state_callback(stream, on_stream_state, mainloop);
    pa_stream_set_read_callback(stream, on_stream_read, &sender);

    // One packet per fragment, and a maxlength that leaves the server nowhere
    // to hide extra audio -- see LOW_LATENCY.md for why the unset fields matter.
    const pa_buffer_attr attr{
        .maxlength = static_cast<std::uint32_t>(rtp::kPayloadBytes * 2),
        .tlength = static_cast<std::uint32_t>(-1),
        .prebuf = static_cast<std::uint32_t>(-1),
        .minreq = static_cast<std::uint32_t>(-1),
        .fragsize = static_cast<std::uint32_t>(rtp::kPayloadBytes),
    };
    const auto flags =
        static_cast<pa_stream_flags_t>(PA_STREAM_ADJUST_LATENCY | PA_STREAM_AUTO_TIMING_UPDATE);
    if (pa_stream_connect_record(stream, nullptr, &attr, flags) < 0) {
        std::cerr << "pa_stream_connect_record: " << pa_strerror(pa_context_errno(context)) << "\n";
        pa_threaded_mainloop_unlock(mainloop);
        return EXIT_FAILURE;
    }

    while (true) {
        const pa_stream_state_t state = pa_stream_get_state(stream);
        if (state == PA_STREAM_READY) {
            break;
        }
        if (!PA_STREAM_IS_GOOD(state)) {
            std::cerr << "stream failed: " << pa_strerror(pa_context_errno(context)) << "\n";
            pa_threaded_mainloop_unlock(mainloop);
            return EXIT_FAILURE;
        }
        pa_threaded_mainloop_wait(mainloop);
    }
    pa_threaded_mainloop_unlock(mainloop);

    sender.start();
    std::cout << "sending L16/" << rtp::kSampleRate << "/" << rtp::kChannels << " to "
              << kDestination << ":" << rtp::kPort << ", " << rtp::kPtimeMs
              << " ms packets (ctrl-c to stop)\n";
    while (!g_quit.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cout << "sent " << sender.packets_sent() << " packets, " << sender.send_errors()
                  << " errors, " << sender.frames_dropped() << " frames dropped\n";
    }

    pa_threaded_mainloop_stop(mainloop);
    pa_stream_unref(stream);
    pa_context_disconnect(context);
    pa_context_unref(context);
    pa_threaded_mainloop_free(mainloop);
    sender.stop();
    return EXIT_SUCCESS;
}
