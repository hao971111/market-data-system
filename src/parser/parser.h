#pragma once

#include "../common/types.h"
#include <string>
#include <optional>
#include <nlohmann/json_fwd.hpp>

namespace mds {

// 解析结果：成功返回数据，失败返回 nullopt
// 用 optional 而不是抛异常，因为无效消息是正常情况（非致命错误）
class Parser {
public:
    // 解析 Trade 消息
    // 返回 nullopt 的情况：非trade消息、字段缺失、格式错误
    static std::optional<Trade> parse_trade(const std::string& json);

    // 解析 OrderBook 深度消息
    static std::optional<OrderBookSnapshot> parse_orderbook(const std::string& json,
                                                             std::string_view symbol);
    static std::optional<OrderBookSnapshot> parse_orderbook(const nlohmann::json& json,
                                                             std::string_view symbol);
};

}  // namespace mds
