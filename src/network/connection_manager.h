#pragma once

#include "websocket_client.h"
#include "trade_seq_gate.h"
#include "../config/config.h"
#include "../common/types.h"
#include "../monitor/metrics.h"
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <string>
#include <unordered_map>

namespace mds {

using TradeCallback     = std::function<void(const Trade&)>;
using OrderBookCallback = std::function<void(const OrderBookSnapshot&)>;

// ConnectionManager 职责：
//   - 管理 WebSocketClient 的生命周期
//   - 断线后自动重连（指数退避）
//   - 解析原始 JSON，分发给上层（屏蔽协议细节）
class ConnectionManager {
public:
    // metrics 由 main 持有，按引用注入。生命周期必须长于 ConnectionManager。
    ConnectionManager(const Config& config, Metrics& metrics);
    ~ConnectionManager();

    void start(const std::string& url);
    void stop();

    // 上层只需关心解析后的数据，不用处理原始 JSON
    void set_trade_callback(TradeCallback cb)         { on_trade_     = std::move(cb); }
    void set_orderbook_callback(OrderBookCallback cb) { on_orderbook_ = std::move(cb); }

    // 离线压测/测试入口：复用 live 模式同一条原始消息解析链路，但不连接网络。
    void process_raw_message(const std::string& msg) { on_raw_message(msg); }

    // 该 symbol 的实时成交当前是否可信。未见过的 symbol 视为 Live（尚未跳号）。
    bool can_trade(const std::string& symbol) const {
        const auto it = trade_seq_gates_.find(symbol);
        if (it == trade_seq_gates_.end()) {
            return true;
        }
        return it->second.can_trade();
    }

private:
    void reconnect_loop(const std::string& url);
    int64_t calc_backoff_ms(int attempt) const;
    std::string build_subscribe_msg(const std::vector<std::string>& symbols) const;

    const Config& config_;
    Metrics&      metrics_;   // 引用，外部持有；只 inc 不 reset
    TradeCallback     on_trade_;
    OrderBookCallback on_orderbook_;
    
    void on_raw_message(const std::string& msg);  // 内部：解析并分发

    std::unique_ptr<WebSocketClient> client_;
    std::mutex client_mutex_;                  // 保护 client_ 的跨线程访问
    std::atomic<bool> running_{false};
    std::atomic<bool> need_reconnect_{false};

    // 第一次进入 on_raw_message 时尝试绑核，之后短路直通。
    // 当前 live 的 WebSocket 回调与 bench 的 process_raw_message 都是单线程进
    // on_raw_message，不存在并发，用普通 bool 即可；未来若 io_context 改多 worker
    // 再换回 std::atomic<bool> + compare_exchange。
    bool pin_attempted_ = false;

    // 每个 symbol 一份序列门。on_raw_message 当前单线程进入，无需加锁。
    std::unordered_map<std::string, TradeSeqGate> trade_seq_gates_;
    // 每个 symbol 上一帧快照的 lastUpdateId。未出现过则不在 map 里。
    std::unordered_map<std::string, int64_t> last_orderbook_update_id_;

    std::thread reconnect_thread_;
    std::mutex cv_mutex_;
    std::condition_variable cv_;
};

}  // namespace mds
