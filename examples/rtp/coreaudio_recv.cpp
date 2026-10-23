// RTP -> CoreAudio AUHAL: plays an incoming L16 stream on the default output.
//
// The render callback only drains rtp::Receiver's jitter buffer; a receive
// thread sits blocked in recv(), so nothing on the real-time thread blocks.
//
// Feed it from the matching example, or with:
//   ffmpeg -re -f lavfi -i "sine=frequency=440:sample_rate=48000" \
//          -ac 1 -c:a pcm_s16be -payload_type 96 -f rtp rtp://127.0.0.1:5004

#include "rtp.hpp"

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>

namespace {

// The entire receive-side latency budget, in packets. 1 is the floor; raise it
// if the underrun counter climbs on your network.
constexpr std::uint32_t kJitterPackets = 2;

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

// Runs on the CoreAudio render thread: a lock-free read from the jitter
// buffer, which zero-fills rather than waiting if the network is behind.
OSStatus on_render(void* userdata, AudioUnitRenderActionFlags* /*flags*/,
                   const AudioTimeStamp* /*stamp*/, UInt32 /*bus*/, UInt32 frames,
                   AudioBufferList* io) {
    auto* receiver = static_cast<rtp::Receiver*>(userdata);

    auto* out = static_cast<float*>(io->mBuffers[0].mData);
    receiver->pull(out, frames);

    // Mono source into a possibly multi-channel device buffer layout.
    for (UInt32 b = 1; b < io->mNumberBuffers; ++b) {
        std::memcpy(io->mBuffers[b].mData, out, frames * sizeof(float));
    }
    return noErr;
}

AudioDeviceID default_device(AudioObjectPropertySelector selector) {
    AudioObjectPropertyAddress address{selector, kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    AudioDeviceID device = kAudioObjectUnknown;
    UInt32 size = sizeof(device);
    if (!check(AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size,
                                          &device),
               "get default device")) {
        return kAudioObjectUnknown;
    }
    return device;
}

// The single biggest latency lever: the device's own I/O quantum. Asking for
// exactly one packet's worth makes each callback consume exactly one packet.
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
        std::cout << "device buffer: " << actual << " frames ("
                  << (1000.0 * actual / rtp::kSampleRate) << " ms)\n";
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

    AudioDeviceID device = default_device(kAudioHardwarePropertyDefaultOutputDevice);
    if (device == kAudioObjectUnknown) {
        return EXIT_FAILURE;
    }
    request_buffer_frame_size(device, rtp::kFramesPerPacket);
    if (!check(AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice,
                                    kAudioUnitScope_Global, kOutputElement, &device,
                                    sizeof(device)),
               "set current device")) {
        return EXIT_FAILURE;
    }

    // Client-side format on the input scope of the output element; AUHAL
    // converts to whatever the device natively runs at.
    AudioStreamBasicDescription format{};
    format.mSampleRate = rtp::kSampleRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mChannelsPerFrame = rtp::kChannels;
    format.mBitsPerChannel = 32;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof(float) * rtp::kChannels;
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

    const AURenderCallbackStruct callback{.inputProc = on_render, .inputProcRefCon = &receiver};
    if (!check(AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback,
                                    kAudioUnitScope_Input, kOutputElement, &callback,
                                    sizeof(callback)),
               "set render callback")) {
        return EXIT_FAILURE;
    }

    receiver.start();
    if (!check(AudioUnitInitialize(unit), "AudioUnitInitialize") ||
        !check(AudioOutputUnitStart(unit), "AudioOutputUnitStart")) {
        return EXIT_FAILURE;
    }

    std::cout << "listening for L16/" << rtp::kSampleRate << "/" << rtp::kChannels << " on port "
              << rtp::kPort << ", jitter buffer " << kJitterPackets << " packets ("
              << (kJitterPackets * rtp::kPtimeMs) << " ms) (ctrl-c to stop)\n";
    while (!g_quit.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cout << "rx " << receiver.packets_received() << ", lost " << receiver.packets_lost()
                  << ", late " << receiver.packets_late() << ", dropped "
                  << receiver.packets_dropped() << ", underruns " << receiver.underruns()
                  << ", overruns " << receiver.overruns() << "\n";
    }

    AudioOutputUnitStop(unit);
    AudioUnitUninitialize(unit);
    AudioComponentInstanceDispose(unit);
    receiver.stop();
    return EXIT_SUCCESS;
}
