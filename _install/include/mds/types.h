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

// OrderBookSnapshot - 订单簿快照（买卖各 20 档）
struct OrderBookSnapshot {
    int64_t timestamp_us;
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

static_assert(sizeof(OrderBookSnapshot) == 664,
              "OrderBookSnapshot size must be 664 bytes");
static_assert(sizeof(OrderBookSnapshot) % 8 == 0,
              "OrderBookSnapshot must be 8-byte aligned");

}  // namespace mds
