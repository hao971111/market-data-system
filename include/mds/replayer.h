#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "mds/types.h"

namespace mds {

// Replayer：从指定 data_dir 顺序回放历史 trade / orderbook 二进制文件，
// 每条记录通过用户回调送出。
//
// 当前只提供"一次性回放"语义：调用 replay_xxx 后阻塞直到回放结束（完成或出错）。
// 后续如有需要，可在不破坏现有 API 的前提下追加流式接口（next_xxx）。
//
// 线程模型：单线程同步调用，回调在调用 replay_xxx 的同一线程内顺序触发。
class Replayer {
public:
    // 单次回放的结果汇总。
    //   header_record_count：文件 header 中记录的条数（writer close 时回填）。
    //                        0 表示老格式或文件未被正常关闭，count_mismatch 不参与判定。
    //   records_replayed   ：本次实际触发回调的条数。
    //   completed          ：file_error / callback_error / count_mismatch 全为 false 时为 true。
    //   file_error         ：reader 报告文件读取异常（文件不存在/损坏/截断）。
    //   callback_error     ：回调为空，或回调抛异常被库捕获，回放在该位置中断。
    //   count_mismatch     ：file_error=false 且 header>0 时，replayed 与 header 不一致。
    //                        传入 from/to 时不判定（窗外记录被丢掉是预期行为）。
    struct Result {
        uint64_t header_record_count = 0;
        uint64_t records_replayed    = 0;
        bool completed       = false;
        bool file_error      = false;
        bool callback_error  = false;
        bool count_mismatch  = false;
    };

    using TradeCallback     = std::function<void(const Trade&)>;
    using OrderBookCallback = std::function<void(const OrderBookSnapshot&)>;

    explicit Replayer(std::string data_dir);
    ~Replayer();

    Replayer(const Replayer&) = delete;
    Replayer& operator=(const Replayer&) = delete;
    Replayer(Replayer&&) noexcept;
    Replayer& operator=(Replayer&&) noexcept;

    // 顺序回放 data_dir 下 trades_YYYYMMDD_HH.bin。
    // from_us / to_us 按 recv_ts_us 闭区间过滤；任一为 nullopt 表示该侧不限制。
    // 都不传：和原来一样全量回放。只打开与窗口相交的小时文件。
    // 空回调 / 回调抛异常会被记入 Result.callback_error，并提前中断回放。
    Result replay_trades(TradeCallback callback,
                         std::optional<int64_t> from_us = std::nullopt,
                         std::optional<int64_t> to_us = std::nullopt);

    // 顺序回放 data_dir 下 orderbooks_*.bin。语义同上。
    Result replay_orderbooks(OrderBookCallback callback,
                             std::optional<int64_t> from_us = std::nullopt,
                             std::optional<int64_t> to_us = std::nullopt);

    // 构造时传入的数据目录，仅用于诊断。
    const std::string& data_dir() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mds
