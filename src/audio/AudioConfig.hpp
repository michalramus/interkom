#pragma once

#include <cstddef>
#include <cstdint>

struct AudioConfig {
    std::uint32_t sampleRate = 48000;
    std::uint8_t channels = 1;
    std::size_t framesPerChunk = 480; // 10 ms at 48 kHz
};
