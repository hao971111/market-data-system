#pragma once

#include "websocket_client.h"
#include "../config/config.h"
#include "../common/types.h"
#include "../monitor/metrics.h"
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <string>

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

    std::thread reconnect_thread_;
    std::mutex cv_mutex_;
    std::condition_variable cv_;
};

}  // namespace mds
