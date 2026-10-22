// CoreAudio AUHAL playback: plays a 440 Hz sine, mono 48 kHz float32.
//
// The render callback is pulled by CoreAudio on a real-time thread, which is
// how a real intercom would drain its jitter buffer.

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <numbers>
#include <thread>

namespace {

constexpr double kSampleRate = 48000.0;
constexpr std::uint32_t kChannels = 1;
constexpr std::uint32_t kMaxFramesPerSlice = 4096;
constexpr AudioUnitElement kOutputElement = 0;
constexpr double kToneHz = 440.0;
constexpr float kAmplitude = 0.25F;

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

struct ToneGenerator {
    double phase = 0.0;
};

// Runs on the CoreAudio render thread: generate in place, never block.
OSStatus on_render(void* userdata, AudioUnitRenderActionFlags* /*flags*/,
                   const AudioTimeStamp* /*stamp*/, UInt32 /*bus*/, UInt32 frames,
                   AudioBufferList* io) {
    auto* tone = static_cast<ToneGenerator*>(userdata);
    constexpr double step = 2.0 * std::numbers::pi * kToneHz / kSampleRate;

    auto* out = static_cast<float*>(io->mBuffers[0].mData);
    for (UInt32 i = 0; i < frames; ++i) {
        out[i] = static_cast<float>(std::sin(tone->phase)) * kAmplitude;
        tone->phase += step;
        if (tone->phase >= 2.0 * std::numbers::pi) {
            tone->phase -= 2.0 * std::numbers::pi;
        }
    }

    // Mono source into a possibly multi-channel device buffer layout.
    for (UInt32 b = 1; b < io->mNumberBuffers; ++b) {
        std::memcpy(io->mBuffers[b].mData, out, frames * sizeof(float));
    }
    return noErr;
}

bool set_default_output_device(AudioUnit unit) {
    AudioObjectPropertyAddress address{kAudioHardwarePropertyDefaultOutputDevice,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    AudioDeviceID device = kAudioObjectUnknown;
    UInt32 size = sizeof(device);
    if (!check(AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size,
                                          &device),
               "get default output device")) {
        return false;
    }
    return check(AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice,
                                      kAudioUnitScope_Global, kOutputElement, &device,
                                      sizeof(device)),
                 "set current device");
}

} // namespace

int main() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    ToneGenerator tone;

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
    if (!set_default_output_device(unit)) {
        return EXIT_FAILURE;
    }

    // Client-side format on the input scope of the output element; AUHAL
    // converts to whatever the device natively runs at.
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

    const AURenderCallbackStruct callback{.inputProc = on_render, .inputProcRefCon = &tone};
    if (!check(AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback,
                                    kAudioUnitScope_Input, kOutputElement, &callback,
                                    sizeof(callback)),
               "set render callback")) {
        return EXIT_FAILURE;
    }

    if (!check(AudioUnitInitialize(unit), "AudioUnitInitialize") ||
        !check(AudioOutputUnitStart(unit), "AudioOutputUnitStart")) {
        return EXIT_FAILURE;
    }

    std::cout << "playing " << kToneHz << " Hz (ctrl-c to stop)\n";
    while (!g_quit.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    AudioOutputUnitStop(unit);
    AudioUnitUninitialize(unit);
    AudioComponentInstanceDispose(unit);
    return EXIT_SUCCESS;
}
