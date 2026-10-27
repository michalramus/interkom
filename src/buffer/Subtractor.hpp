#pragma once

#include "buffer/Input.hpp"
#include "buffer/Mixer.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

// Mix-minus for one listener: everyone in sum except own. Both are pulled with
// the same tick, so through Input's cache the mixer and own input are each
// produced once per tick however many subtractors share them. The difference
// is taken on the mixer's unclipped sum, so it is exact even when the full mix
// clips. Neither pointer is owned.
class Subtractor final : public Input {
  public:
    Subtractor(Mixer* sum, Input* own);

  protected:
    void produce(std::int16_t* out) override;

  private:
    Mixer* sum_;
    Input* own_;
    std::vector<std::int16_t> chunk_;
};
