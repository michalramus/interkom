#pragma once

#include "buffer/DepthPolicy.hpp"
#include "buffer/HeapStore.hpp"
#include "buffer/JitterTypes.hpp"
#include "buffer/RepeatFadePlc.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <vector>

// Turns packets that arrive whenever the network delivers them into exactly one
// chunk per pull, on time, forever.
//
// Threading: add() runs on the RTP receive thread and only copies into a
// lock-free single-producer single-consumer inbox. pull() runs on the consumer
// (a speaker's render callback, or the mixer thread woken by a Clock) and does
// everything else. pull() never waits: whatever is not in its slot by then is
// concealed, and a deeper buffer, not a wait, is what buys more time.
//
// Each part is swappable:
//   Store  orders packets by timestamp     (HeapStore: std::priority_queue)
//   Plc    fills a missing chunk           (RepeatFadePlc)
//   Depth  decides how much to buffer      (AdaptiveDepth, StaticDepth)
//
// One stream (one SSRC) per buffer; a mixer keeps one per participant.
template <class Store = HeapStore, class Plc = RepeatFadePlc, class Depth = AdaptiveDepth>
class JitterBuffer {
  public:
    struct Stats {
        std::uint64_t received = 0;
        std::uint64_t late = 0;       // arrived after its slot, discarded
        std::uint64_t duplicates = 0; // slot already played from real data
        std::uint64_t concealed = 0;  // slots filled by Plc
        std::uint64_t dropped = 0;    // inbox or store full
        std::uint64_t grows = 0;      // cursor rewound to a late packet
        std::uint64_t shrinks = 0;    // chunks dropped to cut latency
        std::uint64_t resyncs = 0;    // timestamp jumped: new stream
        std::uint64_t depth = 0;      // chunks queued after the last pull
    };

    explicit JitterBuffer(const JitterConfig& config = {})
        : config_(config), inbox_(config.inboxChunks + 1), store_(config_), plc_(config_),
          depth_(config_) {}

    // RTP receive thread. Never blocks; drops the packet if the inbox is full.
    void add(std::uint32_t timestamp, const std::int16_t* samples, std::size_t frames) {
        const std::size_t w = write_.load(std::memory_order_relaxed);
        const std::size_t next = (w + 1) % inbox_.size();
        if (next == read_.load(std::memory_order_acquire)) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        Chunk& chunk = inbox_[w];
        chunk.timestamp = timestamp;
        const std::size_t count = std::min(frames, config_.framesPerChunk);
        std::copy_n(samples, count, chunk.samples.begin());
        std::fill(chunk.samples.begin() + static_cast<std::ptrdiff_t>(count),
                  chunk.samples.begin() + static_cast<std::ptrdiff_t>(config_.framesPerChunk),
                  std::int16_t{0});
        write_.store(next, std::memory_order_release);
    }

