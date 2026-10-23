// CoreAudio AUHAL -> RTP: captures the default input and sends it as L16.
//
// The capture callback converts and queues; rtp::Sender's own thread does the
// socket write, so nothing on the real-time thread can block.
//
// Needs microphone permission; a plain CLI binary triggers the TCC prompt on
// first run (grant it to the terminal app, not the binary).
//
// Receive it with the matching example, or with:
//   ffplay -protocol_whitelist file,rtp,udp -i examples/rtp/l16.sdp

#include "rtp.hpp"

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {

// Where to send. Change this to the other machine's address; loopback lets a
// single box run the send and receive examples against each other.
constexpr const char* kDestination = "127.0.0.1";

constexpr std::uint32_t kMaxFramesPerSlice = 4096;
constexpr AudioUnitElement kInputElement = 1;  // the device-facing side of AUHAL
constexpr AudioUnitElement kOutputElement = 0; // the app-facing side

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

struct Transmitter {
    AudioUnit unit = nullptr;
    rtp::Sender* sender = nullptr;
    std::array<float, kMaxFramesPerSlice> scratch{}; // AUHAL hands us float samples
};

// Runs on the CoreAudio render thread: render, convert, queue. No allocation,
// no locks, no system call -- the sender thread owns the socket.
OSStatus on_input(void* userdata, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* stamp,
                  UInt32 bus, UInt32 frames, AudioBufferList* /*io*/) {
    auto* tx = static_cast<Transmitter*>(userdata);
    if (frames > tx->scratch.size()) {
        return noErr; // would overflow our preallocated buffer
    }

    // AUHAL does not pass input data in; we must pull it out ourselves.
    AudioBufferList list{};
    list.mNumberBuffers = 1;
    list.mBuffers[0].mNumberChannels = rtp::kChannels;
    list.mBuffers[0].mDataByteSize = frames * sizeof(float);
    list.mBuffers[0].mData = tx->scratch.data();

    const OSStatus status = AudioUnitRender(tx->unit, flags, stamp, bus, frames, &list);
    if (status != noErr) {
        return status;
    }

    tx->sender->send(tx->scratch.data(), frames);
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
// exactly one packet's worth makes each callback produce exactly one packet.
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

    rtp::Sender sender{kDestination};
    if (!sender.ok()) {
        std::cerr << "rtp sender: " << sender.error() << "\n";
        return EXIT_FAILURE;
    }

    Transmitter tx;
    tx.sender = &sender;

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
    if (!check(AudioComponentInstanceNew(component, &tx.unit), "AudioComponentInstanceNew")) {
        return EXIT_FAILURE;
    }

    // AUHAL starts out playback-only: enable input, disable output.
    UInt32 enable = 1;
    UInt32 disable = 0;
    if (!check(AudioUnitSetProperty(tx.unit, kAudioOutputUnitProperty_EnableIO,
                                    kAudioUnitScope_Input, kInputElement, &enable, sizeof(enable)),
               "enable input") ||
        !check(AudioUnitSetProperty(tx.unit, kAudioOutputUnitProperty_EnableIO,
                                    kAudioUnitScope_Output, kOutputElement, &disable,
                                    sizeof(disable)),
               "disable output")) {
        return EXIT_FAILURE;
    }

    AudioDeviceID device = default_device(kAudioHardwarePropertyDefaultInputDevice);
    if (device == kAudioObjectUnknown) {
        return EXIT_FAILURE;
    }
    request_buffer_frame_size(device, rtp::kFramesPerPacket);
    if (!check(AudioUnitSetProperty(tx.unit, kAudioOutputUnitProperty_CurrentDevice,
                                    kAudioUnitScope_Global, kOutputElement, &device,
                                    sizeof(device)),
               "set current device")) {
        return EXIT_FAILURE;
    }

    // Client-side format on the output scope of the input element; AUHAL
    // converts from whatever the device natively runs at.
    AudioStreamBasicDescription format{};
    format.mSampleRate = rtp::kSampleRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mChannelsPerFrame = rtp::kChannels;
    format.mBitsPerChannel = 32;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof(float) * rtp::kChannels;
    format.mBytesPerPacket = format.mBytesPerFrame;
    if (!check(AudioUnitSetProperty(tx.unit, kAudioUnitProperty_StreamFormat,
                                    kAudioUnitScope_Output, kInputElement, &format, sizeof(format)),
               "set input stream format")) {
        return EXIT_FAILURE;
    }

    UInt32 max_frames = kMaxFramesPerSlice;
    if (!check(AudioUnitSetProperty(tx.unit, kAudioUnitProperty_MaximumFramesPerSlice,
                                    kAudioUnitScope_Global, kOutputElement, &max_frames,
                                    sizeof(max_frames)),
               "set max frames per slice")) {
        return EXIT_FAILURE;
    }

    const AURenderCallbackStruct callback{.inputProc = on_input, .inputProcRefCon = &tx};
    if (!check(AudioUnitSetProperty(tx.unit, kAudioOutputUnitProperty_SetInputCallback,
                                    kAudioUnitScope_Global, kOutputElement, &callback,
                                    sizeof(callback)),
               "set input callback")) {
        return EXIT_FAILURE;
    }

    sender.start();
    if (!check(AudioUnitInitialize(tx.unit), "AudioUnitInitialize") ||
        !check(AudioOutputUnitStart(tx.unit), "AudioOutputUnitStart")) {
        return EXIT_FAILURE;
    }

    std::cout << "sending L16/" << rtp::kSampleRate << "/" << rtp::kChannels << " to "
              << kDestination << ":" << rtp::kPort << ", " << rtp::kPtimeMs
              << " ms packets (ctrl-c to stop)\n";
    while (!g_quit.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cout << "sent " << sender.packets_sent() << " packets, " << sender.send_errors()
                  << " errors, " << sender.frames_dropped() << " frames dropped\n";
    }

    AudioOutputUnitStop(tx.unit);
    AudioUnitUninitialize(tx.unit);
    AudioComponentInstanceDispose(tx.unit);
    sender.stop();
    return EXIT_SUCCESS;
}
