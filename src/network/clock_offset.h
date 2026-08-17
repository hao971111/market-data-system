#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

namespace mds {

// 交易所时钟偏移校准（Step 14）。
//
// NTP 中点：offset = server_time - (t_send + t_recv) / 2
// 外部延迟：lat = recv_ts + offset - exchange_ts
//
// 一次样本作废（不改已经采到的 offset）：
//   传输失败 / serverTime 非法 / RTT > 200ms / |offset| > 10s / 本地收发时间倒退
// 从未成功：offset 视为 0，和校准前一样只收 lat >= 0 的样本。
// 曾经成功、后来失败：继续用上一次合格 offset。
//
// 热路径只读 atomics；写只在校准线程。
class ClockOffsetCalibrator {
public:
    static constexpr int64_t kMaxRttUs = 200'000;           // 200ms
    static constexpr int64_t kMaxAbsOffsetUs = 10'000'000;  // 10s；更大视为单位/解析错误

    enum class Reason : uint8_t {
        Accepted,
        TransportFailed,
        BadServerTime,
        RttTooLarge,
        OffsetTooLarge,
        BadLocalTimes,
    };

    struct Result {
        Reason reason = Reason::TransportFailed;
        int64_t rtt_us = 0;
        int64_t sample_offset_us = 0;  // 本次算出来的值，拒绝时也不一定写入
    };

    static const char* reason_name(Reason r) noexcept {
        switch (r) {
            case Reason::Accepted:        return "accepted";
            case Reason::TransportFailed: return "transport_failed";
            case Reason::BadServerTime:   return "bad_server_time";
            case Reason::RttTooLarge:     return "rtt_too_large";
            case Reason::OffsetTooLarge:  return "offset_too_large";
            case Reason::BadLocalTimes:   return "bad_local_times";
        }
        return "unknown";
    }

    Result on_transport_failure() noexcept {
        fail_count_.fetch_add(1, std::memory_order_relaxed);
        return Result{Reason::TransportFailed, 0, 0};
    }

    // local_* 与 server_time 都是 unix 微秒。
    Result on_sample(int64_t local_send_us, int64_t local_recv_us,
                     int64_t server_time_us) noexcept {
        if (server_time_us <= 0) {
            fail_count_.fetch_add(1, std::memory_order_relaxed);
            return Result{Reason::BadServerTime, 0, 0};
        }
        if (local_recv_us < local_send_us) {
            fail_count_.fetch_add(1, std::memory_order_relaxed);
            return Result{Reason::BadLocalTimes, 0, 0};
        }

        const int64_t rtt_us = local_recv_us - local_send_us;
        const int64_t midpoint_us = local_send_us + rtt_us / 2;
        const int64_t offset_us = server_time_us - midpoint_us;
        Result out{Reason::Accepted, rtt_us, offset_us};

        if (rtt_us > kMaxRttUs) {
            fail_count_.fetch_add(1, std::memory_order_relaxed);
            out.reason = Reason::RttTooLarge;
            return out;
        }
        const int64_t abs_off = offset_us < 0 ? -offset_us : offset_us;
        if (abs_off > kMaxAbsOffsetUs) {
            fail_count_.fetch_add(1, std::memory_order_relaxed);
            out.reason = Reason::OffsetTooLarge;
            return out;
        }

        offset_us_.store(offset_us, std::memory_order_relaxed);
        last_rtt_us_.store(rtt_us, std::memory_order_relaxed);
        has_offset_.store(true, std::memory_order_relaxed);
        accept_count_.fetch_add(1, std::memory_order_relaxed);
        return out;
    }

    bool has_offset() const noexcept {
        return has_offset_.load(std::memory_order_relaxed);
    }
    // 从未成功时返回 0（等于未校准）。
    int64_t offset_us() const noexcept {
        return has_offset() ? offset_us_.load(std::memory_order_relaxed) : 0;
    }
    int64_t last_rtt_us() const noexcept {
        return last_rtt_us_.load(std::memory_order_relaxed);
    }
    uint64_t accept_count() const noexcept {
        return accept_count_.load(std::memory_order_relaxed);
    }
    uint64_t fail_count() const noexcept {
        return fail_count_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<bool> has_offset_{false};
    std::atomic<int64_t> offset_us_{0};
    std::atomic<int64_t> last_rtt_us_{0};
    std::atomic<uint64_t> accept_count_{0};
    std::atomic<uint64_t> fail_count_{0};
};

// exchange_ts <= 0（如 depth20）不记外部延迟。
// lat = recv + offset - exchange；负值返回 nullopt，由调用方记丢弃。
inline std::optional<uint64_t> exchange_to_recv_us(int64_t exchange_ts_us,
                                                   int64_t recv_ts_us,
                                                   int64_t offset_us) {
    if (exchange_ts_us <= 0) {
        return std::nullopt;
    }
    const int64_t lat = recv_ts_us + offset_us - exchange_ts_us;
    if (lat < 0) {
        return std::nullopt;
    }
    return static_cast<uint64_t>(lat);
}

}  // namespace mds
