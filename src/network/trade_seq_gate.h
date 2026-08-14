#pragma once

#include <cstdint>
#include <optional>
#include <utility>

namespace mds {

// 单个 symbol 的 trade_id 序列门（Step 11）。
// 口径来自 Step 10：正常步进严格 +1；按 symbol 独立维护。
// 重传补数（on_replay）完整闭环在 Step 22；这里先具备状态与接口。
class TradeSeqGate {
public:
    enum class State : uint8_t { Live, Recovering };

    enum class Action : uint8_t {
        Accept,     // 放行回调
        Duplicate,  // id <= last（重复/乱序/回退）
        Gap,        // 首次发现跳号，进入 Recovering；本包不放行
        Hold,       // 已在 Recovering，实时包暂不放行
    };

    struct Result {
        Action action = Action::Accept;
        // 仅 Gap 时有效：缺失闭区间 [missing_begin, missing_end]
        int64_t missing_begin = 0;
        int64_t missing_end = 0;

        uint64_t missing_count() const {
            if (action != Action::Gap || missing_end < missing_begin) {
                return 0;
            }
            return static_cast<uint64_t>(missing_end - missing_begin + 1);
        }
    };

    State state() const noexcept { return state_; }
    bool can_trade() const noexcept { return state_ == State::Live; }
    bool has_last() const noexcept { return has_last_; }
    int64_t last_id() const noexcept { return last_id_; }

    std::optional<std::pair<int64_t, int64_t>> missing_range() const noexcept {
        if (state_ != State::Recovering) {
            return std::nullopt;
        }
        return std::make_pair(missing_begin_, missing_end_);
    }

    // 处理一笔实时成交 id。
    Result on_live_trade(int64_t trade_id) noexcept {
        if (state_ == State::Recovering) {
            return Result{Action::Hold, missing_begin_, missing_end_};
        }

        if (!has_last_) {
            has_last_ = true;
            last_id_ = trade_id;
            return Result{Action::Accept};
        }

        if (trade_id == last_id_ + 1) {
            last_id_ = trade_id;
            return Result{Action::Accept};
        }

        if (trade_id <= last_id_) {
            return Result{Action::Duplicate};
        }

        // trade_id > last_id_ + 1
        state_ = State::Recovering;
        missing_begin_ = last_id_ + 1;
        missing_end_ = trade_id - 1;
        // 注意：不把 last 推到 trade_id；缺口以 last 为锚，等待补数（Step 22）
        return Result{Action::Gap, missing_begin_, missing_end_};
    }

    // 恢复态下吃重传包：必须严格 last+1。补到 missing_end 后回到 Live。
    // 返回是否接受该重传包。
    bool on_replay_trade(int64_t trade_id) noexcept {
        if (state_ != State::Recovering || !has_last_) {
            return false;
        }
        if (trade_id != last_id_ + 1) {
            return false;
        }
        last_id_ = trade_id;
        if (last_id_ == missing_end_) {
            state_ = State::Live;
            missing_begin_ = 0;
            missing_end_ = 0;
        }
        return true;
    }

private:
    bool has_last_ = false;
    int64_t last_id_ = 0;
    int64_t missing_begin_ = 0;
    int64_t missing_end_ = 0;
    State state_ = State::Live;
};

}  // namespace mds
