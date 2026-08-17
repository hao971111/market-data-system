#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace mds {

// Trade - 逐笔成交数据（固定布局，适合高效二进制写入/回放）
//
// 落盘格式 version=2。按 UTC 小时切分 trades_YYYYMMDD_HH.bin。
// 旧 data/trades.bin（version=1 / 56 字节记录）不兼容。
// crc32 预留给 Step 16，当前恒为 0，不参与校验。
struct Trade {
    int64_t exchange_ts_us;  // 交易所报文时间（微秒）
    int64_t recv_ts_us;      // 本进程收到消息的时间（system_clock）
    int64_t trade_id;
    double price;
    double quantity;
    char symbol[16];
    bool is_buyer_maker;
    char padding[3];
    uint32_t crc32;  // reserved（Step 16）

    void set_symbol(std::string_view sym) {
        const std::size_t len = std::min(sym.size(), sizeof(symbol) - 1);
        std::memcpy(symbol, sym.data(), len);
        symbol[len] = '\0';
    }
};

static_assert(sizeof(Trade) == 64, "Trade size must be 64 bytes");
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
//   - exchange_ts_us 对 depth20 快照恒为 0（报文没有交易所时间）
//   - last_update_id 来自报文 lastUpdateId，只用来检查回退
//
// 落盘格式 version=2。按 UTC 小时切分 orderbooks_YYYYMMDD_HH.bin。
// 旧 data/orderbooks.bin 不兼容。
// crc32 预留给 Step 16，当前恒为 0。
struct OrderBookSnapshot {
    int64_t exchange_ts_us;
    int64_t recv_ts_us;
    int64_t last_update_id;
    char symbol[16];
    OrderBookLevel bids[ORDERBOOK_DEPTH];
    OrderBookLevel asks[ORDERBOOK_DEPTH];
    uint32_t crc32;  // reserved（Step 16）
    char padding[4];

    void set_symbol(std::string_view sym) {
        const std::size_t len = std::min(sym.size(), sizeof(symbol) - 1);
        std::memcpy(symbol, sym.data(), len);
        symbol[len] = '\0';
    }

    double best_bid_price() const { return bids[0].price; }
    double best_ask_price() const { return asks[0].price; }
    double spread() const { return best_ask_price() - best_bid_price(); }
};

static_assert(sizeof(OrderBookSnapshot) == 688,
              "OrderBookSnapshot size must be 688 bytes");
static_assert(sizeof(OrderBookSnapshot) % 8 == 0,
              "OrderBookSnapshot must be 8-byte aligned");

}  // namespace mds
