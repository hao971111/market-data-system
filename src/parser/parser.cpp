#include "parser.h"
#include <charconv>
#include <nlohmann/json.hpp>
#include <iostream>
#include <cmath>     // isfinite
#include <climits>   // INT64_MAX
#include <algorithm> // min
#include <system_error>
#include <cctype>
#include <locale>
#include <sstream>
#include <type_traits>
#include <utility>

namespace mds {

namespace {

template <typename T, typename = void>
struct has_from_chars_floating : std::false_type {};

template <typename T>
struct has_from_chars_floating<T, std::void_t<decltype(std::from_chars(
                                    std::declval<const char*>(),
                                    std::declval<const char*>(),
                                    std::declval<T&>()))>> : std::true_type {};

inline bool parse_double_strict(std::string_view text, double& out) {
    if constexpr (has_from_chars_floating<double>::value) {
        const char* first = text.data();
        const char* last = text.data() + text.size();
        auto [ptr, ec] = std::from_chars(first, last, out);
        return ec == std::errc() && ptr == last;
    } else {
        // 兼容旧工具链：无浮点 from_chars 时，使用 classic locale 严格解析。
        if (text.empty()) {
            return false;
        }
        const unsigned char first_ch = static_cast<unsigned char>(text.front());
        if (std::isspace(first_ch)) {
            return false;
        }
        std::istringstream iss{std::string(text)};
        iss.imbue(std::locale::classic());
        iss >> std::noskipws >> out;
        return iss && iss.eof();
    }
}

std::string to_lower_ascii(std::string s) {
    for (char& ch : s) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return s;
}

}  // namespace

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

        // 注意：Binance 的 price/quantity 是字符串，需要转 double。
        // 用 from_chars 避免 stod 的 locale 与异常路径开销。
        const auto p_it = j.find("p");
        const auto q_it = j.find("q");
        if (p_it == j.end() || q_it == j.end() ||
            !p_it->is_string() || !q_it->is_string()) {
            std::cerr << "[Parser] Invalid price/quantity field type" << std::endl;
            return std::nullopt;
        }
        const auto& price_text = p_it->get_ref<const std::string&>();
        const auto& qty_text = q_it->get_ref<const std::string&>();
        if (!parse_double_strict(price_text, trade.price) ||
            !parse_double_strict(qty_text, trade.quantity)) {
            std::cerr << "[Parser] Invalid price or quantity format" << std::endl;
            return std::nullopt;
        }

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
        trade.exchange_ts_us = ts_ms * 1000;
        trade.trade_id       = j["t"].get<int64_t>();
        trade.is_buyer_maker = j["m"].get<bool>();

        // symbol：Binance trade 的 s 是大写；统一成小写，与订阅列表 / OrderBook 一致
        auto sym = to_lower_ascii(j["s"].get<std::string>());
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

        const auto id_it = j.find("lastUpdateId");
        if (id_it == j.end() || !id_it->is_number_integer()) {
            return std::nullopt;
        }
        const auto last_update_id = id_it->get<int64_t>();
        if (last_update_id <= 0) {
            return std::nullopt;
        }

        OrderBookSnapshot snap{};
        snap.last_update_id = last_update_id;
        // 与 Trade 一致：统一小写，避免大小写分叉
        snap.set_symbol(to_lower_ascii(std::string(symbol)));

        auto parse_levels = [](const nlohmann::json& arr, OrderBookLevel* levels) {
            if (levels == nullptr) {
                return false;
            }
            std::fill_n(levels, ORDERBOOK_DEPTH, OrderBookLevel{0.0, 0.0});
            if (!arr.is_array()) {
                return false;
            }
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
                    if (!parse_double_strict(level[0].get_ref<const std::string&>(), levels[i].price) ||
                        !parse_double_strict(level[1].get_ref<const std::string&>(), levels[i].quantity)) {
                        return false;
                    }
                    if (!std::isfinite(levels[i].price) || levels[i].price <= 0 ||
                        !std::isfinite(levels[i].quantity) || levels[i].quantity <= 0) {
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
