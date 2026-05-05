#include "parser.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <cmath>     // isfinite
#include <climits>   // INT64_MAX
#include <algorithm> // min
#include <chrono>

namespace mds {

std::optional<Trade> Parser::parse_trade(const std::string& json_str) {
    try {
        auto j = nlohmann::json::parse(json_str);
        return parse_trade(j);
    } catch (const nlohmann::json::exception& e) {
        // JSON 结构错误
        std::cerr << "[Parser] JSON parse error: " << e.what() << std::endl;
        return std::nullopt;
    } catch (const std::exception& e) {
        // stod 等转换错误（invalid_argument / out_of_range）
        std::cerr << "[Parser] Parse error: " << e.what() << std::endl;
        return std::nullopt;
    }
}

std::optional<Trade> Parser::parse_trade(const nlohmann::json& j) {
    try {
        // 过滤非 trade 消息（比如订阅确认消息、ping 等）
        if (!j.contains("e") || j["e"] != "trade") {
            return std::nullopt;
        }

        // 必须字段校验
        if (!j.contains("s") || !j.contains("t") ||
            !j.contains("p") || !j.contains("q") ||
            !j.contains("T") || !j.contains("m")) {
            std::cerr << "[Parser] Missing fields in trade message" << std::endl;
            return std::nullopt;
        }

        Trade trade{};

        // 注意：Binance 的 price/quantity 是字符串，需要转 double
        // 原因：浮点精度，交易所用字符串避免精度损失
        trade.price    = std::stod(j["p"].get<std::string>());
        trade.quantity = std::stod(j["q"].get<std::string>());

        // 校验价格合法性（防止 inf/nan 污染后续计算）
        if (!std::isfinite(trade.price) || trade.price <= 0 ||
            !std::isfinite(trade.quantity) || trade.quantity <= 0) {
            std::cerr << "[Parser] Invalid price or quantity value" << std::endl;
            return std::nullopt;
        }

        // 毫秒时间戳转微秒（先校验范围，防止乘法溢出）
        int64_t ts_ms = j["T"].get<int64_t>();
        if (ts_ms <= 0 || ts_ms > INT64_MAX / 1000) {
            std::cerr << "[Parser] Timestamp out of range" << std::endl;
            return std::nullopt;
        }
        trade.timestamp_us   = ts_ms * 1000;
        trade.trade_id       = j["t"].get<int64_t>();
        trade.is_buyer_maker = j["m"].get<bool>();

        // symbol 超长时警告（set_symbol 会截断到15字节）
        auto sym = j["s"].get<std::string>();
        if (sym.size() > 15) {
            std::cerr << "[Parser] Symbol truncated: " << sym << std::endl;
        }
        trade.set_symbol(sym);

        return trade;
    } catch (const nlohmann::json::exception& e) {
        std::cerr << "[Parser] JSON parse error: " << e.what() << std::endl;
        return std::nullopt;
    } catch (const std::exception& e) {
        std::cerr << "[Parser] Parse error: " << e.what() << std::endl;
        return std::nullopt;
    }
}

std::optional<OrderBookSnapshot> Parser::parse_orderbook(const std::string& json_str,
                                                          std::string_view symbol) {
    try {
        auto j = nlohmann::json::parse(json_str);
        return parse_orderbook(j, symbol);
    } catch (const std::exception& e) {
        std::cerr << "[Parser] OrderBook parse error: " << e.what() << std::endl;
        return std::nullopt;
    }
}

std::optional<OrderBookSnapshot> Parser::parse_orderbook(const nlohmann::json& j,
                                                          std::string_view symbol) {
    try {
        if (!j.contains("bids") || !j.contains("asks")) {
            return std::nullopt;
        }

        OrderBookSnapshot snap{};
        snap.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        snap.set_symbol(symbol);

        auto parse_levels = [](const nlohmann::json& arr, OrderBookLevel* levels) {
            if (!arr.is_array()) {
                return false;
            }
            auto parse_double = [](const std::string& text, double& out) {
                size_t pos = 0;
                out = std::stod(text, &pos);
                return pos == text.size();
            };
            const auto count = std::min(arr.size(), static_cast<size_t>(ORDERBOOK_DEPTH));
            for (size_t i = 0; i < count; i++) {
                const auto& level = arr[i];
                if (!level.is_array() || level.size() < 2) {
                    return false;
                }
                if (!level[0].is_string() || !level[1].is_string()) {
                    return false;
                }
                try {
                    if (!parse_double(level[0].get_ref<const std::string&>(), levels[i].price) ||
                        !parse_double(level[1].get_ref<const std::string&>(), levels[i].quantity)) {
                        return false;
                    }
                } catch (const std::exception&) {
                    return false;
                }
            }
            return true;
        };

        if (!parse_levels(j["bids"], snap.bids) ||
            !parse_levels(j["asks"], snap.asks)) {
            return std::nullopt;
        }

        return snap;

    } catch (const std::exception& e) {
        std::cerr << "[Parser] OrderBook parse error: " << e.what() << std::endl;
        return std::nullopt;
    }
}

}  // namespace mds
