// Jitter buffer behaviour, driven one pull at a time. Chunks are 4 frames so
// the arithmetic stays readable; packet n carries the value (n + 1) * 100.

#undef NDEBUG // these asserts are the test, keep them in release builds too
#include "buffer/JitterBuffer.hpp"

#include <array>
#include <cassert>
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

template <class Buffer>
Samples pull(Buffer& jb) {
    Samples out{};
    jb.pull(out.data());
    return out;
}

// Depth 2: pull i plays packet i - 2.
void inOrder(std::uint32_t base) {
    Static jb{config(2)};
    for (int i = 0; i < 20; ++i) {
        add(jb, base, i);
        const Samples out = pull(jb);
        assert(out[0] == (i >= 2 ? value(i - 2) : 0));
    }
    const auto stats = jb.stats();
    assert(stats.received == 20 && stats.concealed == 0 && stats.late == 0);
    assert(stats.depth == 2);
}

void reorder(std::uint32_t base) {
    Static jb{config(2)};
    for (int i = 0; i < 21; ++i) {
        // 0, 2, 1, 3, 5, 4, ...: every third pair swapped.
        const int packet = (i % 3 == 1) ? i + 1 : (i % 3 == 2) ? i - 1 : i;
        add(jb, base, packet);
        const Samples out = pull(jb);
        assert(out[0] == (i >= 2 ? value(i - 2) : 0));
    }
    assert(jb.stats().concealed == 0);
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
            assert(out[0] == value(4));
            assert(out[kFrames - 1] < value(4) && out[kFrames - 1] > 0);
        } else if (i >= 2) {
            assert(out[0] == value(i - 2));
        }
    }
    assert(jb.stats().concealed == 1);
}

// Depth 1: pull i plays packet i - 1. Packet 5 shows up two pulls late, after
// its slot was concealed.
template <class Buffer>
auto lateOnce(std::array<std::int16_t, 12>& played) {
    Buffer jb{config(1)};
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
    assert(stats.late == 1 && stats.grows == 0 && stats.concealed == 1);
    assert(played[7] == value(6)); // slot moved on without it
}

void lateRewindsAdaptive() {
    std::array<std::int16_t, 12> played{};
    const auto stats = lateOnce<Adaptive>(played);
    assert(stats.late == 0 && stats.grows == 1 && stats.concealed == 1);
    // Pull 6 concealed packet 5's slot; pull 7 rewinds and plays it, and from
    // there on everything is one chunk later.
    assert(played[6] == value(4));
    for (std::size_t i = 7; i < played.size(); ++i) {
        assert(played[i] == value(static_cast<int>(i) - 2));
    }
    assert(stats.depth == 2);
}

void shrinksToMinimum() {
    Adaptive jb{config(4)};
    std::int16_t last = 0;
    for (int i = 0; i < 100; ++i) {
        add(jb, 0, i);
        const std::int16_t now = pull(jb)[0];
        assert(now == 0 || now > last); // drops skip ahead, never repeat
        last = now == 0 ? last : now;
    }
    const auto stats = jb.stats();
    assert(stats.shrinks == 3 && stats.depth == 1 && stats.concealed == 0);
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
        assert(out[0] == (i >= 2 ? value(i - 2) : 0));
    }
    const auto stats = jb.stats();
    assert(stats.duplicates == 2 && stats.late == 0 && stats.concealed == 0);
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
            assert(out[0] == value(i - 2));
        }
    }
    assert(jb.stats().resyncs == 1);
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
    shrinksToMinimum();
    duplicates();
    resync();
    std::cout << "jitter buffer: all passed\n";
    return 0;
}
