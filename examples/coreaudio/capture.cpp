// CoreAudio AUHAL capture: records mono 48 kHz S16LE to a raw file.
//
// Needs microphone permission; a plain CLI binary triggers the TCC prompt on
// first run (grant it to the terminal app, not the binary).
//
// Play back with:
//   ffplay -f s16le -ar 48000 -ac 1 capture.raw

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr double kSampleRate = 48000.0;
constexpr std::uint32_t kChannels = 1;
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

struct Recorder {
    AudioUnit unit = nullptr;
    std::FILE* file = nullptr;
    std::vector<float> scratch;      // AUHAL hands us float samples
    std::vector<std::int16_t> pcm16; // converted before writing
    std::uint64_t bytes = 0;
};

// Runs on the CoreAudio render thread: no allocation, no locks, no I/O beyond
// the buffered fwrite below (kept simple for an example).
OSStatus on_input(void* userdata, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* stamp,
                  UInt32 bus, UInt32 frames, AudioBufferList* /*io*/) {
    auto* rec = static_cast<Recorder*>(userdata);
    if (frames > rec->scratch.size()) {
        return noErr; // would overflow our preallocated buffer
    }

    // AUHAL does not pass input data in; we must pull it out ourselves.
    AudioBufferList list{};
    list.mNumberBuffers = 1;
    list.mBuffers[0].mNumberChannels = kChannels;
    list.mBuffers[0].mDataByteSize = frames * sizeof(float);
    list.mBuffers[0].mData = rec->scratch.data();

    const OSStatus status = AudioUnitRender(rec->unit, flags, stamp, bus, frames, &list);
    if (status != noErr) {
        return status;
    }

    for (UInt32 i = 0; i < frames; ++i) {
        const float clamped = std::clamp(rec->scratch[i], -1.0F, 1.0F);
        rec->pcm16[i] = static_cast<std::int16_t>(clamped * 32767.0F);
    }
    rec->bytes += std::fwrite(rec->pcm16.data(), sizeof(std::int16_t), frames, rec->file) *
                  sizeof(std::int16_t);
    return noErr;
}

bool set_default_input_device(AudioUnit unit) {
    AudioObjectPropertyAddress address{kAudioHardwarePropertyDefaultInputDevice,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    AudioDeviceID device = kAudioObjectUnknown;
    UInt32 size = sizeof(device);
    if (!check(AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size,
                                          &device),
               "get default input device")) {
        return false;
    }
    return check(AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice,
                                      kAudioUnitScope_Global, kOutputElement, &device,
                                      sizeof(device)),
                 "set current device");
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
    rec.scratch.resize(kMaxFramesPerSlice);
    rec.pcm16.resize(kMaxFramesPerSlice);

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

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
    if (!check(AudioComponentInstanceNew(component, &rec.unit), "AudioComponentInstanceNew")) {
        return EXIT_FAILURE;
    }

    // AUHAL starts out playback-only: enable input, disable output.
    UInt32 enable = 1;
    UInt32 disable = 0;
    if (!check(AudioUnitSetProperty(rec.unit, kAudioOutputUnitProperty_EnableIO,
                                    kAudioUnitScope_Input, kInputElement, &enable, sizeof(enable)),
               "enable input") ||
        !check(AudioUnitSetProperty(rec.unit, kAudioOutputUnitProperty_EnableIO,
                                    kAudioUnitScope_Output, kOutputElement, &disable,
                                    sizeof(disable)),
               "disable output") ||
        !set_default_input_device(rec.unit)) {
        return EXIT_FAILURE;
    }

    // Client-side format on the output scope of the input element; AUHAL
    // converts from whatever the device natively runs at.
    AudioStreamBasicDescription format{};
    format.mSampleRate = kSampleRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mChannelsPerFrame = kChannels;
    format.mBitsPerChannel = 32;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof(float) * kChannels;
    format.mBytesPerPacket = format.mBytesPerFrame;
    if (!check(AudioUnitSetProperty(rec.unit, kAudioUnitProperty_StreamFormat,
                                    kAudioUnitScope_Output, kInputElement, &format, sizeof(format)),
               "set input stream format")) {
        return EXIT_FAILURE;
    }

    UInt32 max_frames = kMaxFramesPerSlice;
    if (!check(AudioUnitSetProperty(rec.unit, kAudioUnitProperty_MaximumFramesPerSlice,
                                    kAudioUnitScope_Global, kOutputElement, &max_frames,
                                    sizeof(max_frames)),
               "set max frames per slice")) {
        return EXIT_FAILURE;
    }

    const AURenderCallbackStruct callback{.inputProc = on_input, .inputProcRefCon = &rec};
    if (!check(AudioUnitSetProperty(rec.unit, kAudioOutputUnitProperty_SetInputCallback,
                                    kAudioUnitScope_Global, kOutputElement, &callback,
                                    sizeof(callback)),
               "set input callback")) {
        return EXIT_FAILURE;
    }

    if (!check(AudioUnitInitialize(rec.unit), "AudioUnitInitialize") ||
        !check(AudioOutputUnitStart(rec.unit), "AudioOutputUnitStart")) {
        return EXIT_FAILURE;
    }

    std::cout << "recording to " << path << " (ctrl-c to stop)\n";
    while (!g_quit.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    AudioOutputUnitStop(rec.unit);
    AudioUnitUninitialize(rec.unit);
    AudioComponentInstanceDispose(rec.unit);

    std::fclose(rec.file);
    std::cout << "wrote " << rec.bytes << " bytes\n";
    return EXIT_SUCCESS;
}
