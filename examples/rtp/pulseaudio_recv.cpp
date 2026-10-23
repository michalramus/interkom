// RTP -> PulseAudio: plays an incoming L16 stream on the default sink.
//
// The write callback only drains rtp::Receiver's jitter buffer; a receive
// thread sits blocked in recv(), so the mainloop thread never waits.
//
// Feed it from the matching example, or with:
//   ffmpeg -re -f lavfi -i "sine=frequency=440:sample_rate=48000" \
//          -ac 1 -c:a pcm_s16be -payload_type 96 -f rtp rtp://127.0.0.1:5004

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

// The entire receive-side latency budget, in packets. 1 is the floor; raise it
// if the underrun counter climbs on your network.
constexpr std::uint32_t kJitterPackets = 2;

std::atomic_bool g_quit{false};

void on_signal(int /*sig*/) {
    g_quit.store(true, std::memory_order_relaxed);
}

// Called by the mainloop thread when the sink can take more audio.
void on_stream_write(pa_stream* stream, std::size_t nbytes, void* userdata) {
    auto* receiver = static_cast<rtp::Receiver*>(userdata);

    // Write straight into Pulse's buffer to avoid a copy.
    void* buffer = nullptr;
    std::size_t size = nbytes;
    if (pa_stream_begin_write(stream, &buffer, &size) < 0 || buffer == nullptr) {
        std::cerr << "pa_stream_begin_write failed\n";
        return;
    }

    receiver->pull(static_cast<std::int16_t*>(buffer), size / sizeof(std::int16_t));

    if (pa_stream_write(stream, buffer, size, nullptr, 0, PA_SEEK_RELATIVE) < 0) {
        std::cerr << "pa_stream_write failed\n";
        pa_stream_cancel_write(stream);
    }
}

void on_underflow(pa_stream* /*stream*/, void* userdata) {
    auto* count = static_cast<std::atomic<std::uint64_t>*>(userdata);
    count->fetch_add(1, std::memory_order_relaxed);
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

    rtp::Receiver receiver{rtp::kPort, kJitterPackets};
    if (!receiver.ok()) {
        std::cerr << "rtp receiver: " << receiver.error() << "\n";
        return EXIT_FAILURE;
    }
    std::atomic<std::uint64_t> sink_underflows{0};

    pa_threaded_mainloop* mainloop = pa_threaded_mainloop_new();
    pa_context* context = pa_context_new(pa_threaded_mainloop_get_api(mainloop), "interkom-rtp-rx");
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
    pa_stream* stream = pa_stream_new(context, "rtp-recv", &spec, nullptr);
    pa_stream_set_state_callback(stream, on_stream_state, mainloop);
    pa_stream_set_write_callback(stream, on_stream_write, &receiver);
    // Underruns are the price of a small buffer; without this you shrink blind.
    pa_stream_set_underflow_callback(stream, on_underflow, &sink_underflows);

    // tlength == maxlength with prebuf == 0 is the combination the server
    // honours exactly -- see LOW_LATENCY.md. Our jitter buffer is the only
    // place audio should be waiting, so give the sink no room of its own.
    const auto target = static_cast<std::uint32_t>(rtp::kPayloadBytes);
    const pa_buffer_attr attr{
        .maxlength = target,
        .tlength = target,
        .prebuf = 0,
        .minreq = static_cast<std::uint32_t>(-1),
        .fragsize = static_cast<std::uint32_t>(-1),
    };
    const auto flags =
        static_cast<pa_stream_flags_t>(PA_STREAM_ADJUST_LATENCY | PA_STREAM_AUTO_TIMING_UPDATE);
    if (pa_stream_connect_playback(stream, nullptr, &attr, flags, nullptr, nullptr) < 0) {
        std::cerr << "pa_stream_connect_playback: " << pa_strerror(pa_context_errno(context))
                  << "\n";
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

    receiver.start();
    std::cout << "listening for L16/" << rtp::kSampleRate << "/" << rtp::kChannels << " on port "
              << rtp::kPort << ", jitter buffer " << kJitterPackets << " packets ("
              << (kJitterPackets * rtp::kPtimeMs) << " ms) (ctrl-c to stop)\n";
    while (!g_quit.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cout << "rx " << receiver.packets_received() << ", lost " << receiver.packets_lost()
                  << ", late " << receiver.packets_late() << ", dropped "
                  << receiver.packets_dropped() << ", underruns " << receiver.underruns()
                  << ", overruns " << receiver.overruns() << ", sink underflows "
                  << sink_underflows.load(std::memory_order_relaxed) << "\n";
    }

    pa_threaded_mainloop_stop(mainloop);
    pa_stream_unref(stream);
    pa_context_disconnect(context);
    pa_context_unref(context);
    pa_threaded_mainloop_free(mainloop);
    receiver.stop();
    return EXIT_SUCCESS;
}
