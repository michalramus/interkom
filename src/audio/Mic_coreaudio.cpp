// CoreAudio AUHAL backend for Mic. Based on examples/rtp/coreaudio_send.cpp.

#include "audio/CoreAudioDevice.hpp"
#include "audio/Mic.hpp"

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <iostream>
#include <vector>

namespace {

constexpr std::uint32_t kMaxFramesPerSlice = 4096;
constexpr AudioUnitElement kInputElement = 1;  // the device-facing side of AUHAL
constexpr AudioUnitElement kOutputElement = 0; // the app-facing side

bool check(OSStatus status, const char* what) {
    if (status == noErr) {
        return true;
    }
    std::cerr << what << " failed: " << status << "\n";
    return false;
}

AudioDeviceID default_input_device() {
    AudioObjectPropertyAddress address{kAudioHardwarePropertyDefaultInputDevice,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    AudioDeviceID device = kAudioObjectUnknown;
    UInt32 size = sizeof(device);
    if (!check(AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size,
                                          &device),
               "get default input device")) {
        return kAudioObjectUnknown;
    }
    return device;
}

} // namespace

struct Mic::Impl {
    AudioConfig config;
    Callback callback;
    AudioUnit unit = nullptr;
    std::vector<std::int16_t> scratch;
    double deviceRate = 0; // frames reach on_input at this rate
};

namespace {

// Runs on the CoreAudio thread: pull the input out of AUHAL, hand it on.
OSStatus on_input(void* userdata, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* stamp,
                  UInt32 bus, UInt32 frames, AudioBufferList* /*io*/) {
    auto* impl = static_cast<Mic::Impl*>(userdata);
    // inNumberFrames counts device frames; AUHAL converts to our rate, so ask
    // for that many of ours.
    frames = static_cast<UInt32>(static_cast<double>(frames) * impl->config.sampleRate /
                                 impl->deviceRate);
    if (frames * impl->config.channels > impl->scratch.size()) {
        return noErr; // would overflow our preallocated buffer
    }

    AudioBufferList list{};
    list.mNumberBuffers = 1;
    list.mBuffers[0].mNumberChannels = impl->config.channels;
    list.mBuffers[0].mDataByteSize = frames * impl->config.channels * sizeof(std::int16_t);
    list.mBuffers[0].mData = impl->scratch.data();

    const OSStatus status = AudioUnitRender(impl->unit, flags, stamp, bus, frames, &list);
    if (status != noErr) {
        return status;
    }
    impl->callback(impl->scratch.data(), frames);
    return noErr;
}

} // namespace

Mic::Mic(AudioConfig config, Callback callback) : impl_(std::make_unique<Impl>()) {
    impl_->config = config;
    impl_->callback = std::move(callback);
    impl_->scratch.resize(kMaxFramesPerSlice * config.channels);
}

Mic::~Mic() {
    stop();
}

bool Mic::start() {
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
        return false;
    }
    AudioUnit& unit = impl_->unit;
    if (!check(AudioComponentInstanceNew(component, &unit), "AudioComponentInstanceNew")) {
        return false;
    }

    // AUHAL starts out playback-only: enable input, disable output.
    UInt32 enable = 1;
    UInt32 disable = 0;
    if (!check(AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input,
                                    kInputElement, &enable, sizeof(enable)),
               "enable input") ||
        !check(AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output,
                                    kOutputElement, &disable, sizeof(disable)),
               "disable output")) {
        return false;
    }

    AudioDeviceID device = default_input_device();
    if (device == kAudioObjectUnknown) {
        return false;
    }
    impl_->deviceRate =
        request_chunk_buffer(device, impl_->config.framesPerChunk, impl_->config.sampleRate);
    if (!check(AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice,
                                    kAudioUnitScope_Global, kOutputElement, &device,
                                    sizeof(device)),
               "set current device")) {
        return false;
    }

    // Signed 16-bit straight from AUHAL, which converts from the device format.
    AudioStreamBasicDescription format{};
    format.mSampleRate = impl_->config.sampleRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    format.mChannelsPerFrame = impl_->config.channels;
    format.mBitsPerChannel = 16;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof(std::int16_t) * impl_->config.channels;
    format.mBytesPerPacket = format.mBytesPerFrame;
    if (!check(AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output,
                                    kInputElement, &format, sizeof(format)),
               "set input stream format")) {
        return false;
    }

    UInt32 max_frames = kMaxFramesPerSlice;
    if (!check(AudioUnitSetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice,
                                    kAudioUnitScope_Global, kOutputElement, &max_frames,
                                    sizeof(max_frames)),
               "set max frames per slice")) {
        return false;
    }

    const AURenderCallbackStruct callback{.inputProc = on_input, .inputProcRefCon = impl_.get()};
    if (!check(AudioUnitSetProperty(unit, kAudioOutputUnitProperty_SetInputCallback,
                                    kAudioUnitScope_Global, kOutputElement, &callback,
                                    sizeof(callback)),
               "set input callback")) {
        return false;
    }

    return check(AudioUnitInitialize(unit), "AudioUnitInitialize") &&
           check(AudioOutputUnitStart(unit), "AudioOutputUnitStart");
}

void Mic::stop() {
    if (impl_->unit == nullptr) {
        return;
    }
    AudioOutputUnitStop(impl_->unit);
    AudioUnitUninitialize(impl_->unit);
    AudioComponentInstanceDispose(impl_->unit);
    impl_->unit = nullptr;
}
