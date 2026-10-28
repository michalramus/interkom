#pragma once

#include <CoreAudio/CoreAudio.h>
#include <cstddef>
#include <cstdint>

// Asks the device for one chunk per callback. The buffer size is in device
// frames at the device's own rate, so a 2.5 ms chunk at 16 kHz on a 48 kHz
// device is 120 device frames, not 40. The device may refuse and keep its own.
// Returns the device's nominal sample rate.
Float64 request_chunk_buffer(AudioDeviceID device, std::size_t framesPerChunk,
                          std::uint32_t sampleRate);
