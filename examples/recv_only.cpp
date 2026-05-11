// examples/recv_only.cpp
//
// 演示：用 mds 库做最小 live 接收。
// 编译产物：build/recv_only
//
// 用法：
//   ./build/recv_only
//
// 说明：
//   - 这是“最常见的库调用姿势”：配置直接写在代码里。
//   - 运行 15 秒后自动 stop 并打印 metrics 快照。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

#include <mds/feed.h>
#include <mds/types.h>

int main() {
    constexpr int kRunSeconds = 15;

    mds::FeedConfig cfg;
    cfg.symbols = {"btcusdt", "ethusdt"};
    cfg.ws_url = "wss://stream.binance.com:9443/stream";
    cfg.data_dir = "./data";  // 置为 "" 可关闭落盘
    cfg.reconnect_interval_ms = 100;
    cfg.ping_interval_ms = 1000;
    cfg.no_data_timeout_ms = 3000;

    mds::MarketDataFeed feed(cfg);

    std::atomic<uint64_t> trade_seen{0};
    std::atomic<uint64_t> book_seen{0};

    feed.set_on_trade([&](const mds::Trade& t) {
        const uint64_t n = trade_seen.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 3) {
            std::cout << "[trade] #" << n
                      << " " << t.symbol
                      << " price=" << t.price
                      << " qty=" << t.quantity << std::endl;
        }
    });

    feed.set_on_orderbook([&](const mds::OrderBookSnapshot& ob) {
        const uint64_t n = book_seen.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 3) {
            std::cout << "[book ] #" << n
                      << " " << ob.symbol
                      << " bid=" << ob.best_bid_price()
                      << " ask=" << ob.best_ask_price() << std::endl;
        }
    });

    std::cout << "[example] start recv_only: duration=" << kRunSeconds
              << "s data_dir='" << cfg.data_dir << "'" << std::endl;

    feed.start();
    std::this_thread::sleep_for(std::chrono::seconds(kRunSeconds));
    feed.stop();

    const auto m = feed.metrics_snapshot();
    std::cout << "[example] done"
              << " msgs=" << m.messages_received
              << " trades=" << m.trades_received
              << " books=" << m.orderbooks_received
              << " written_trade=" << m.trades_written
              << " written_book=" << m.orderbooks_written
              << " dropped_trade=" << m.trades_dropped
              << " dropped_book=" << m.orderbooks_dropped
              << " connect=" << m.connect_success << "/" << m.connect_attempts
              << std::endl;

    return 0;
}
