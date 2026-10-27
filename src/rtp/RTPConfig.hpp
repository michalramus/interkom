#pragma once

#include <cstdint>
#include <string>

struct RTPConfig {
    std::string address = "127.0.0.1"; // sender: destination, receiver: where to bind
    std::uint16_t port = 5004;
    std::uint32_t sampleRate = 48000;
};
