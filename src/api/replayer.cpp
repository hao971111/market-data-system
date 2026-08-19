#include "mds/replayer.h"

#include <utility>

#include "common/types.h"
#include "replay/order_book_replayer.h"
#include "replay/record_replayer.h"
#include "replay/trade_replayer.h"
#include "storage/file_roll.h"

namespace mds {

// pImpl：把 TradeReplayer / OrderBookReplayer 这些内部模板隐藏在这里，
// 公共头 <mds/replayer.h> 不会暴露 src/replay/* 给外部用户。
class Replayer::Impl {
public:
    explicit Impl(std::string data_dir) : data_dir_(std::move(data_dir)) {}

    const std::string& data_dir() const noexcept { return data_dir_; }

    Replayer::Result replay_trades(TradeCallback cb,
                                   std::optional<int64_t> from_us,
                                   std::optional<int64_t> to_us) {
        TradeReplayer trade_replayer;
        // 直接把用户回调 move 进内部，让 replay_all 自己处理空 / 抛异常。
        // 不在 facade 再包一层 lambda，避免空 std::function 被包成"永远 truthy"
        // 的 lambda、必须走 bad_function_call 异常路径才能识别空回调。
        const auto inner = trade_replayer.replay_all(
            data_dir_, std::move(cb), TimeWindow{from_us, to_us});
        return to_public(inner);
    }

    Replayer::Result replay_orderbooks(OrderBookCallback cb,
                                       std::optional<int64_t> from_us,
                                       std::optional<int64_t> to_us) {
        OrderBookReplayer ob_replayer;
        const auto inner = ob_replayer.replay_all(
            data_dir_, std::move(cb), TimeWindow{from_us, to_us});
        return to_public(inner);
    }

private:
    // 内部 mds::ReplayResult（src/replay/record_replayer.h，顶层）→
    // 公共 mds::Replayer::Result（嵌套类型）。
    // 两个 struct 字段语义完全一致，手工拷贝是为了让 public header
    // 不依赖 src/ 的内部头。
    static Replayer::Result to_public(const ReplayResult& in) {
        Replayer::Result out;
        out.header_record_count = in.header_record_count;
        out.records_replayed    = in.records_replayed;
        out.completed           = in.completed;
        out.file_error          = in.file_error;
        out.callback_error      = in.callback_error;
        out.count_mismatch      = in.count_mismatch;
        return out;
    }

    std::string data_dir_;
};

// 析构 / move 必须放在 .cpp 中，因为 std::unique_ptr<Impl> 在头里只看到
// Impl 的前向声明；销毁/移动时需要 Impl 的完整类型。
Replayer::Replayer(std::string data_dir)
    : impl_(std::make_unique<Impl>(std::move(data_dir))) {}

Replayer::~Replayer() = default;
Replayer::Replayer(Replayer&&) noexcept = default;
Replayer& Replayer::operator=(Replayer&&) noexcept = default;

Replayer::Result Replayer::replay_trades(TradeCallback callback,
                                         std::optional<int64_t> from_us,
                                         std::optional<int64_t> to_us) {
    return impl_->replay_trades(std::move(callback), from_us, to_us);
}

Replayer::Result Replayer::replay_orderbooks(OrderBookCallback callback,
                                             std::optional<int64_t> from_us,
                                             std::optional<int64_t> to_us) {
    return impl_->replay_orderbooks(std::move(callback), from_us, to_us);
}

const std::string& Replayer::data_dir() const noexcept {
    return impl_->data_dir();
}

}  // namespace mds
