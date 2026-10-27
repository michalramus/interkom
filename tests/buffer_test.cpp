// Input's per-tick cache, the message player and the mixer.

#undef NDEBUG // these asserts are the test, keep them in release builds too
#include "buffer/CommunicateBuffer.hpp"
#include "buffer/Input.hpp"
#include "buffer/Mixer.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using Samples = std::vector<std::int16_t>;

// Fills chunk n (from 0) with (n + 1) * step and counts how often it produced.
class Counter final : public Input {
  public:
    explicit Counter(std::size_t frames, std::int16_t step = 1) : Input(frames), step_(step) {}

    int produced = 0;

  protected:
    void produce(std::int16_t* out) override {
        ++produced;
        std::fill(out, out + frames(), static_cast<std::int16_t>(produced * step_));
    }

  private:
    std::int16_t step_;
};

Samples pull(Input& input, std::uint64_t tick) {
    Samples out(input.frames());
    input.pull(tick, out.data());
    return out;
}

bool silent(const Samples& samples) {
    return std::all_of(samples.begin(), samples.end(), [](std::int16_t s) { return s == 0; });
}

void cachedPerTick() {
    Counter input{4};
    assert(pull(input, 0)[0] == 1);
    assert(pull(input, 0)[0] == 1); // same tick: cached, not produced again
    assert(pull(input, 1)[0] == 2);
    assert(input.produced == 2);
}

// 8 kHz, 1000 frames per chunk: a message is 8 chunks.
void communicate() {
    CommunicateBuffer messages{8000, 1000};
    std::uint64_t tick = 0;
    assert(silent(pull(messages, tick++)));

    messages.select(1);
    for (int i = 0; i < 8; ++i) {
        assert(!silent(pull(messages, tick++)));
    }
    assert(silent(pull(messages, tick++))); // ended, back to 0

    // Reselecting mid-message restarts it: 8 more chunks, not the 5 left.
    messages.select(2);
    for (int i = 0; i < 3; ++i) {
        pull(messages, tick++);
    }
    messages.select(2);
    for (int i = 0; i < 8; ++i) {
        assert(!silent(pull(messages, tick++)));
    }
    assert(silent(pull(messages, tick++)));

    messages.select(1);
    pull(messages, tick++);
    messages.select(0); // stop
    assert(silent(pull(messages, tick++)));
}

void mixes() {
    Counter a{4};
    Counter b{4, 10};
    Mixer mixer{4};
    std::uint64_t tick = 0;
    assert(silent(pull(mixer, tick++))); // no inputs yet

    assert(mixer.addInput(&a) && mixer.addInput(&b));
    assert(pull(mixer, tick++)[0] == 1 + 10);
    assert(pull(mixer, tick++)[0] == 2 + 20);

    assert(mixer.removeInput(&b));
    assert(pull(mixer, tick++)[0] == 3);
    assert(b.produced == 2); // no longer pulled
}

void saturates() {
    Counter loud{4, 30000};
    Counter louder{4, 30000};
    Counter quiet{4, -30000};
    Counter quieter{4, -30000};
    Mixer up{4};
    Mixer down{4};
    up.addInput(&loud);
    up.addInput(&loud); // already in: ignored
    up.addInput(&louder);
    down.addInput(&quiet);
    down.addInput(&quieter);
    assert(pull(up, 0)[0] == 32767);
    assert(pull(down, 0)[0] == -32768);
    assert(loud.produced == 1);
}

// Mix-minus for three participants: each hears the other two. Every jitter
// buffer sits in two mixers but must be consumed once per tick.
void mixMinus() {
    Counter a{4, 1};
    Counter b{4, 10};
    Counter c{4, 100};
    Mixer forA{4};
    Mixer forB{4};
    Mixer forC{4};
    forA.addInput(&b);
    forA.addInput(&c);
    forB.addInput(&a);
    forB.addInput(&c);
    forC.addInput(&a);
    forC.addInput(&b);

    for (std::uint64_t tick = 0; tick < 3; ++tick) {
        const auto n = static_cast<std::int16_t>(tick + 1);
        assert(pull(forA, tick)[0] == 10 * n + 100 * n);
        assert(pull(forB, tick)[0] == 1 * n + 100 * n);
        assert(pull(forC, tick)[0] == 1 * n + 10 * n);
    }
    assert(a.produced == 3 && b.produced == 3 && c.produced == 3);
}

} // namespace

int main() {
    cachedPerTick();
    communicate();
    mixes();
    saturates();
    mixMinus();
    std::cout << "buffers: all passed\n";
    return 0;
}
