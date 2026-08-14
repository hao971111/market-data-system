#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace mds {

// Trade - 逐笔成交数据（固定布局，适合高效二进制写入/回放）
struct Trade {
    int64_t timestamp_us;
    int64_t trade_id;
    double price;
    double quantity;
    char symbol[16];
    bool is_buyer_maker;
    char padding[7];

    void set_symbol(std::string_view sym) {
        const std::size_t len = std::min(sym.size(), sizeof(symbol) - 1);
        std::memcpy(symbol, sym.data(), len);
        symbol[len] = '\0';
    }
};

static_assert(sizeof(Trade) == 56, "Trade size must be 56 bytes");
static_assert(sizeof(Trade) % 8 == 0, "Trade must be 8-byte aligned");

struct OrderBookLevel {
    double price;
    double quantity;
};

static_assert(sizeof(OrderBookLevel) == 16, "OrderBookLevel size must be 16 bytes");

constexpr int ORDERBOOK_DEPTH = 20;

// OrderBookSnapshot —— Binance `@depth20@100ms` 的周期性 20 档局部快照。
//
// 这不是完整增量订单簿：
//   - 每约 100ms 一帧，两次快照之间的档位变化不会全部留下
//   - 不能用来做 gap 补齐、逐笔订单簿重建、或和 Trade 精确对齐
//   - recv_ts_us 是本机收到时的 system_clock，报文里没有交易所时间
//   - last_update_id 来自报文 lastUpdateId，用来检查快照序号是否连续
struct OrderBookSnapshot {
    int64_t recv_ts_us;      // 本地接收时间（微秒），不是交易所时间
    int64_t last_update_id;  // Binance lastUpdateId；缺省 0 表示未解析到
    char symbol[16];
    OrderBookLevel bids[ORDERBOOK_DEPTH];
    OrderBookLevel asks[ORDERBOOK_DEPTH];

    void set_symbol(std::string_view sym) {
        const std::size_t len = std::min(sym.size(), sizeof(symbol) - 1);
        std::memcpy(symbol, sym.data(), len);
        symbol[len] = '\0';
    }

    double best_bid_price() const { return bids[0].price; }
    double best_ask_price() const { return asks[0].price; }
    double spread() const { return best_ask_price() - best_bid_price(); }
};

static_assert(sizeof(OrderBookSnapshot) == 672,
              "OrderBookSnapshot size must be 672 bytes");
static_assert(sizeof(OrderBookSnapshot) % 8 == 0,
              "OrderBookSnapshot must be 8-byte aligned");

}  // namespace mds
