#pragma once

#include "buffer/JitterTypes.hpp"

#include <cstddef>
#include <queue>
#include <vector>

// Packets waiting for their playout slot, oldest timestamp on top. Touched only
// by the consumer thread. The vector is reserved up front and a heap never
// shrinks it, so push() and pop() do not allocate.
class HeapStore {
  public:
    explicit HeapStore(const JitterConfig& config);

    // Returns false when full; the caller counts the drop.
    bool push(const Chunk& chunk);

    bool empty() const {
        return heap_.empty();
    }

    std::size_t size() const {
        return heap_.size();
    }

    const Chunk& top() const {
        return heap_.top();
    }

    void pop() {
        heap_.pop();
    }

  private:
    // priority_queue keeps the *largest* on top, so "larger" means later.
    struct Later {
        bool operator()(const Chunk& a, const Chunk& b) const;
    };

    using Heap = std::priority_queue<Chunk, std::vector<Chunk>, Later>;

    std::size_t capacity_;
    Heap heap_;
};
