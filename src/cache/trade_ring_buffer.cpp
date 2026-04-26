#include "trade_ring_buffer.h"

#include <algorithm>
#include <stdexcept>

namespace mds {

size_t TradeRingBuffer::validate_capacity(size_t capacity) {
    if (capacity == 0) {
        throw std::invalid_argument("TradeRingBuffer capacity must be > 0");
    }
    return capacity;
}

TradeRingBuffer::TradeRingBuffer(size_t capacity)
    : cap_(validate_capacity(capacity)),
      buffer_(cap_) {}

void TradeRingBuffer::push(const Trade& trade) {
    std::lock_guard<std::mutex> lock(mutex_);
    buffer_[head_] = trade;
    head_ = (head_ + 1) % cap_;
    if (count_ < cap_) {
        ++count_;
    }
}

size_t TradeRingBuffer::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
}

std::vector<Trade> TradeRingBuffer::snapshot(size_t max_count) const {
    // 关键：reserve 预分配在锁外，但拷贝 take 条数据要在锁内完成，
    // 否则 push 可能在我们拷贝中途覆盖正读的元素，造成 torn read。
    std::lock_guard<std::mutex> lock(mutex_);

    const size_t take = std::min(max_count, count_);

    std::vector<Trade> out;
    out.reserve(take);

    // 最新一条的下一格就是 head_，所以最近 take 条的起点：
    //   start = (head_ + cap_ - take) % cap_
    const size_t start = (head_ + cap_ - take) % cap_;
    for (size_t i = 0; i < take; ++i) {
        out.push_back(buffer_[(start + i) % cap_]);
    }
    return out;
}

}  // namespace mds
