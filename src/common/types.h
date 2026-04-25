#pragma once

#include <cstdint>
#include <cstring>      // memcpy
#include <algorithm>    // min
#include <string_view>  // C++17
namespace mds {  // market data system

/**
 * Trade - 逐笔成交数据（从交易所 @trade 流接收）
 * 
 * 设计要点：
 * 1. 固定大小，可直接memcpy，适合二进制存储
 * 2. 无动态内存分配，避免堆操作开销
 * 3. 字段顺序考虑内存对齐，减少padding
 */
struct Trade {
    int64_t  timestamp_us;     // 8字节：微秒时间戳（交易所时间）
    int64_t  trade_id;         // 8字节：成交ID
    double   price;            // 8字节：成交价格
    double   quantity;         // 8字节：成交数量
    char     symbol[16];       // 16字节：交易对，如"BTCUSDT"
    bool     is_buyer_maker;   // 1字节：买方是否为maker
    char     padding[7];       // 7字节：填充，保证结构体总大小是8的倍数
    
    // 总大小：8+8+8+8+16+1+7 = 56字节（8字节对齐）
    
    void set_symbol(std::string_view sym) {
        size_t len = std::min(sym.size(), sizeof(symbol) - 1);
        std::memcpy(symbol, sym.data(), len);
        symbol[len] = '\0';
    }
};

// 编译期检查：确保结构体大小符合预期
static_assert(sizeof(Trade) == 56, "Trade size must be 56 bytes");
static_assert(sizeof(Trade) % 8 == 0, "Trade must be 8-byte aligned");

/**
 * OrderBookLevel - 订单簿单档数据
 * 
 * 表示某个价格上的挂单量
 */
struct OrderBookLevel {
    double price;      // 8字节：价格
    double quantity;   // 8字节：该价格上的总挂单量
    // 总大小：16字节
};

static_assert(sizeof(OrderBookLevel) == 16, "OrderBookLevel size must be 16 bytes");

/**
 * OrderBookSnapshot - 订单簿快照（从交易所 @depth 流接收）
 * 
 * 包含买卖双方各20档深度
 * 
 * 买盘(bids): 按价格从高到低排列，bids[0]是最高买价（买一）
 * 卖盘(asks): 按价格从低到高排列，asks[0]是最低卖价（卖一）
 */
constexpr int ORDERBOOK_DEPTH = 20;  // 深度档数

struct OrderBookSnapshot {
    int64_t timestamp_us;                       // 8字节：时间戳
    char    symbol[16];                         // 16字节：交易对
    OrderBookLevel bids[ORDERBOOK_DEPTH];       // 320字节：买盘20档
    OrderBookLevel asks[ORDERBOOK_DEPTH];       // 320字节：卖盘20档
    
    // 总大小：8 + 16 + 320 + 320 = 664字节
    
    void set_symbol(std::string_view sym) {
        size_t len = std::min(sym.size(), sizeof(symbol) - 1);
        std::memcpy(symbol, sym.data(), len);
        symbol[len] = '\0';
    }
    
    // 获取买一价/卖一价（最优报价）
    double best_bid_price() const { return bids[0].price; }
    double best_ask_price() const { return asks[0].price; }
    
    // 买卖价差（spread）
    double spread() const { return best_ask_price() - best_bid_price(); }
};

static_assert(sizeof(OrderBookSnapshot) == 664, "OrderBookSnapshot size must be 664 bytes");
static_assert(sizeof(OrderBookSnapshot) % 8 == 0, "OrderBookSnapshot must be 8-byte aligned");

}  // namespace mds
