#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace mds {

// 一次性 HTTP(S) GET。给时钟校准用，Step 22 补数也会走这里。
// proxy_url 规则与 WebSocket 相同：http://host:port，空=直连。
// 超时覆盖 connect + TLS + 读写；失败不抛，填 error。
struct HttpGetResult {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string error;
};

HttpGetResult http_get(
    const std::string& url,
    const std::string& proxy_url = "",
    std::chrono::milliseconds timeout = std::chrono::milliseconds{3000});

// Binance GET /api/v3/time → {"serverTime": <ms>}。成功返回 unix 微秒。
std::optional<int64_t> parse_binance_server_time_us(const std::string& body);

}  // namespace mds
