#include "buffer/HeapStore.hpp"

#include <utility>

HeapStore::HeapStore(const JitterConfig& config) : capacity_(config.storeChunks) {
    std::vector<Chunk> storage;
    storage.reserve(capacity_);
    heap_ = Heap(Later{}, std::move(storage));
}

bool HeapStore::push(const Chunk& chunk) {
    if (heap_.size() == capacity_) {
        return false;
    }
    heap_.push(chunk);
    return true;
}

bool HeapStore::Later::operator()(const Chunk& a, const Chunk& b) const {
    return before(b.timestamp, a.timestamp);
}
