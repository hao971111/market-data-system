#include "network/https_client.h"

#include <string>
#include <thread>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <gtest/gtest.h>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

namespace {

void serve_one(net::io_context& ioc, tcp::acceptor& acc, http::status status,
               std::string body) {
    try {
        beast::tcp_stream sock(ioc);
        acc.accept(sock.socket());
        beast::flat_buffer buf;
        http::request<http::string_body> req;
        http::read(sock, buf, req);
        http::response<http::string_body> res{status, 11};
        res.set(http::field::content_type, "application/json");
        res.set(http::field::connection, "close");
        res.body() = std::move(body);
        res.prepare_payload();
        http::write(sock, res);
    } catch (...) {
        // 对端先失败时 close(acceptor) 会打断 accept，这里吞掉
    }
}

}  // namespace

TEST(HttpsClient, GetOkFromLocalHttp) {
    net::io_context ioc;
    tcp::acceptor acc(ioc, tcp::endpoint(tcp::v4(), 0));
    const auto port = acc.local_endpoint().port();
    std::thread server([&] { serve_one(ioc, acc, http::status::ok,
                                       R"({"serverTime":1500000000000})"); });

    const auto r = mds::http_get("http://127.0.0.1:" + std::to_string(port) + "/api/v3/time");
    beast::error_code ec;
    acc.close(ec);
    server.join();

    EXPECT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.status, 200);
    const auto ts = mds::parse_binance_server_time_us(r.body);
    ASSERT_TRUE(ts.has_value());
    EXPECT_EQ(*ts, 1500000000000LL * 1000);
}

TEST(HttpsClient, GetNon200IsNotOk) {
    net::io_context ioc;
    tcp::acceptor acc(ioc, tcp::endpoint(tcp::v4(), 0));
    const auto port = acc.local_endpoint().port();
    std::thread server([&] { serve_one(ioc, acc, http::status::internal_server_error, "{}"); });

    const auto r = mds::http_get("http://127.0.0.1:" + std::to_string(port) + "/");
    beast::error_code ec;
    acc.close(ec);
    server.join();

    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.status, 500);
}

TEST(HttpsClient, ConnectRefused) {
    const auto r = mds::http_get("http://127.0.0.1:1/");
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.empty());
}

TEST(HttpsClient, InvalidUrl) {
    const auto r = mds::http_get("not-a-url");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "invalid url");
}

TEST(ParseBinanceServerTime, ValidAndRejects) {
    EXPECT_EQ(mds::parse_binance_server_time_us(R"({"serverTime":1499827319559})").value(),
              1499827319559LL * 1000);
    EXPECT_FALSE(mds::parse_binance_server_time_us("").has_value());
    EXPECT_FALSE(mds::parse_binance_server_time_us("{}").has_value());
    EXPECT_FALSE(mds::parse_binance_server_time_us(R"({"serverTime":"x"})").has_value());
    EXPECT_FALSE(mds::parse_binance_server_time_us(R"({"serverTime":0})").has_value());
    EXPECT_FALSE(mds::parse_binance_server_time_us(R"({"serverTime":-1})").has_value());
    EXPECT_FALSE(mds::parse_binance_server_time_us("not json").has_value());
}
