#pragma once

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace mds {

// 线程安全的固定容量环形缓冲，保存最近 N 条数据，写满后覆盖最旧元素。
// 设计场景：行情回调线程写入，策略/监控线程读取快照。
template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity)
        : cap_(validate_capacity(capacity)),
          buffer_(cap_) {}

    void push(const T& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        buffer_[head_] = item;
        head_ = (head_ + 1) % cap_;
        if (count_ < cap_) {
            ++count_;
        }
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_;
    }

    size_t capacity() const { return cap_; }

    // 返回最近 min(max_count, size()) 条数据，顺序为最旧 -> 最新。
    std::vector<T> snapshot(size_t max_count) const {
        std::lock_guard<std::mutex> lock(mutex_);

        const size_t take = std::min(max_count, count_);
        std::vector<T> out;
        out.reserve(take);

        const size_t start = (head_ + cap_ - take) % cap_;
        for (size_t i = 0; i < take; ++i) {
            out.push_back(buffer_[(start + i) % cap_]);
        }
        return out;
    }

private:
    static size_t validate_capacity(size_t capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("RingBuffer capacity must be > 0");
        }
        return capacity;
    }

    mutable std::mutex mutex_;
    const size_t cap_;
    std::vector<T> buffer_;
    size_t head_ = 0;
    size_t count_ = 0;
};

}  // namespace mds
