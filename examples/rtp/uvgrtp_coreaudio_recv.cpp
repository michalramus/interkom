// uvgRTP + a jitter buffer + a real audio device: the combination an intercom
// actually needs, in one file.
//
// uvgRTP hands each frame to on_frame() the moment it arrives, on its own
// thread. CoreAudio demands exactly one buffer every buffer period, on time,
// forever. The jitter buffer between them is what reconciles those two, and no
// RTP library provides one -- uvgRTP's only "jitter" is the RTCP report field.
//
// Feed it with uvgrtp_send.cpp.

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <uvgrtp/lib.hh>
#include <vector>

namespace {

constexpr char kLocalAddress[] = "0.0.0.0"; // RCE_RECEIVE_ONLY: where to bind
constexpr std::uint16_t kPort = 5004;
constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint32_t kChannels = 1;
constexpr std::size_t kFramesPerPacket = 480; // 10 ms

// The whole receive-side latency budget. 1 packet is the floor; raise it if
// underruns climb.
constexpr std::size_t kJitterPackets = 2;

// A gap longer than this is a stream restart, not loss; resync instead of
// concealing 100 ms of silence.
constexpr std::uint16_t kMaxConcealPackets = 8;

constexpr std::uint32_t kMaxFramesPerSlice = 4096;
constexpr AudioUnitElement kOutputElement = 0;

std::atomic_bool g_quit{false};

void on_signal(int /*sig*/) {
    g_quit.store(true, std::memory_order_relaxed);
}

bool check(OSStatus status, const char* what) {
    if (status == noErr) {
        return true;
    }
    std::cerr << what << " failed: " << status << "\n";
    return false;
}

// Single-producer single-consumer: uvgRTP's receive thread writes, the
// CoreAudio render thread reads, so neither needs a lock.
class JitterBuffer {
  public:
    JitterBuffer()
        : target_(kJitterPackets * kFramesPerPacket),
          // Headroom above the target absorbs a burst; pull() trims back down,
          // so latency cannot creep upwards. 50 packets is 500 ms.
          limit_((kJitterPackets + 4) * kFramesPerPacket), buffer_(50 * kFramesPerPacket) {}

    // Producer: uvgRTP's receive thread.
    void push(std::uint16_t sequence, const std::uint8_t* payload, std::size_t bytes) {
        ++received_;

        if (synced_) {
            // A missing packet becomes the same amount of silence, so audio
            // after a dropout is not shifted earlier in time.
            const auto gap = static_cast<std::int16_t>(sequence - expected_);
            if (gap < 0) {
                ++late_; // reordered or duplicate; its slot is already played
                return;
            }
            if (gap > 0 && gap <= kMaxConcealPackets) {
                lost_ += static_cast<std::uint64_t>(gap);
                for (std::size_t i = 0; i < static_cast<std::size_t>(gap) * kFramesPerPacket; ++i) {
                    write(0);
                }
            }
        }
        synced_ = true;
        expected_ = static_cast<std::uint16_t>(sequence + 1);

        // L16 is big-endian on the wire.
        const std::size_t frames = std::min(bytes / 2, kFramesPerPacket);
        if (space() < frames) {
            ++dropped_;
            return;
        }
        for (std::size_t i = 0; i < frames; ++i) {
            write(static_cast<std::int16_t>(payload[i * 2] << 8 | payload[i * 2 + 1]));
        }
        commit();
    }

    // Consumer: the CoreAudio render thread. Never blocks; zero-fills instead.
    void pull(float* out, std::size_t frames) {
        const std::size_t queued = size();

        if (queued > limit_ + frames) {
            // Sender's clock is faster than ours, or a burst landed. Give up
            // the excess audio rather than the latency: trim to one request
            // above the target so the reads below leave exactly the target.
            advance_read(queued - (target_ + frames));
            ++overruns_;
        }

        if (priming_) {
            if (size() < target_ + frames) {
                std::fill(out, out + frames, 0.0F); // still filling
                return;
            }
            priming_ = false;
        }

        const std::size_t available = std::min(frames, size());
        for (std::size_t i = 0; i < available; ++i) {
            out[i] = static_cast<float>(read()) / 32768.0F;
        }
        commit_read();
        std::fill(out + available, out + frames, 0.0F);

        if (available < frames) {
            ++underruns_;
            priming_ = true; // refill to the target before trying again
        }
    }

