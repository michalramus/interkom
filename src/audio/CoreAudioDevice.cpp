#include "audio/CoreAudioDevice.hpp"

#include <cmath>
#include <iostream>

void request_chunk_buffer(AudioDeviceID device, std::size_t framesPerChunk,
                          std::uint32_t sampleRate) {
    AudioObjectPropertyAddress rate_address{kAudioDevicePropertyNominalSampleRate,
                                            kAudioObjectPropertyScopeGlobal,
                                            kAudioObjectPropertyElementMain};
    Float64 deviceRate = sampleRate;
    UInt32 size = sizeof(deviceRate);
    AudioObjectGetPropertyData(device, &rate_address, 0, nullptr, &size, &deviceRate);

    AudioObjectPropertyAddress buffer_address{kAudioDevicePropertyBufferFrameSize,
                                              kAudioObjectPropertyScopeGlobal,
                                              kAudioObjectPropertyElementMain};
    auto frames = static_cast<UInt32>(
        std::lround(static_cast<double>(framesPerChunk) * deviceRate / sampleRate));
    AudioObjectSetPropertyData(device, &buffer_address, 0, nullptr, sizeof(frames), &frames);

    UInt32 actual = 0;
    size = sizeof(actual);
    if (AudioObjectGetPropertyData(device, &buffer_address, 0, nullptr, &size, &actual) == noErr) {
        std::cout << "device " << device << ": " << actual << " frames at " << deviceRate
                  << " Hz (" << (1000.0 * actual / deviceRate) << " ms)\n";
    }
}
