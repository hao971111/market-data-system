#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "mds/types.h"

namespace mds {

// MarketDataFeed 的对外配置。比内部 Config 精简，只暴露库使用者关心的字段。
struct FeedConfig {
    // 订阅的交易对（小写），如 {"btcusdt", "ethusdt"}。
    std::vector<std::string> symbols = {"btcusdt", "ethusdt"};

    // WebSocket URL。默认 Binance combined stream。
    std::string ws_url = "wss://stream.binance.com:9443/stream";

    // HTTP CONNECT 代理 URL。空 = 直连，会进一步 fallback 到 https_proxy / HTTPS_PROXY 环境变量。
    std::string proxy_url;

    // 落盘目录。空字符串 = 不落盘（仅触发回调）；非空 = 自动写 data_dir/trades.bin、data_dir/orderbooks.bin。
    std::string data_dir = "./data";

    // 重连 / 心跳参数（毫秒）。
    int reconnect_interval_ms = 100;
    int ping_interval_ms      = 1000;
    int no_data_timeout_ms    = 3000;
};

// 运行时指标的只读快照。POD，可自由拷贝。
// 字段为内部 Metrics 的累计 counter；直方图 / percentile 后续按需追加。
struct MetricsSnapshot {
    uint64_t messages_received    = 0;
    uint64_t trades_received      = 0;
    uint64_t orderbooks_received  = 0;
    uint64_t trades_written       = 0;
    uint64_t orderbooks_written   = 0;
    uint64_t trades_dropped       = 0;
    uint64_t orderbooks_dropped   = 0;

    uint64_t connect_attempts     = 0;
    uint64_t connect_success      = 0;

    uint64_t pipeline_over_500us  = 0;
    uint64_t pipeline_over_1000us = 0;
    uint64_t clock_anomaly_count  = 0;
};

// MarketDataFeed —— 实时行情接收器（库的接收侧 facade）。
//
// 用法：
//   mds::FeedConfig cfg;
//   cfg.symbols = {"btcusdt"};
//   mds::MarketDataFeed feed(cfg);
//   feed.set_on_trade([](const mds::Trade&){ ... });
//   feed.start();
//   ...
//   feed.stop();
//
// 落盘：FeedConfig::data_dir 非空时自动写 trades.bin / orderbooks.bin；
//       置空则只回调、不落盘。
//
// 线程模型：start() 后内部启动 IO 线程；用户回调（on_trade / on_orderbook）
//          在 IO 线程上调用，回调中应避免长耗时操作。stop() / 析构会等待线程退出。
class MarketDataFeed {
public:
    using TradeCallback      = std::function<void(const Trade&)>;
    using OrderBookCallback  = std::function<void(const OrderBookSnapshot&)>;

    explicit MarketDataFeed(FeedConfig config);
    ~MarketDataFeed();

    MarketDataFeed(const MarketDataFeed&) = delete;
    MarketDataFeed& operator=(const MarketDataFeed&) = delete;
    MarketDataFeed(MarketDataFeed&&) noexcept;
    MarketDataFeed& operator=(MarketDataFeed&&) noexcept;

    // 回调注册。**必须**在 start() 之前调用——start() 之后内部 IO 线程会读这些
    // std::function，再赋值不是线程安全的，可能崩溃。
    void set_on_trade(TradeCallback cb);
    void set_on_orderbook(OrderBookCallback cb);

    // 启动接收。非阻塞，拉起内部 IO 线程后立即返回。
    void start();

    // 停止接收并等待内部线程退出。重复调用安全，析构时也会自动调用。
    void stop();

    // 当前运行指标快照。线程安全，可在任何线程调用。
    MetricsSnapshot metrics_snapshot() const;

    const FeedConfig& config() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mds
