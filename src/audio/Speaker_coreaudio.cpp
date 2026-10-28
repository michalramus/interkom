// CoreAudio AUHAL backend for Speaker. Based on examples/coreaudio/playback.cpp
// and examples/rtp/uvgrtp_coreaudio_recv.cpp.

#include "audio/Speaker.hpp"

#include "audio/CoreAudioDevice.hpp"

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <algorithm>
#include <cstring>
#include <iostream>

namespace {

constexpr std::uint32_t kMaxFramesPerSlice = 4096;
constexpr AudioUnitElement kOutputElement = 0;

bool check(OSStatus status, const char* what) {
    if (status == noErr) {
        return true;
    }
    std::cerr << what << " failed: " << status << "\n";
    return false;
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

} // namespace

struct Speaker::Impl {
    AudioConfig config;
    Source source;
    AudioUnit unit = nullptr;
    const std::int16_t* chunk = nullptr; // last chunk from source
    std::size_t offset = 0;              // samples of it already played
};

namespace {

// Runs on the CoreAudio render thread: convert chunks into the device buffer.
OSStatus on_render(void* userdata, AudioUnitRenderActionFlags* /*flags*/,
                   const AudioTimeStamp* /*stamp*/, UInt32 /*bus*/, UInt32 frames,
                   AudioBufferList* io) {
    auto* impl = static_cast<Speaker::Impl*>(userdata);
    const std::size_t chunkFrames = impl->config.framesPerChunk;

    auto* out = static_cast<float*>(io->mBuffers[0].mData);
    std::size_t written = 0;
    while (written < frames) {
        if (impl->chunk == nullptr || impl->offset == chunkFrames) {
            impl->chunk = impl->source();
            impl->offset = 0;
        }
        const std::size_t count = std::min(frames - written, chunkFrames - impl->offset);
        for (std::size_t i = 0; i < count; ++i) {
            out[written + i] = static_cast<float>(impl->chunk[impl->offset + i]) / 32768.0F;
        }
        written += count;
        impl->offset += count;
    }

    // Mono source into a possibly multi-channel device buffer layout.
    for (UInt32 b = 1; b < io->mNumberBuffers; ++b) {
        std::memcpy(io->mBuffers[b].mData, out, frames * sizeof(float));
    }
    return noErr;
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

    AudioDeviceID device = default_output_device();
    if (device == kAudioObjectUnknown) {
        return false;
    }
    request_chunk_buffer(device, impl_->config.framesPerChunk, impl_->config.sampleRate);
    if (!check(AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice,
                                    kAudioUnitScope_Global, kOutputElement, &device,
                                    sizeof(device)),
               "set current device")) {
        return false;
    }

    // Mono float32 in; AUHAL converts to whatever the device runs at.
    AudioStreamBasicDescription format{};
    format.mSampleRate = impl_->config.sampleRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mChannelsPerFrame = 1;
    format.mBitsPerChannel = 32;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof(float);
    format.mBytesPerPacket = format.mBytesPerFrame;
    if (!check(AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input,
                                    kOutputElement, &format, sizeof(format)),
               "set output stream format")) {
        return false;
    }

    UInt32 max_frames = kMaxFramesPerSlice;
    if (!check(AudioUnitSetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice,
                                    kAudioUnitScope_Global, kOutputElement, &max_frames,
                                    sizeof(max_frames)),
               "set max frames per slice")) {
        return false;
    }

    const AURenderCallbackStruct callback{.inputProc = on_render, .inputProcRefCon = impl_.get()};
    if (!check(AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback,
                                    kAudioUnitScope_Input, kOutputElement, &callback,
                                    sizeof(callback)),
               "set render callback")) {
        return false;
    }

    return check(AudioUnitInitialize(unit), "AudioUnitInitialize") &&
           check(AudioOutputUnitStart(unit), "AudioOutputUnitStart");
}

void Speaker::stop() {
    if (impl_->unit == nullptr) {
        return;
    }
    AudioOutputUnitStop(impl_->unit);
    AudioUnitUninitialize(impl_->unit);
    AudioComponentInstanceDispose(impl_->unit);
    impl_->unit = nullptr;
    impl_->chunk = nullptr;
}
