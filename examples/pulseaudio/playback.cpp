// PulseAudio async playback: plays a 440 Hz sine, mono 48 kHz S16LE.
//
// Audio is produced inside the write callback, which is how a real intercom
// would feed the sink from a jitter buffer.

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <pulse/pulseaudio.h>
#include <thread>

namespace {

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint8_t kChannels = 1;
constexpr double kToneHz = 440.0;
constexpr double kAmplitude = 0.25;

std::atomic_bool g_quit{false};

void on_signal(int /*sig*/) {
    g_quit.store(true, std::memory_order_relaxed);
}

struct ToneGenerator {
    double phase = 0.0;

    void fill(std::int16_t* out, std::size_t frames) {
        constexpr double step = 2.0 * std::numbers::pi * kToneHz / kSampleRate;
        for (std::size_t i = 0; i < frames; ++i) {
            out[i] = static_cast<std::int16_t>(std::sin(phase) * kAmplitude * 32767.0);
            phase += step;
            if (phase >= 2.0 * std::numbers::pi) {
                phase -= 2.0 * std::numbers::pi;
            }
        }
    }
};

// Called by the mainloop thread when the sink can take more audio.
void on_stream_write(pa_stream* stream, std::size_t nbytes, void* userdata) {
    auto* tone = static_cast<ToneGenerator*>(userdata);

    // Write straight into Pulse's buffer to avoid a copy.
    void* buffer = nullptr;
    std::size_t size = nbytes;
    if (pa_stream_begin_write(stream, &buffer, &size) < 0 || buffer == nullptr) {
        std::cerr << "pa_stream_begin_write failed\n";
        return;
    }

    tone->fill(static_cast<std::int16_t*>(buffer), size / sizeof(std::int16_t));

    if (pa_stream_write(stream, buffer, size, nullptr, 0, PA_SEEK_RELATIVE) < 0) {
        std::cerr << "pa_stream_write failed\n";
        pa_stream_cancel_write(stream);
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

    ToneGenerator tone;

    pa_threaded_mainloop* mainloop = pa_threaded_mainloop_new();
    pa_context* context =
        pa_context_new(pa_threaded_mainloop_get_api(mainloop), "interkom-playback");
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

    const pa_sample_spec spec{PA_SAMPLE_S16LE, kSampleRate, kChannels};
    pa_stream* stream = pa_stream_new(context, "tone", &spec, nullptr);
    pa_stream_set_state_callback(stream, on_stream_state, mainloop);
    pa_stream_set_write_callback(stream, on_stream_write, &tone);

    // Ask for ~20 ms of buffering; the server shrinks it further if it can.
    const auto target = static_cast<std::uint32_t>(pa_usec_to_bytes(20 * PA_USEC_PER_MSEC, &spec));
    const pa_buffer_attr attr{
        .maxlength = static_cast<std::uint32_t>(-1),
        .tlength = target,
        .prebuf = static_cast<std::uint32_t>(-1),
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

    std::cout << "playing " << kToneHz << " Hz (ctrl-c to stop)\n";
    while (!g_quit.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    pa_threaded_mainloop_stop(mainloop);
    pa_stream_unref(stream);
    pa_context_disconnect(context);
    pa_context_unref(context);
    pa_threaded_mainloop_free(mainloop);
    return EXIT_SUCCESS;
}
