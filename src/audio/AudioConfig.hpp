#pragma once

#include <cstddef>
#include <cstdint>

struct AudioConfig {
    std::uint32_t sampleRate = 16000;
    std::uint8_t channels = 1;
    std::size_t framesPerChunk = 40; // 2.5 ms at 16 kHz
};
