#pragma once

#include "../common/types.h"
#include <cstddef>
#include <mutex>
#include <vector>

namespace mds {

// 线程安全的环形缓冲：保存最近 N 条 Trade，写满后覆盖最旧的。
// 设计场景：SPMC（1 写 N 读）——网络线程单写，策略/监控等多线程并发读快照。
//          实现上 mutex 也能容忍多写，但业务上不应出现多写。
// 性能：mutex 方案，每次操作 ~50ns 锁开销；HFT 极致场景可换 seq-lock，
//      见 EXTENSIONS.md（SPSC 队列语义不同，不适用本场景）。
class TradeRingBuffer {
public:
    // capacity 必须 > 0，传 0 会抛 std::invalid_argument。
    // 非法值兜底由调用方（Config 校验）负责，库本身保持严格。
    explicit TradeRingBuffer(size_t capacity);

    void push(const Trade& trade);

    size_t size() const;
    size_t capacity() const { return cap_; }  // const 标量，构造后只读，零成本无锁

    // 返回最近 min(max_count, size()) 条 Trade。
    // 顺序：最旧 -> 最新（即按 push 进入顺序）。
    // max_count 大于实际条数时返回全部已存数据。
    std::vector<Trade> snapshot(size_t max_count) const;

private:
    // 把"容量必须 > 0"的校验提到初始化列表前，
    // 让 capacity == 0 永远先抛 invalid_argument，符合公开契约
    static size_t validate_capacity(size_t capacity);

    // 注意成员声明顺序：cap_ 必须在 buffer_ 之前，因为 buffer_ 的初始化依赖 cap_
    mutable std::mutex mutex_;     // 保护 head_ / count_ / buffer_ 内容
    const size_t cap_;             // 容量，构造后不变；用 const 标量固化"只读"语义
    std::vector<Trade> buffer_;    // 大小固定 = cap_
    size_t head_ = 0;              // 下一个写入位置
    size_t count_ = 0;             // 当前已存条数
};

}  // namespace mds