    // Consumer thread. Writes exactly framesPerChunk samples to out.
    void pull(std::int16_t* out) {
        drain();
        const std::size_t frames = config_.framesPerChunk;
        if (!started_) {
            std::fill_n(out, frames, std::int16_t{0});
            return;
        }

        while (!store_.empty() && before(store_.top().timestamp, cursor_)) {
            store_.pop(); // a second copy of a slot that has been played
            duplicates_.fetch_add(1, std::memory_order_relaxed);
        }
        if (!store_.empty() && store_.top().timestamp == cursor_) {
            std::copy_n(store_.top().samples.begin(), frames, out);
            plc_.good(out, frames);
            store_.pop();
            played_ = (played_ << 1U) | 1U;
            playing_ = true;
        } else {
            plc_.conceal(out, frames);
            played_ <<= 1U;
            if (playing_) { // silence while priming is not a loss
                concealed_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        cursor_ += static_cast<std::uint32_t>(frames);

        if (const std::size_t chunks = depth_.shrink(store_.size()); chunks > 0) {
            drop(chunks);
        }
        depthChunks_.store(store_.size(), std::memory_order_relaxed);
    }

    // Any thread.
    Stats stats() const {
        constexpr auto relaxed = std::memory_order_relaxed;
        return {received_.load(relaxed),   late_.load(relaxed),    duplicates_.load(relaxed),
                concealed_.load(relaxed),  dropped_.load(relaxed), grows_.load(relaxed),
                shrinks_.load(relaxed),    resyncs_.load(relaxed), depthChunks_.load(relaxed)};
    }

  private:
    // played_ remembers this many slots behind the cursor.
    static constexpr std::size_t kHistory = 64;

    void drain() {
        std::size_t r = read_.load(std::memory_order_relaxed);
        while (r != write_.load(std::memory_order_acquire)) {
            accept(inbox_[r]);
            r = (r + 1) % inbox_.size();
            read_.store(r, std::memory_order_release);
        }
    }

    void accept(const Chunk& chunk) {
        received_.fetch_add(1, std::memory_order_relaxed);
        const auto frames = static_cast<std::int32_t>(config_.framesPerChunk);
        const auto resync = static_cast<std::int32_t>(config_.resyncChunks) * frames;

        if (started_ && std::abs(distance(cursor_, chunk.timestamp)) > resync) {
            restart(); // sender restarted or jumped; this is not jitter
            resyncs_.fetch_add(1, std::memory_order_relaxed);
        }
        if (!started_) {
            // Start depthChunks behind the first packet: that is the priming.
            cursor_ = chunk.timestamp -
                      static_cast<std::uint32_t>(config_.depthChunks * config_.framesPerChunk);
            started_ = true;
        }

        const std::int32_t ahead = distance(cursor_, chunk.timestamp);
        if (ahead >= 0) {
            depth_.record(-(ahead / frames));
            store(chunk);
            return;
        }

        const auto chunks = static_cast<std::size_t>((-ahead + frames - 1) / frames);
        if (chunks <= kHistory && ((played_ >> (chunks - 1)) & 1U) != 0) {
            duplicates_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        depth_.record(static_cast<std::int32_t>(chunks));
        if (chunks <= kHistory && depth_.grow(chunks, store_.size())) {
            // Step back to the late packet's slot; what was concealed there
            // becomes the extra delay.
            cursor_ -= static_cast<std::uint32_t>(chunks * config_.framesPerChunk);
            played_ >>= chunks;
            grows_.fetch_add(1, std::memory_order_relaxed);
            store(chunk);
            return;
        }
        late_.fetch_add(1, std::memory_order_relaxed);
    }

    void store(const Chunk& chunk) {
        if (!store_.push(chunk)) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Skip chunks ahead of the cursor. They count as played, so a straggler
    // for one of them is a duplicate rather than a reason to rewind.
    void drop(std::size_t chunks) {
        cursor_ += static_cast<std::uint32_t>(chunks * config_.framesPerChunk);
        played_ = chunks >= kHistory ? ~std::uint64_t{0}
                                     : (played_ << chunks) | ((std::uint64_t{1} << chunks) - 1);
        while (!store_.empty() && before(store_.top().timestamp, cursor_)) {
            store_.pop();
        }
        shrinks_.fetch_add(chunks, std::memory_order_relaxed);
    }

    void restart() {
        while (!store_.empty()) {
            store_.pop();
        }
        started_ = false;
        playing_ = false;
        played_ = 0;
    }

    JitterConfig config_;

    // Single-producer single-consumer: add() writes, pull() reads.
    std::vector<Chunk> inbox_;
    std::atomic<std::size_t> read_{0};
    std::atomic<std::size_t> write_{0};

    // Consumer thread only.
    Store store_;
    Plc plc_;
    Depth depth_;
    std::uint32_t cursor_ = 0; // timestamp of the next chunk pull() plays
    bool started_ = false;
    bool playing_ = false;
    std::uint64_t played_ = 0; // bit i: slot i+1 chunks back was real audio

    // Counters: only read for display. dropped_ is bumped from both threads.
    std::atomic<std::uint64_t> received_{0};
    std::atomic<std::uint64_t> late_{0};
    std::atomic<std::uint64_t> duplicates_{0};
    std::atomic<std::uint64_t> concealed_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> grows_{0};
    std::atomic<std::uint64_t> shrinks_{0};
    std::atomic<std::uint64_t> resyncs_{0};
    std::atomic<std::size_t> depthChunks_{0};
};
