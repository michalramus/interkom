#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Largest chunk the jitter buffer holds: 20 ms at 48 kHz, 60 ms at 16 kHz.
constexpr std::size_t kMaxFrames = 960;

// One RTP packet's worth of mono L16, already in host byte order.
struct Chunk {
    std::uint32_t timestamp = 0;
    std::array<std::int16_t, kMaxFrames> samples{};
};

// RTP timestamps wrap after ~74.6 h at 16 kHz, so ordering is by signed
// distance, never by plain < on the raw value.
constexpr std::int32_t distance(std::uint32_t from, std::uint32_t to) {
    return static_cast<std::int32_t>(to - from);
}

constexpr bool before(std::uint32_t a, std::uint32_t b) {
    return distance(b, a) < 0;
}

struct JitterConfig {
    // Every count below is in chunks; the times are for 2.5 ms chunks.
    std::size_t framesPerChunk = 40;   // 2.5 ms at 16 kHz; must match the sender
    std::size_t depthChunks = 4;       // target depth, 10 ms; fixed for StaticDepth
    std::size_t minChunks = 1;         // AdaptiveDepth never goes below 2.5 ms...
    std::size_t maxChunks = 40;        // ...or above 100 ms; at most 64
    std::size_t inboxChunks = 256;     // add() -> pull() handoff, 640 ms
    std::size_t storeChunks = 256;     // packets waiting for their slot, 640 ms
    std::size_t latenessWindow = 2000; // packets in the percentile, 5 s
    std::size_t shrinkWindow = 2000;   // pulls between shrink checks, 5 s
    std::size_t resyncChunks = 200;    // further off than this (500 ms) is a new
                                       // stream; this many missing in a row: stopped
    std::size_t fadeChunks = 12;       // Plc fades a lost chunk out over 30 ms
};
