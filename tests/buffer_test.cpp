// Input's per-tick cache, the message player, the mixer and mix-minus.

#include "buffer/Input.hpp"
#include "buffer/mix/Mixer.hpp"
#include "buffer/mix/Subtractor.hpp"
#include "buffer/source/CommunicateBuffer.hpp"
#include "check.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using Samples = std::vector<std::int16_t>;

// Fills chunk n (from 0) with (n + 1) * step and counts how often it produced.
class Counter final : public Input {
  public:
    explicit Counter(std::size_t frames, std::int16_t step = 1)
        : Input(frames), step_(step), out_(frames) {}

    int produced = 0;

  protected:
    const std::int16_t* produce() override {
        ++produced;
        std::fill(out_.begin(), out_.end(), static_cast<std::int16_t>(produced * step_));
        return out_.data();
    }

  private:
    std::int16_t step_;
    Samples out_;
};

Samples pull(Input& input, std::uint64_t tick) {
    const std::int16_t* chunk = input.pull(tick);
    return {chunk, chunk + input.frames()};
}

bool silent(const Samples& samples) {
    return std::all_of(samples.begin(), samples.end(), [](std::int16_t s) { return s == 0; });
}

void cachedPerTick() {
    Counter input{4};
    CHECK(pull(input, 0)[0] == 1);
    CHECK(pull(input, 0)[0] == 1); // same tick: cached, not produced again
    CHECK(pull(input, 1)[0] == 2);
    CHECK(input.produced == 2);
    CHECK(input.pull(2) == input.pull(2)); // the same memory, not a copy
}

// 8 kHz, 1000 frames per chunk: a message is 8 chunks.
void communicate() {
    CommunicateBuffer messages{8000, 1000};
    std::uint64_t tick = 0;
    CHECK(silent(pull(messages, tick++)));

    messages.select(1);
    for (int i = 0; i < 8; ++i) {
        CHECK(!silent(pull(messages, tick++)));
    }
    CHECK(silent(pull(messages, tick++))); // ended, back to 0

    // Reselecting mid-message restarts it: 8 more chunks, not the 5 left.
    messages.select(2);
    for (int i = 0; i < 3; ++i) {
        pull(messages, tick++);
    }
    messages.select(2);
    for (int i = 0; i < 8; ++i) {
        CHECK(!silent(pull(messages, tick++)));
    }
    CHECK(silent(pull(messages, tick++)));

    messages.select(1);
    pull(messages, tick++);
    messages.select(0); // stop
    CHECK(silent(pull(messages, tick++)));
}

// 8 kHz, 3000 frames per chunk: two whole chunks, then 2000 samples padded.
void communicateTail() {
    CommunicateBuffer messages{8000, 3000};
    messages.select(1);
    CHECK(!silent(pull(messages, 0)));
    CHECK(!silent(pull(messages, 1)));
    const Samples tail = pull(messages, 2);
    CHECK(!silent({tail.begin(), tail.begin() + 2000}));
    CHECK(silent({tail.begin() + 2000, tail.end()}));
    CHECK(silent(pull(messages, 3)));
}

void mixes() {
    Counter a{4};
    Counter b{4, 10};
    Mixer mixer{4};
    std::uint64_t tick = 0;
    CHECK(silent(pull(mixer, tick++))); // no inputs yet

    CHECK(mixer.addInput(&a) && mixer.addInput(&b));
    CHECK(pull(mixer, tick++)[0] == 1 + 10);
    CHECK(pull(mixer, tick++)[0] == 2 + 20);

    CHECK(mixer.removeInput(&b));
    CHECK(pull(mixer, tick++)[0] == 3);
    CHECK(b.produced == 2); // no longer pulled
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
    CHECK(pull(up, 0)[0] == 32767);
    CHECK(pull(down, 0)[0] == -32768);
    CHECK(loud.produced == 1);
}

// Mix-minus for three participants: one mixer of everyone, one subtractor per
// listener. Each input is consumed once per tick however many pull it.
void mixMinus() {
    Counter a{4, 1};
    Counter b{4, 10};
    Counter c{4, 100};
    Mixer all{4};
    all.addInput(&a);
    all.addInput(&b);
    all.addInput(&c);
    Subtractor forA{&all, &a};
    Subtractor forB{&all, &b};
    Subtractor forC{&all, &c};

    for (std::uint64_t tick = 0; tick < 3; ++tick) {
        const auto n = static_cast<std::int16_t>(tick + 1);
        CHECK(pull(forA, tick)[0] == 10 * n + 100 * n);
        CHECK(pull(forB, tick)[0] == 1 * n + 100 * n);
        CHECK(pull(forC, tick)[0] == 1 * n + 10 * n);
    }
    CHECK(a.produced == 3 && b.produced == 3 && c.produced == 3);
}

// The full mix clips at 32767, but each listener's mix-minus does not.
void mixMinusExactWhenSumClips() {
    Counter a{4, 32000};
    Counter b{4, 2000};
    Mixer all{4};
    all.addInput(&a);
    all.addInput(&b);
    Subtractor forA{&all, &a};
    Subtractor forB{&all, &b};
    CHECK(pull(all, 0)[0] == 32767);
    CHECK(pull(forA, 0)[0] == 2000);
    CHECK(pull(forB, 0)[0] == 32000);
}

} // namespace

int main() {
    cachedPerTick();
    communicate();
    communicateTail();
    mixes();
    saturates();
    mixMinus();
    mixMinusExactWhenSumClips();
    std::cout << "buffers: all passed\n";
    return 0;
}
