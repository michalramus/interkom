#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Largest chunk the jitter buffer holds: 20 ms at 48 kHz.
constexpr std::size_t kMaxFrames = 960;

// One RTP packet's worth of mono L16, already in host byte order.
struct Chunk {
    std::uint32_t timestamp = 0;
    std::array<std::int16_t, kMaxFrames> samples{};
};

// RTP timestamps wrap after ~24.8 h at 48 kHz, so ordering is by signed
// distance, never by plain < on the raw value.
constexpr std::int32_t distance(std::uint32_t from, std::uint32_t to) {
    return static_cast<std::int32_t>(to - from);
}

constexpr bool before(std::uint32_t a, std::uint32_t b) {
    return distance(b, a) < 0;
}

struct JitterConfig {
    std::size_t framesPerChunk = 480; // 10 ms at 48 kHz; must match the sender
    std::size_t depthChunks = 2;      // target depth; fixed for StaticDepth
    std::size_t minChunks = 1;        // AdaptiveDepth never goes below...
    std::size_t maxChunks = 10;       // ...or above these; at most 64
    std::size_t inboxChunks = 64;     // add() -> pull() handoff
    std::size_t storeChunks = 64;     // packets waiting for their slot
    std::size_t latenessWindow = 500; // packets in the percentile, 5 s
    std::size_t shrinkWindow = 500;   // pulls between shrink checks, 5 s
    std::size_t resyncChunks = 50;    // further off than this is a new stream;
                                      // this many missing in a row: stopped
};