    void report() const {
        std::cout << "rx " << received_ << ", lost " << lost_ << ", late " << late_ << ", dropped "
                  << dropped_ << ", underruns " << underruns_ << ", overruns " << overruns_ << "\n";
    }

  private:
    std::size_t size() const {
        const std::size_t w = write_.load(std::memory_order_acquire);
        const std::size_t r = read_.load(std::memory_order_acquire);
        return (w >= r) ? (w - r) : (buffer_.size() - r + w);
    }

    std::size_t space() const {
        return buffer_.size() - 1 - size();
    }

    void write(std::int16_t sample) {
        buffer_[pending_write_] = sample;
        pending_write_ = (pending_write_ + 1 == buffer_.size()) ? 0 : pending_write_ + 1;
    }

    void commit() {
        write_.store(pending_write_, std::memory_order_release);
    }

    std::int16_t read() {
        const std::int16_t sample = buffer_[pending_read_];
        pending_read_ = (pending_read_ + 1 == buffer_.size()) ? 0 : pending_read_ + 1;
        return sample;
    }

    void commit_read() {
        read_.store(pending_read_, std::memory_order_release);
    }

    void advance_read(std::size_t count) {
        pending_read_ = (pending_read_ + std::min(count, size())) % buffer_.size();
        commit_read();
    }

    std::size_t target_;
    std::size_t limit_;
    std::vector<std::int16_t> buffer_;
    std::atomic<std::size_t> read_{0};
    std::atomic<std::size_t> write_{0};
    std::size_t pending_read_ = 0;  // consumer thread only
    std::size_t pending_write_ = 0; // producer thread only
    std::uint16_t expected_ = 0;
    bool synced_ = false;
    bool priming_ = true;
    // Counters: each is touched by one thread and only read for display.
    std::atomic<std::uint64_t> received_{0};
    std::atomic<std::uint64_t> lost_{0};
    std::atomic<std::uint64_t> late_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> underruns_{0};
    std::atomic<std::uint64_t> overruns_{0};
};

// Runs on uvgRTP's receive thread. Must not block: queue and return.
void on_frame(void* arg, uvgrtp::frame::rtp_frame* frame) {
    auto* jitter = static_cast<JitterBuffer*>(arg);
    jitter->push(frame->header.seq, frame->payload, frame->payload_len);
    uvgrtp::frame::dealloc_frame(frame);
}

// Runs on the CoreAudio render thread. A lock-free read, nothing else.
OSStatus on_render(void* arg, AudioUnitRenderActionFlags* /*flags*/,
                   const AudioTimeStamp* /*stamp*/, UInt32 /*bus*/, UInt32 frames,
                   AudioBufferList* io) {
    auto* jitter = static_cast<JitterBuffer*>(arg);

    auto* out = static_cast<float*>(io->mBuffers[0].mData);
    jitter->pull(out, frames);

    // Mono source into a possibly multi-channel device buffer layout.
    for (UInt32 b = 1; b < io->mNumberBuffers; ++b) {
        std::memcpy(io->mBuffers[b].mData, out, frames * sizeof(float));
    }
    return noErr;
}

AudioDeviceID default_output_device() {
    AudioObjectPropertyAddress address{kAudioHardwarePropertyDefaultOutputDevice,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    AudioDeviceID device = kAudioObjectUnknown;
    UInt32 size = sizeof(device);
    if (!check(AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size,
                                          &device),
               "get default output device")) {
        return kAudioObjectUnknown;
    }
    return device;
}

// The biggest latency lever on the device side: ask for exactly one packet per
// callback, so no repacking buffer is needed anywhere.
void request_buffer_frame_size(AudioDeviceID device, UInt32 frames) {
    AudioObjectPropertyAddress address{kAudioDevicePropertyBufferFrameSize,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    if (AudioObjectSetPropertyData(device, &address, 0, nullptr, sizeof(frames), &frames) !=
        noErr) {
        std::cerr << "note: device refused a " << frames << " frame buffer; keeping its own\n";
        return;
    }
    UInt32 actual = 0;
    UInt32 size = sizeof(actual);
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &actual) == noErr) {
        std::cout << "device buffer: " << actual << " frames (" << (1000.0 * actual / kSampleRate)
                  << " ms)\n";
    }
}

} // namespace

