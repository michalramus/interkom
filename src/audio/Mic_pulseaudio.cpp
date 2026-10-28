// PulseAudio threaded-mainloop backend for Mic. Based on
// examples/pulseaudio/capture.cpp.

#include "audio/Mic.hpp"
#include "audio/PulseStream.hpp"

#include <iostream>
#include <pulse/pulseaudio.h>

struct Mic::Impl {
    AudioConfig config;
    Callback callback;
    pa_threaded_mainloop* mainloop = nullptr;
    pa_context* context = nullptr;
    pa_stream* stream = nullptr;
};

namespace {

// Called by the mainloop thread whenever captured audio is available.
void on_stream_read(pa_stream* stream, std::size_t /*nbytes*/, void* userdata) {
    auto* impl = static_cast<Mic::Impl*>(userdata);

    // Drain every queued fragment; peek can also report a hole (data == nullptr).
    while (pa_stream_readable_size(stream) > 0) {
        const void* data = nullptr;
        std::size_t size = 0;
        if (pa_stream_peek(stream, &data, &size) < 0 || size == 0) {
            return;
        }
        if (data != nullptr) {
            impl->callback(static_cast<const std::int16_t*>(data),
                           size / (sizeof(std::int16_t) * impl->config.channels));
        }
        pa_stream_drop(stream);
    }
}

void on_stream_state(pa_stream* /*stream*/, void* userdata) {
    pa_threaded_mainloop_signal(static_cast<pa_threaded_mainloop*>(userdata), 0);
}

void on_context_state(pa_context* /*context*/, void* userdata) {
    pa_threaded_mainloop_signal(static_cast<pa_threaded_mainloop*>(userdata), 0);
}

} // namespace

Mic::Mic(AudioConfig config, Callback callback) : impl_(std::make_unique<Impl>()) {
    impl_->config = config;
    impl_->callback = std::move(callback);
}

Mic::~Mic() {
    stop();
}

bool Mic::start() {
    impl_->mainloop = pa_threaded_mainloop_new();
    impl_->context = pa_context_new(pa_threaded_mainloop_get_api(impl_->mainloop), "interkom-mic");
    pa_context_set_state_callback(impl_->context, on_context_state, impl_->mainloop);

    pa_threaded_mainloop_lock(impl_->mainloop);
    if (pa_threaded_mainloop_start(impl_->mainloop) < 0 ||
        pa_context_connect(impl_->context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
        std::cerr << "pa_context_connect: " << pa_strerror(pa_context_errno(impl_->context))
                  << "\n";
        pa_threaded_mainloop_unlock(impl_->mainloop);
        return false;
    }

    // Block until the server either accepts us or refuses.
    pa_context_state_t context_state = PA_CONTEXT_UNCONNECTED;
    while ((context_state = pa_context_get_state(impl_->context)) != PA_CONTEXT_READY) {
        if (!PA_CONTEXT_IS_GOOD(context_state)) {
            std::cerr << "context failed: " << pa_strerror(pa_context_errno(impl_->context))
                      << "\n";
            pa_threaded_mainloop_unlock(impl_->mainloop);
            return false;
        }
        pa_threaded_mainloop_wait(impl_->mainloop);
    }

    const pa_sample_spec spec{PA_SAMPLE_S16LE, impl_->config.sampleRate, impl_->config.channels};
    impl_->stream = new_chunk_stream(impl_->context, "mic", spec, impl_->config.framesPerChunk);
    pa_stream_set_state_callback(impl_->stream, on_stream_state, impl_->mainloop);
    pa_stream_set_read_callback(impl_->stream, on_stream_read, impl_.get());

    // One chunk per fragment; ADJUST_LATENCY lets the server pick the rest.
    const pa_buffer_attr attr{
        .maxlength = static_cast<std::uint32_t>(-1),
        .tlength = static_cast<std::uint32_t>(-1),
        .prebuf = static_cast<std::uint32_t>(-1),
        .minreq = static_cast<std::uint32_t>(-1),
        .fragsize = static_cast<std::uint32_t>(impl_->config.framesPerChunk * pa_frame_size(&spec)),
    };
    if (pa_stream_connect_record(impl_->stream, nullptr, &attr, PA_STREAM_ADJUST_LATENCY) < 0) {
        std::cerr << "pa_stream_connect_record: " << pa_strerror(pa_context_errno(impl_->context))
                  << "\n";
        pa_threaded_mainloop_unlock(impl_->mainloop);
        return false;
    }

    pa_stream_state_t stream_state = PA_STREAM_UNCONNECTED;
    while ((stream_state = pa_stream_get_state(impl_->stream)) != PA_STREAM_READY) {
        if (!PA_STREAM_IS_GOOD(stream_state)) {
            std::cerr << "stream failed: " << pa_strerror(pa_context_errno(impl_->context)) << "\n";
            pa_threaded_mainloop_unlock(impl_->mainloop);
            return false;
        }
        pa_threaded_mainloop_wait(impl_->mainloop);
    }
    print_buffer_attr(impl_->stream, "mic", spec);
    pa_threaded_mainloop_unlock(impl_->mainloop);
    return true;
}

void Mic::stop() {
    if (impl_->mainloop == nullptr) {
        return;
    }
    pa_threaded_mainloop_stop(impl_->mainloop);
    if (impl_->stream != nullptr) {
        pa_stream_unref(impl_->stream);
    }
    pa_context_disconnect(impl_->context);
    pa_context_unref(impl_->context);
    pa_threaded_mainloop_free(impl_->mainloop);
    impl_->mainloop = nullptr;
    impl_->context = nullptr;
    impl_->stream = nullptr;
}
