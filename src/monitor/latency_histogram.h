#pragma once

#include <array>
#include <atomic>
#include <climits>
#include <cstdint>

namespace mds {

// 指数桶延迟直方图。线程安全（atomic + relaxed），无锁。
//
// 工作原理：
//   bucket[i] 覆盖 [2^i, 2^(i+1)) us
//   record(lat_us) 用 __builtin_clzll 直接算桶下标，O(1)
//   上报线程可按周期 snapshot_and_reset 拿分位数 + 清零
//
// 精度：每档 2× 范围，p99 估计有 ±2× 误差。生产级用 HDR Histogram
// （精度 ±0.001%）或 t-digest，实现量大，第一版指数桶足够，能区分
// 100us / 1ms / 10ms / 100ms 这些数量级足以发现"今天系统延迟比昨天差一档"。
//
// 跟 std::atomic<uint64_t>[] 比，没用 std::vector：避免动态分配 +
// false sharing 问题。固定 64 桶，全部塞在一个对象里，连续内存。
class LatencyHistogram {
public:
    // 64 个桶足够覆盖 1us ~ 2^63 us（接近 30 万年），永远用不完
    static constexpr int kBuckets = 64;

    LatencyHistogram() {
        for (auto& b : buckets_) b.store(0, std::memory_order_relaxed);
    }
    LatencyHistogram(const LatencyHistogram&) = delete;
    LatencyHistogram& operator=(const LatencyHistogram&) = delete;

    // 记录一条延迟。调用者保证 lat_us 是非负值（负值是时钟回拨/口径错误，
    // 应在调用方 filter 掉，本类不再校验，避免 hot path 多一次比较）。
    void record(uint64_t lat_us) {
        // bucket = floor(log2(lat_us))；lat_us == 0 归入桶 0。
        // __builtin_clzll(x) 返回 64 位整数前导零数量，63 - clzll 即为最高有效位下标。
        // 对 0 调 clzll 是 UB，所以单独判一下。
        const int idx = (lat_us == 0) ? 0 : (63 - __builtin_clzll(lat_us));
        buckets_[idx].fetch_add(1, std::memory_order_relaxed);

        // CAS loop 更新 max。relaxed 即可——这里不参与任何同步关系，只是统计。
        // compare_exchange_weak 在偶发失败时会自动重试，不会"丢"更新；
        // 失败时 cur 已被刷成最新值，下一轮直接用最新值再比。
        uint64_t cur = max_us_.load(std::memory_order_relaxed);
        while (lat_us > cur &&
               !max_us_.compare_exchange_weak(cur, lat_us,
                                              std::memory_order_relaxed)) {
        }
    }

    struct Snapshot {
        uint64_t count   = 0;
        uint64_t p50_us  = 0;
        uint64_t p95_us  = 0;
        uint64_t p99_us  = 0;
        uint64_t p999_us = 0;  // P99.9
        uint64_t max_us  = 0;
    };

    // 取一次快照并清零所有计数。线程安全但不是"原子一致"——快照过程中
    // 新到的 record 可能落在新桶里、也可能落进旧桶里，会有微小漂移。
    // 监控用途下完全可以接受（每秒采样几千条样本，几条飘忽不影响分位数）。
    Snapshot snapshot_and_reset() {
        std::array<uint64_t, kBuckets> local{};
        uint64_t total = 0;
        for (int i = 0; i < kBuckets; ++i) {
            local[i] = buckets_[i].exchange(0, std::memory_order_relaxed);
            total += local[i];
        }
        Snapshot s;
        s.count = total;
        s.max_us = max_us_.exchange(0, std::memory_order_relaxed);
        if (total == 0) return s;

        s.p50_us = percentile(local, total, 50);
        s.p95_us = percentile(local, total, 95);
        s.p99_us = percentile(local, total, 99);
        // 999‰ = 99.9%
        s.p999_us = percentile_permille(local, total, 999);
        return s;
    }

private:
    // 按累计计数定位分位数，返回桶上界（保守估计）。
    // target 用 ceil(total * percent / 100)：保证 100 percentile 命中最后一个桶。
    static uint64_t percentile(const std::array<uint64_t, kBuckets>& buckets,
                               uint64_t total, int percent) {
        const uint64_t target = (total * static_cast<uint64_t>(percent) + 99) / 100;
        return percentile_target(buckets, target);
    }

    // permille：千分位，999 = P99.9
    static uint64_t percentile_permille(const std::array<uint64_t, kBuckets>& buckets,
                                        uint64_t total, uint64_t permille) {
        const uint64_t target = (total * permille + 999) / 1000;
        return percentile_target(buckets, target);
    }

    static uint64_t percentile_target(const std::array<uint64_t, kBuckets>& buckets,
                                      uint64_t target) {
        uint64_t cumulative = 0;
        for (int i = 0; i < kBuckets; ++i) {
            cumulative += buckets[i];
            if (cumulative >= target) {
                // 桶 i 覆盖 [2^i, 2^(i+1))，用上界做估计
                return (i >= 63) ? ULLONG_MAX : (1ULL << (i + 1));
            }
        }
        return 0;  // 不可达：total > 0 时 cumulative 必然到达 target
    }

    std::array<std::atomic<uint64_t>, kBuckets> buckets_;
    std::atomic<uint64_t> max_us_{0};
};

}  // namespace mds