int main() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    JitterBuffer jitter;

    // --- uvgRTP ---------------------------------------------------------
    uvgrtp::context context;
    uvgrtp::session* session = context.create_session(kLocalAddress);
    if (session == nullptr) {
        std::cerr << "create_session failed\n";
        return EXIT_FAILURE;
    }
    uvgrtp::media_stream* stream =
        session->create_stream(kPort, RTP_FORMAT_L16_MONO, RCE_RECEIVE_ONLY);
    if (stream == nullptr) {
        std::cerr << "create_stream failed\n";
        return EXIT_FAILURE;
    }
    // L16_MONO is RFC 3551 payload type 11, which fixes the clock at 44.1 kHz.
    stream->configure_ctx(RCC_CLOCK_RATE, kSampleRate);

    // --- CoreAudio ------------------------------------------------------
    const AudioComponentDescription desc{
        .componentType = kAudioUnitType_Output,
        .componentSubType = kAudioUnitSubType_HALOutput,
        .componentManufacturer = kAudioUnitManufacturer_Apple,
        .componentFlags = 0,
        .componentFlagsMask = 0,
    };
    AudioComponent component = AudioComponentFindNext(nullptr, &desc);
    if (component == nullptr) {
        std::cerr << "no HAL output component\n";
        return EXIT_FAILURE;
    }
    AudioUnit unit = nullptr;
    if (!check(AudioComponentInstanceNew(component, &unit), "AudioComponentInstanceNew")) {
        return EXIT_FAILURE;
    }

    AudioDeviceID device = default_output_device();
    if (device == kAudioObjectUnknown) {
        return EXIT_FAILURE;
    }
    request_buffer_frame_size(device, kFramesPerPacket);
    if (!check(AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice,
                                    kAudioUnitScope_Global, kOutputElement, &device,
                                    sizeof(device)),
               "set current device")) {
        return EXIT_FAILURE;
    }

    AudioStreamBasicDescription format{};
    format.mSampleRate = kSampleRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mChannelsPerFrame = kChannels;
    format.mBitsPerChannel = 32;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof(float) * kChannels;
    format.mBytesPerPacket = format.mBytesPerFrame;
    if (!check(AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input,
                                    kOutputElement, &format, sizeof(format)),
               "set output stream format")) {
        return EXIT_FAILURE;
    }

    UInt32 max_frames = kMaxFramesPerSlice;
    if (!check(AudioUnitSetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice,
                                    kAudioUnitScope_Global, kOutputElement, &max_frames,
                                    sizeof(max_frames)),
               "set max frames per slice")) {
        return EXIT_FAILURE;
    }

    const AURenderCallbackStruct callback{.inputProc = on_render, .inputProcRefCon = &jitter};
    if (!check(AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback,
                                    kAudioUnitScope_Input, kOutputElement, &callback,
                                    sizeof(callback)),
               "set render callback")) {
        return EXIT_FAILURE;
    }

    // --- run ------------------------------------------------------------
    if (stream->install_receive_hook(&jitter, on_frame) != RTP_OK) {
        std::cerr << "install_receive_hook failed\n";
        return EXIT_FAILURE;
    }
    if (!check(AudioUnitInitialize(unit), "AudioUnitInitialize") ||
        !check(AudioOutputUnitStart(unit), "AudioOutputUnitStart")) {
        return EXIT_FAILURE;
    }

    std::cout << "playing L16 from port " << kPort << ", jitter buffer " << kJitterPackets
              << " packets (" << (kJitterPackets * 1000 * kFramesPerPacket / kSampleRate)
              << " ms) (ctrl-c to stop)\n";
    while (!g_quit.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        jitter.report();
    }

    AudioOutputUnitStop(unit);
    AudioUnitUninitialize(unit);
    AudioComponentInstanceDispose(unit);
    // Destroying the session stops and joins uvgRTP's receive thread, so the
    // hook cannot fire after the jitter buffer goes out of scope.
    context.destroy_session(session);
    return EXIT_SUCCESS;
}
