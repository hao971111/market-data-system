/**
 * @file main.cpp
 * @brief 高性能行情数据系统入口
 * 
 * 系统功能：
 * 1. 从Binance WebSocket接入实时行情（Trade + OrderBook）
 * 2. 高效解析和缓存
 * 3. 二进制格式持久化存储
 * 4. 按时间回放
 * 5. 提供标准化API供下游模块调用
 */

#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include "cache/trade_ring_buffer.h"
#include "config/config.h"
#include "network/connection_manager.h"
#include "storage/binary_trade_writer.h"

std::atomic<bool> g_running{true};

void signal_handler(int signum) {
    std::cout << "\n[INFO] Received signal " << signum << ", shutting down..." << std::endl;
    g_running = false;
}

int main() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "==================================" << std::endl;
    std::cout << " Market Data System v0.1.0" << std::endl;
    std::cout << "==================================" << std::endl;

    mds::Config config;
    config.load("config.json");

    // 声明顺序约束（析构是反向的，下面顺序不能调）：
    //   trade_writer / trade_cache 必须在 mgr 之前声明，
    //   这样 main 退出时 mgr 先析构（stop() + join 回调线程），
    //   之后 cache/writer 再析构，回调 lambda 捕获的引用始终有效。
    //   一旦把 mgr 提到 cache/writer 上面，回调线程会在已析构对象上动手 -> UB。
    mds::BinaryTradeWriter trade_writer;
    if (!trade_writer.open(config.data_dir, config.ring_buffer_size)) {
        return 1;
    }

    // 内存环形缓冲：保存最近 N 条 Trade，给上层快速查询用。
    // 当前未做线程安全，仅在 trade 回调线程内 push。
    mds::TradeRingBuffer trade_cache(config.ring_buffer_size);

    mds::ConnectionManager mgr(config);

    mgr.set_trade_callback([&trade_writer, &trade_cache](const mds::Trade& t) {
        trade_cache.push(t);

        if (!trade_writer.write(t)) {
            std::cerr << "[ERROR] Failed to write trade" << std::endl;
        }

        std::cout << "[TRADE] " << t.symbol
                  << " price=" << t.price
                  << " qty=" << t.quantity << std::endl;
    });

    mgr.set_orderbook_callback([](const mds::OrderBookSnapshot& ob) {
        std::cout << "[BOOK]  " << ob.symbol
                  << " bid=" << ob.best_bid_price()
                  << " ask=" << ob.best_ask_price()
                  << " spread=" << ob.spread() << std::endl;
    });

    const auto& url = config.data_sources[0].ws_url;
    mgr.start(url);

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    mgr.stop();
    std::cout << "[INFO] System shutdown complete." << std::endl;
    return 0;
}
