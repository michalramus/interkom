// PulseAudio async capture: records mono 48 kHz S16LE to a raw file.
//
// Play back with:
//   ffplay -f s16le -ar 48000 -ch_layout mono capture.raw

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <pulse/pulseaudio.h>
#include <string>
#include <thread>

namespace {

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint8_t kChannels = 1;

std::atomic_bool g_quit{false};

void on_signal(int /*sig*/) {
    g_quit.store(true, std::memory_order_relaxed);
}

struct Recorder {
    std::FILE* file = nullptr;
    std::uint64_t bytes = 0;
};

// Called by the mainloop thread whenever captured audio is available.
void on_stream_read(pa_stream* stream, std::size_t /*nbytes*/, void* userdata) {
    auto* rec = static_cast<Recorder*>(userdata);

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
            std::fwrite(data, 1, size, rec->file);
            rec->bytes += size;
        }
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

int main(int argc, char** argv) {
    const std::string path = (argc > 1) ? argv[1] : "capture.raw";

    Recorder rec;
    rec.file = std::fopen(path.c_str(), "wb");
    if (rec.file == nullptr) {
        std::cerr << "cannot open " << path << " for writing\n";
        return EXIT_FAILURE;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    pa_threaded_mainloop* mainloop = pa_threaded_mainloop_new();
    pa_context* context =
        pa_context_new(pa_threaded_mainloop_get_api(mainloop), "interkom-capture");
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

    const pa_sample_spec spec{PA_SAMPLE_S16LE, kSampleRate, kChannels};
    pa_stream* stream = pa_stream_new(context, "record", &spec, nullptr);
    pa_stream_set_state_callback(stream, on_stream_state, mainloop);
    pa_stream_set_read_callback(stream, on_stream_read, &rec);

    // ~20 ms fragments; ADJUST_LATENCY lets the server pick the rest.
    const pa_buffer_attr attr{
        .maxlength = static_cast<std::uint32_t>(-1),
        .tlength = static_cast<std::uint32_t>(-1),
        .prebuf = static_cast<std::uint32_t>(-1),
        .minreq = static_cast<std::uint32_t>(-1),
        .fragsize = static_cast<std::uint32_t>(pa_usec_to_bytes(20 * PA_USEC_PER_MSEC, &spec)),
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

    std::cout << "recording to " << path << " (ctrl-c to stop)\n";
    while (!g_quit.load(std::memory_order_relaxed)) {
        // The mainloop thread does the real work; this thread only waits for ctrl-c.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    pa_threaded_mainloop_stop(mainloop);
    pa_stream_unref(stream);
    pa_context_disconnect(context);
    pa_context_unref(context);
    pa_threaded_mainloop_free(mainloop);

    std::fclose(rec.file);
    std::cout << "wrote " << rec.bytes << " bytes\n";
    return EXIT_SUCCESS;
}
