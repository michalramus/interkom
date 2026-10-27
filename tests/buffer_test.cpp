// Input's per-tick cache, the message player and the mixer.

#undef NDEBUG // these asserts are the test, keep them in release builds too
#include "buffer/CommunicateBuffer.hpp"
#include "buffer/Input.hpp"

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

} // namespace

int main() {
    cachedPerTick();
    communicate();
    std::cout << "buffers: all passed\n";
    return 0;
}
