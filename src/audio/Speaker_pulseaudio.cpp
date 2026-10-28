// PulseAudio threaded-mainloop backend for Speaker. Based on
// examples/pulseaudio/playback.cpp.

#include "audio/PulseStream.hpp"
#include "audio/Speaker.hpp"

#include <algorithm>
#include <iostream>
#include <pulse/pulseaudio.h>

struct Speaker::Impl {
    AudioConfig config;
    Source source;
    pa_threaded_mainloop* mainloop = nullptr;
    pa_context* context = nullptr;
    pa_stream* stream = nullptr;
    const std::int16_t* chunk = nullptr; // last chunk from source
    std::size_t offset = 0;              // samples of it already played
};

namespace {

// Called by the mainloop thread when the sink can take more audio. Chunks go
// straight into Pulse's buffer.
void on_stream_write(pa_stream* stream, std::size_t nbytes, void* userdata) {
    auto* impl = static_cast<Speaker::Impl*>(userdata);
    const std::size_t chunkFrames = impl->config.framesPerChunk;

    void* buffer = nullptr;
    std::size_t size = nbytes;
    if (pa_stream_begin_write(stream, &buffer, &size) < 0 || buffer == nullptr) {
        std::cerr << "pa_stream_begin_write failed\n";
        return;
    }

    auto* out = static_cast<std::int16_t*>(buffer);
    const std::size_t frames = size / sizeof(std::int16_t);
    std::size_t written = 0;
    while (written < frames) {
        if (impl->chunk == nullptr || impl->offset == chunkFrames) {
            impl->chunk = impl->source();
            impl->offset = 0;
        }
        const std::size_t count = std::min(frames - written, chunkFrames - impl->offset);
        std::copy_n(impl->chunk + impl->offset, count, out + written);
        written += count;
        impl->offset += count;
    }

    if (pa_stream_write(stream, buffer, size, nullptr, 0, PA_SEEK_RELATIVE) < 0) {
        std::cerr << "pa_stream_write failed\n";
        pa_stream_cancel_write(stream);
    }
}

void on_stream_state(pa_stream* /*stream*/, void* userdata) {
    pa_threaded_mainloop_signal(static_cast<pa_threaded_mainloop*>(userdata), 0);
}

void on_context_state(pa_context* /*context*/, void* userdata) {
    pa_threaded_mainloop_signal(static_cast<pa_threaded_mainloop*>(userdata), 0);
}

} // namespace

Speaker::Speaker(AudioConfig config, Source source) : impl_(std::make_unique<Impl>()) {
    impl_->config = config;
    impl_->source = std::move(source);
}

Speaker::~Speaker() {
    stop();
}

bool Speaker::start() {
    impl_->mainloop = pa_threaded_mainloop_new();
    impl_->context =
        pa_context_new(pa_threaded_mainloop_get_api(impl_->mainloop), "interkom-speaker");
    pa_context_set_state_callback(impl_->context, on_context_state, impl_->mainloop);

    pa_threaded_mainloop_lock(impl_->mainloop);
    if (pa_threaded_mainloop_start(impl_->mainloop) < 0 ||
        pa_context_connect(impl_->context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
        std::cerr << "pa_context_connect: " << pa_strerror(pa_context_errno(impl_->context))
                  << "\n";
        pa_threaded_mainloop_unlock(impl_->mainloop);
        return false;
    }

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

    // Mono: the source hands out mono chunks.
    const pa_sample_spec spec{PA_SAMPLE_S16LE, impl_->config.sampleRate, 1};
    impl_->stream = new_chunk_stream(impl_->context, "speaker", spec, impl_->config.framesPerChunk);
    pa_stream_set_state_callback(impl_->stream, on_stream_state, impl_->mainloop);
    pa_stream_set_write_callback(impl_->stream, on_stream_write, impl_.get());

    // Two chunks queued in the server, refilled a chunk at a time.
    const auto chunk =
        static_cast<std::uint32_t>(impl_->config.framesPerChunk * pa_frame_size(&spec));
    const pa_buffer_attr attr{
        .maxlength = static_cast<std::uint32_t>(-1),
        .tlength = 2 * chunk,
        .prebuf = static_cast<std::uint32_t>(-1),
        .minreq = chunk,
        .fragsize = static_cast<std::uint32_t>(-1),
    };
    const auto flags =
        static_cast<pa_stream_flags_t>(PA_STREAM_ADJUST_LATENCY | PA_STREAM_AUTO_TIMING_UPDATE);
    if (pa_stream_connect_playback(impl_->stream, nullptr, &attr, flags, nullptr, nullptr) < 0) {
        std::cerr << "pa_stream_connect_playback: " << pa_strerror(pa_context_errno(impl_->context))
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
    print_buffer_attr(impl_->stream, "speaker", spec);
    pa_threaded_mainloop_unlock(impl_->mainloop);
    return true;
}

void Speaker::stop() {
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
    impl_->chunk = nullptr;
}
