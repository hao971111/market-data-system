#include "https_client.h"

#include <cstdint>
#include <regex>

#include <boost/asio/ssl/context.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <nlohmann/json.hpp>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

namespace mds {
namespace {

struct HttpUrl {
    bool tls = false;
    std::string host;
    std::string port;
    std::string path;
};

std::optional<HttpUrl> parse_http_url(const std::string& url) {
    static const std::regex re(R"(^(https?)://([^:/]+)(?::(\d+))?(/.*)?$)");
    std::smatch m;
    if (!std::regex_match(url, m, re)) {
        return std::nullopt;
    }
    HttpUrl u;
    u.tls = (m[1].str() == "https");
    u.host = m[2].str();
    u.port = (m[3].matched && m[3].length() > 0) ? m[3].str()
                                                 : (u.tls ? "443" : "80");
    u.path = (m[4].matched && m[4].length() > 0) ? m[4].str() : "/";
    return u;
}

struct ProxyTarget {
    std::string host;
    std::string port;
};

// 与 websocket_client 同一条规则；错误信息不能回显 proxy_url。
std::optional<ProxyTarget> parse_proxy_url(const std::string& proxy_url) {
    if (proxy_url.empty()) {
        return std::nullopt;
    }
    static const std::regex re(R"(^https?://([^:/]+):(\d+)/?$)");
    std::smatch m;
    if (!std::regex_match(proxy_url, m, re)) {
        throw std::runtime_error(
            "Invalid proxy_url format (expected http://host:port, no auth/path)");
    }
    return ProxyTarget{m[1].str(), m[2].str()};
}

void connect_via_proxy(beast::tcp_stream& stream, tcp::resolver& resolver,
                       const ProxyTarget& proxy, const HttpUrl& url) {
    auto eps = resolver.resolve(tcp::v4(), proxy.host, proxy.port);
    stream.connect(eps);

    const std::string target = url.host + ":" + url.port;
    http::request<http::empty_body> conn_req{http::verb::connect, target, 11};
    conn_req.set(http::field::host, target);
    conn_req.set(http::field::proxy_connection, "keep-alive");
    http::write(stream, conn_req);

    beast::flat_buffer buf;
    http::response_parser<http::empty_body> parser;
    parser.skip(true);
    http::read_header(stream, buf, parser);
    const auto& proxy_res = parser.get();
    if (proxy_res.result() != http::status::ok) {
        throw std::runtime_error("Proxy CONNECT failed: "
                                 + std::to_string(static_cast<unsigned>(proxy_res.result()))
                                 + " " + std::string(proxy_res.reason()));
    }
    if (buf.size() != 0) {
        throw std::runtime_error("Proxy returned unexpected bytes after CONNECT");
    }
}

template <typename Stream>
HttpGetResult write_and_read(Stream& stream, const HttpUrl& url) {
    http::request<http::string_body> req{http::verb::get, url.path, 11};
    req.set(http::field::host, url.host);
    req.set(http::field::user_agent, "mds-client/1.0");
    req.set(http::field::connection, "close");
    http::write(stream, req);

    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);

    HttpGetResult out;
    out.status = static_cast<int>(res.result_int());
    out.body = std::move(res.body());
    constexpr std::size_t kMaxBody = 64 * 1024;
    if (out.body.size() > kMaxBody) {
        out.body.clear();
        out.error = "response body too large";
        return out;
    }
    if (res.result() != http::status::ok) {
        out.error = "http status " + std::to_string(out.status);
        return out;
    }
    out.ok = true;
    return out;
}

}  // namespace

HttpGetResult http_get(const std::string& url, const std::string& proxy_url,
                       std::chrono::milliseconds timeout) {
    HttpGetResult out;
    try {
        const auto parsed = parse_http_url(url);
        if (!parsed || parsed->host.empty()) {
            out.error = "invalid url";
            return out;
        }
        const HttpUrl& target = *parsed;
        const auto proxy = parse_proxy_url(proxy_url);
        if (proxy && !target.tls) {
            out.error = "proxy requires https url";
            return out;
        }

        net::io_context ioc;
        tcp::resolver resolver(ioc);

        if (target.tls) {
            ssl::context ctx(ssl::context::tlsv12_client);
            ctx.set_default_verify_paths();
            beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);
            beast::get_lowest_layer(stream).expires_after(timeout);

            if (proxy) {
                connect_via_proxy(beast::get_lowest_layer(stream), resolver, *proxy,
                                  target);
            } else {
                auto eps = resolver.resolve(tcp::v4(), target.host, target.port);
                beast::get_lowest_layer(stream).connect(eps);
            }

            if (!SSL_set_tlsext_host_name(stream.native_handle(), target.host.c_str())) {
                out.error = "SNI failed";
                return out;
            }
            stream.handshake(ssl::stream_base::client);
            out = write_and_read(stream, target);
        } else {
            beast::tcp_stream stream(ioc);
            stream.expires_after(timeout);
            auto eps = resolver.resolve(tcp::v4(), target.host, target.port);
            stream.connect(eps);
            out = write_and_read(stream, target);
        }
    } catch (const std::exception& e) {
        out.ok = false;
        out.error = e.what();
    }
    return out;
}

std::optional<int64_t> parse_binance_server_time_us(const std::string& body) {
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("serverTime")) {
        return std::nullopt;
    }
    const auto& v = j["serverTime"];
    int64_t ms = 0;
    if (v.is_number_unsigned()) {
        const uint64_t u = v.get<uint64_t>();
        if (u == 0 || u > static_cast<uint64_t>(INT64_MAX / 1000)) {
            return std::nullopt;
        }
        ms = static_cast<int64_t>(u);
    } else if (v.is_number_integer()) {
        ms = v.get<int64_t>();
        if (ms <= 0 || ms > INT64_MAX / 1000) {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }
    return ms * 1000;
}

}  // namespace mds
