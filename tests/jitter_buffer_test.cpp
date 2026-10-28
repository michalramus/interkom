// Jitter buffer behaviour, driven one pull at a time. Chunks are 4 frames so
// the arithmetic stays readable; packet n carries the value (n + 1) * 100.

#include "buffer/jitter/JitterBuffer.hpp"
#include "check.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>

namespace {

constexpr std::size_t kFrames = 4;

using Static = JitterBuffer<HeapStore, RepeatFadePlc, StaticDepth>;
using Adaptive = JitterBuffer<HeapStore, RepeatFadePlc, AdaptiveDepth>;
using Samples = std::array<std::int16_t, kFrames>;

JitterConfig config(std::size_t depth) {
    JitterConfig c;
    c.framesPerChunk = kFrames;
    c.depthChunks = depth;
    c.minChunks = 1;
    c.maxChunks = 8;
    c.inboxChunks = 16;
    c.storeChunks = 16;
    c.latenessWindow = 10;
    c.shrinkWindow = 10;
    return c;
}

std::int16_t value(int packet) {
    return static_cast<std::int16_t>((packet + 1) * 100);
}

template <class Buffer>
void add(Buffer& jb, std::uint32_t base, int packet) {
    Samples samples;
    samples.fill(value(packet));
    jb.add(base + static_cast<std::uint32_t>(packet * static_cast<int>(kFrames)), samples.data(),
           kFrames);
}

// Every call is a new tick.
Samples pull(Input& jb) {
    static std::uint64_t tick = 0;
    Samples out{};
    std::copy_n(jb.pull(tick++), kFrames, out.begin());
    return out;
}

// Depth 2: pull i plays packet i - 2.
void inOrder(std::uint32_t base) {
    Static jb{config(2)};
    for (int i = 0; i < 20; ++i) {
        add(jb, base, i);
        const Samples out = pull(jb);
        CHECK(out[0] == (i >= 2 ? value(i - 2) : 0));
    }
    const auto stats = jb.stats();
    CHECK(stats.received == 20 && stats.concealed == 0 && stats.late == 0);
    CHECK(stats.depth == 2);
}

void reorder(std::uint32_t base) {
    Static jb{config(2)};
    for (int i = 0; i < 21; ++i) {
        // 0, 2, 1, 3, 5, 4, ...: every third pair swapped.
        const int packet = (i % 3 == 1) ? i + 1 : (i % 3 == 2) ? i - 1 : i;
        add(jb, base, packet);
        const Samples out = pull(jb);
        CHECK(out[0] == (i >= 2 ? value(i - 2) : 0));
    }
    CHECK(jb.stats().concealed == 0);
}

void lossIsConcealed() {
    Static jb{config(2)};
    for (int i = 0; i < 10; ++i) {
        if (i != 5) {
            add(jb, 0, i);
        }
        const Samples out = pull(jb);
        if (i == 7) {
            // Packet 4 repeated, fading.
            CHECK(out[0] == value(4));
            CHECK(out[kFrames - 1] < value(4) && out[kFrames - 1] > 0);
        } else if (i >= 2) {
            CHECK(out[0] == value(i - 2));
        }
    }
    CHECK(jb.stats().concealed == 1);
}

// Depth 1: pull i plays packet i - 1. Packet 5 shows up two pulls late, after
// its slot was concealed.
// minChunks 0: nothing holds, so the slot really is concealed and the late
// packet is what moves the depth.
template <class Buffer>
auto lateOnce(std::array<std::int16_t, 12>& played) {
    JitterConfig c = config(1);
    c.minChunks = 0;
    Buffer jb{c};
    for (int i = 0; i < 12; ++i) {
        if (i == 7) {
            add(jb, 0, 5);
        }
        if (i != 5) {
            add(jb, 0, i);
        }
        played[static_cast<std::size_t>(i)] = pull(jb)[0];
    }
    return jb.stats();
}

void lateIsDiscardedByStatic() {
    std::array<std::int16_t, 12> played{};
    const auto stats = lateOnce<Static>(played);
    CHECK(stats.late == 1 && stats.grows == 0 && stats.concealed == 1);
    CHECK(played[7] == value(6)); // slot moved on without it
}

void lateRewindsAdaptive() {
    std::array<std::int16_t, 12> played{};
    const auto stats = lateOnce<Adaptive>(played);
    CHECK(stats.late == 0 && stats.grows == 1 && stats.concealed == 1);
    // Pull 6 concealed packet 5's slot; pull 7 rewinds and plays it, and from
    // there on everything is one chunk later.
    CHECK(played[6] == value(4));
    for (std::size_t i = 7; i < played.size(); ++i) {
        CHECK(played[i] == value(static_cast<int>(i) - 2));
    }
    CHECK(stats.depth == 2);
}

void shrinksToMinimum() {
    Adaptive jb{config(4)};
    std::int16_t last = 0;
    for (int i = 0; i < 100; ++i) {
        add(jb, 0, i);
        const std::int16_t now = pull(jb)[0];
        CHECK(now == 0 || now > last); // drops skip ahead, never repeat
        last = now == 0 ? last : now;
    }
    const auto stats = jb.stats();
    CHECK(stats.shrinks == 3 && stats.depth == 1 && stats.concealed == 0);
}

void duplicates() {
    Static jb{config(2)};
    for (int i = 0; i < 10; ++i) {
        add(jb, 0, i);
        if (i == 3) {
            add(jb, 0, 3); // still queued
        }
        if (i == 6) {
            add(jb, 0, 1); // played at pull 3
        }
        const Samples out = pull(jb);
        CHECK(out[0] == (i >= 2 ? value(i - 2) : 0));
    }
    const auto stats = jb.stats();
    CHECK(stats.duplicates == 2 && stats.late == 0 && stats.concealed == 0);
}

void resync() {
    Static jb{config(2)};
    for (int i = 0; i < 10; ++i) {
        add(jb, 0, i);
        pull(jb);
    }
    // Sender restarted with a new random timestamp.
    for (int i = 0; i < 10; ++i) {
        add(jb, 1'000'000, i);
        const Samples out = pull(jb);
        if (i >= 2) {
            CHECK(out[0] == value(i - 2));
        }
    }
    CHECK(jb.stats().resyncs == 1);
}

// The consumer starts late: ten packets wait before the first pull. Playout
// anchors on the newest, so the backlog costs no latency.
template <class Buffer>
void startupBacklog() {
    Buffer jb{config(2)};
    for (int i = 0; i < 10; ++i) {
        add(jb, 0, i);
    }
    CHECK(pull(jb)[0] == value(7));
    auto stats = jb.stats();
    CHECK(stats.depth == 2 && stats.flushed == 7 && stats.shrinks == 0);
    for (int i = 10; i < 20; ++i) {
        add(jb, 0, i);
        CHECK(pull(jb)[0] == value(i - 2));
    }
    stats = jb.stats();
    CHECK(stats.depth == 2 && stats.concealed == 0);
}

// A burst past maxChunks goes back to the target depth, not to maxChunks.
void burstTrimsToTarget() {
    Adaptive jb{config(2)};
    for (int i = 0; i < 5; ++i) {
        add(jb, 0, i);
        pull(jb);
    }
    for (int i = 5; i < 15; ++i) { // ten at once: 12 queued > maxChunks 8
        add(jb, 0, i);
    }
    CHECK(pull(jb)[0] == value(3));
    const auto stats = jb.stats();
    CHECK(stats.depth == 2 && stats.shrinks == 9);
}

// Same late packet with minChunks 1: the buffer refuses to play its last
// queued chunk, holds a stand-in instead, and packet 5 is then on time.
void lowQueueHoldsAdaptive() {
    std::array<std::int16_t, 12> played{};
    Adaptive jb{config(1)};
    for (int i = 0; i < 12; ++i) {
        if (i == 7) {
            add(jb, 0, 5);
        }
        if (i != 5) {
            add(jb, 0, i);
        }
        played[static_cast<std::size_t>(i)] = pull(jb)[0];
        if (i >= 1) {
            CHECK(jb.stats().depth >= 1);
        }
    }
    const auto stats = jb.stats();
    CHECK(stats.late == 0 && stats.concealed == 0 && stats.grows == 1);
    CHECK(played[5] == value(3)); // stand-in: packet 3 repeated
    for (std::size_t i = 6; i < played.size(); ++i) {
        CHECK(played[i] == value(static_cast<int>(i) - 2));
    }
}

// Packets arrive one per tick, the consumer pulls four at a time (a sound
// server asking for a whole period). The depth grows until a burst fits, then
// the queue never empties and every packet is played in order.
void burstyConsumerHolds() {
    Adaptive jb{config(2)};
    std::int16_t last = 0;
    int packet = 0;
    for (int period = 0; period < 30; ++period) {
        for (int i = 0; i < 4; ++i) {
            add(jb, 0, packet++);
        }
        for (int i = 0; i < 4; ++i) {
            const std::int16_t now = pull(jb)[0];
            if (period >= 5) {
                CHECK(jb.stats().depth >= 1);
                CHECK(now == last + 100); // real audio, no gaps, no repeats
            }
            last = now;
        }
    }
    const auto stats = jb.stats();
    CHECK(stats.late == 0 && stats.concealed == 0 && stats.shrinks == 0);
}

// The sender stops: after resyncChunks concealed chunks the buffer goes idle and
// stops counting losses. When it comes back it anchors on the newest packet.
void idleThenReturn() {
    JitterConfig c = config(2);
    c.resyncChunks = 5;
    Static jb{c};
    for (int i = 0; i < 10; ++i) {
        add(jb, 0, i);
        pull(jb);
    }
    for (int i = 0; i < 20; ++i) { // 2 queued, 5 concealed, 13 idle
        pull(jb);
    }
    auto stats = jb.stats();
    CHECK(stats.concealed == 5 && stats.idle == 13);

    for (int i = 40; i < 44; ++i) { // back, after a gap
        add(jb, 0, i);
    }
    CHECK(pull(jb)[0] == value(41));
    for (int i = 44; i < 50; ++i) {
        add(jb, 0, i);
        CHECK(pull(jb)[0] == value(i - 2));
    }
    stats = jb.stats();
    CHECK(stats.concealed == 5 && stats.idle == 13 && stats.depth == 2);
}

// Holding is bounded: a sender that stops still drains and goes idle.
void heldStreamStillGoesIdle() {
    JitterConfig c = config(2);
    c.resyncChunks = 5;
    Adaptive jb{c};
    for (int i = 0; i < 10; ++i) {
        add(jb, 0, i);
        pull(jb);
    }
    for (int i = 0; i < 30; ++i) { // 1 played, 8 held, 1 played, 5 concealed
        pull(jb);
    }
    const auto stats = jb.stats();
    CHECK(stats.grows == 8 && stats.concealed == 5 && stats.idle == 15);
}

} // namespace

int main() {
    inOrder(0);
    inOrder(0xFFFF'FFF0); // timestamp wraps after four packets
    reorder(0);
    reorder(0xFFFF'FFF0);
    lossIsConcealed();
    lateIsDiscardedByStatic();
    lateRewindsAdaptive();
    lowQueueHoldsAdaptive();
    burstyConsumerHolds();
    shrinksToMinimum();
    duplicates();
    resync();
    startupBacklog<Static>();
    startupBacklog<Adaptive>();
    burstTrimsToTarget();
    idleThenReturn();
    heldStreamStillGoesIdle();
    std::cout << "jitter buffer: all passed\n";
    return 0;
}
