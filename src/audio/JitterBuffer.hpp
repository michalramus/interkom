#pragma once

#include <cstddef>
#include <cstdint>

// Interface only; the design is still open.
//
// Intended threading:
// - RTPReceiver's callback wakes the jitter buffer thread, which calls add().
// - Clock wakes the jitter buffer thread to prepare() the next chunk.
// - Clock also wakes the consumers (RTP senders on a mixer, speakers on a
//   beltpack), which check isReady() and get() the chunk.
class JitterBuffer {
  public:
    // Inserts audio ordered by RTP timestamp. Returns false if it arrived too
    // late to be played.
    bool add(std::uint32_t timestamp, const std::int16_t* samples, std::size_t frames);

    // Prepares the chunk that plays at timestamp.
    void prepare(std::uint32_t timestamp);

    bool isReady() const;

    // Copies up to frames samples of the prepared chunk into out; returns how
    // many were copied.
    std::size_t get(std::int16_t* out, std::size_t frames);
};
