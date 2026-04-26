#include "websocket_client.h"

#include <mutex>
#include <condition_variable>
#include <chrono>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/beast/http.hpp>
#include <iostream>
#include <optional>
#include <regex>

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

namespace mds {

// 解析URL: wss://host:port/path
struct UrlParts {
    std::string host;
    std::string port;
    std::string path;
    
    static UrlParts parse(const std::string& url) {
        UrlParts parts;
        std::regex re(R"(wss?://([^:/]+):?(\d*)(/.*)?)");
        std::smatch match;
        if (std::regex_match(url, match, re)) {
            parts.host = match[1];
            parts.port = match[2].length() > 0 ? match[2].str() : "443";
            parts.path = match[3].length() > 0 ? match[3].str() : "/";
        }
        return parts;
    }
};

// HTTP CONNECT 代理目标
struct ProxyTarget {
    std::string host;
    std::string port;
};

// 解析代理 URL 字符串。支持格式：http://host:port 或 http://host:port/
// 空字符串 = 直连（返回 nullopt）
// 不支持：用户名密码（http://user:pass@host:port）、SOCKS5、HTTPS 代理（连代理本身用 TLS）
// 这些扩展见 EXTENSIONS.md
// 注意：proxy_url 由 Config 在启动时读完，运行期间不变；这里不再读 getenv
// 安全：异常信息绝不回显原始 proxy_url。误填 user:pass@ 这种格式时，凭据会经
//      on_error_ 进 stderr/日志/监控系统，属于敏感数据泄漏。只描述格式要求即可。
static std::optional<ProxyTarget> parse_proxy_url(const std::string& proxy_url) {
    if (proxy_url.empty()) return std::nullopt;

    static const std::regex re(R"(^https?://([^:/]+):(\d+)/?$)");
    std::smatch m;
    if (!std::regex_match(proxy_url, m, re)) {
        throw std::runtime_error(
            "Invalid proxy_url format (expected http://host:port, no auth/path)");
    }
    return ProxyTarget{m[1].str(), m[2].str()};
}

// Pimpl实现类
class WebSocketClient::Impl {
public:
    Impl() : ctx_(ssl::context::tlsv12_client), resolver_(ioc_), 
             ws_(net::make_strand(ioc_), ctx_) {
        ctx_.set_default_verify_paths();
    }
    
    ~Impl() {
        running_ = false;
        heartbeat_cv_.notify_all();
        if (read_thread_.joinable())      read_thread_.join();
        if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
    }
    
    bool connect(const std::string& url, const std::string& proxy_url,
                 WebSocketClient* owner,
                 int ping_interval_ms, int no_data_timeout_ms) {
        owner_ = owner;
        url_parts_ = UrlParts::parse(url);
        disconnect_notified_ = false;
        ping_interval_ms_ = ping_interval_ms;
        no_data_timeout_ms_ = no_data_timeout_ms;
        
        bool success = false;
        {
            std::lock_guard<std::mutex> lk(lifecycle_mutex_);
            try {
                beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(10));

                // 代理由 Config 在启动时读完（配置 > 环境变量），传递到这里
                auto proxy = parse_proxy_url(proxy_url);
                if (proxy) {
                    // 走 HTTP CONNECT 隧道：先连代理，再请求建立到目标的 TCP 隧道
                    std::cout << "[WS] Using HTTP proxy "
                              << proxy->host << ":" << proxy->port << std::endl;

                    // 整段包 try/catch：CONNECT 任一步失败都要把已经 open 的 socket 关掉，
                    // 否则 ws_ 复用时下一次 connect() 会撞 already_open，永久无法重连
                    try {
                        auto proxy_eps = resolver_.resolve(tcp::v4(), proxy->host, proxy->port);
                        beast::get_lowest_layer(ws_).connect(proxy_eps);

                        const std::string target = url_parts_.host + ":" + url_parts_.port;
                        // 用 beast::http 构造 CONNECT 请求并写入 socket
                        http::request<http::empty_body> conn_req{http::verb::connect, target, 11};
                        conn_req.set(http::field::host, target);
                        conn_req.set(http::field::proxy_connection, "keep-alive");
                        http::write(beast::get_lowest_layer(ws_), conn_req);

                        // 只读 header（CONNECT 200 没 body，server 也不会主动发更多 bytes）
                        // skip(true) 防止 parser 等 body 永远不返回
                        beast::flat_buffer buf;
                        http::response_parser<http::empty_body> parser;
                        parser.skip(true);
                        http::read_header(beast::get_lowest_layer(ws_), buf, parser);
                        const auto& proxy_res = parser.get();
                        if (proxy_res.result() != http::status::ok) {
                            throw std::runtime_error("Proxy CONNECT failed: "
                                + std::to_string(static_cast<unsigned>(proxy_res.result()))
                                + " " + std::string(proxy_res.reason()));
                        }

                        // 隧道字节边界检查：read_header 基于 read_some，可能把 \r\n\r\n
                        // 之后的字节也拉进 buf。这些字节本应是 TLS 流的开头，但 SSL
                        // handshake 不会消费 buf，会导致 TLS 第一字节错位 -> 神秘的
                        // "wrong version number" 报错。早抛清晰错误比让 TLS 自己报强。
                        // RFC 7231：CONNECT 200 响应没有 body，正常代理不会有残留字节
                        if (buf.size() != 0) {
                            throw std::runtime_error("Proxy returned "
                                + std::to_string(buf.size())
                                + " unexpected bytes after CONNECT response");
                        }
                    } catch (...) {
                        beast::error_code ec;
                        beast::get_lowest_layer(ws_).socket().close(ec);
                        throw;  // 让外层 catch (const std::exception&) 走 on_error_ 回调
                    }
                } else {
                    // 直连：强制 IPv4 解析（WSL2 NAT 模式无出站 IPv6 路由）
                    auto results = resolver_.resolve(
                        tcp::v4(), url_parts_.host, url_parts_.port);
                    beast::get_lowest_layer(ws_).connect(results);
                }
                
                if (!SSL_set_tlsext_host_name(ws_.next_layer().native_handle(), 
                                              url_parts_.host.c_str())) {
                    return false;
                }
                ws_.next_layer().handshake(ssl::stream_base::client);
                
                beast::get_lowest_layer(ws_).expires_never();
                ws_.set_option(websocket::stream_base::decorator(
                    [](websocket::request_type& req) {
                        req.set(http::field::user_agent, "mds-client/1.0");
                    }));
                ws_.handshake(url_parts_.host + ":" + url_parts_.port, url_parts_.path);
                
                running_ = true;
                last_recv_time_ms_ = now_ms();
                read_thread_ = std::thread([this]() { read_loop(); });
                heartbeat_thread_ = std::thread([this]() { heartbeat_loop(); });
                success = true;
                
            } catch (const std::exception& e) {
                if (owner_->on_error_) {
                    owner_->on_error_(e.what());
                }
            }
        }  // lifecycle_mutex_ 在此释放
        
        // on_connect_ 在锁外调用，防止回调中调用 disconnect() 导致死锁
        // 先设置 connected_，确保回调中 is_connected() 返回 true
        if (success) {
            owner_->connected_ = true;
            if (owner_->on_connect_) {
                owner_->on_connect_();
            }
        }
        return success;
    }
    
    void disconnect() {
        std::lock_guard<std::mutex> lk(lifecycle_mutex_);
        running_ = false;
        
        heartbeat_cv_.notify_all();
        
        try {
            std::lock_guard<std::mutex> lock(write_mutex_);
            ws_.close(websocket::close_code::normal);
        } catch (...) {}
        
        if (read_thread_.joinable()) {
            read_thread_.join();
        }
        if (heartbeat_thread_.joinable()) {
            heartbeat_thread_.join();
        }
        
        // 确保 on_disconnect_ 只调用一次
        notify_disconnect();
    }
    
    void notify_disconnect() {
        bool expected = false;
        if (disconnect_notified_.compare_exchange_strong(expected, true)) {
            owner_->connected_ = false;
            if (owner_->on_disconnect_) {
                owner_->on_disconnect_();
            }
        }
    }
    
    bool send(const std::string& msg) {
        try {
            std::lock_guard<std::mutex> lock(write_mutex_);
            ws_.write(net::buffer(msg));
            return true;
        } catch (const std::exception& e) {
            if (owner_->on_error_) {
                owner_->on_error_(e.what());
            }
            return false;
        }
    }

private:
    void read_loop() {
        beast::flat_buffer buffer;
        while (running_) {
            try {
                buffer.clear();
                ws_.read(buffer);
                
                // 任何消息到达都算"心跳有效"，更新最后接收时间
                last_recv_time_ms_ = now_ms();
                
                std::string msg = beast::buffers_to_string(buffer.data());
                if (owner_->on_message_) {
                    owner_->on_message_(msg);
                }
            } catch (const beast::system_error& e) {
                if (running_) {
                    if (owner_->on_error_) {
                        owner_->on_error_(e.what());
                    }
                }
                break;
            }
        }
        running_ = false;
        heartbeat_cv_.notify_all();  // 唤醒心跳线程，让它退出
        
        notify_disconnect();
    }
    
    // 心跳监控循环
    // 职责：
    //   1. 检查 no_data_timeout（主要手段）
    //   2. 定期发 Ping（兜底保活）
    void heartbeat_loop() {
        using namespace std::chrono;
        
        while (running_) {
            // 可被中断的等待：被唤醒或超时
            {
                std::unique_lock<std::mutex> lock(heartbeat_mutex_);
                heartbeat_cv_.wait_for(lock, milliseconds(ping_interval_ms_),
                                       [this] { return !running_.load(); });
            }
            if (!running_) break;
            
            int64_t since_last_recv = now_ms() - last_recv_time_ms_.load();
            
            // 1. 无数据超时检测（主要手段）
            if (since_last_recv > no_data_timeout_ms_) {
                if (owner_->on_error_) {
                    owner_->on_error_("Heartbeat timeout: no data for " 
                                      + std::to_string(since_last_recv) + "ms");
                }
                // 关闭底层socket，让read_loop的阻塞read抛异常退出
                try {
                    beast::get_lowest_layer(ws_).close();
                } catch (...) {}
                running_ = false;
                break;
            }
            
            // 2. 定期发 Ping（兜底；大部分时候数据流会持续来，Ping用不上）
            try {
                std::lock_guard<std::mutex> lock(write_mutex_);
                ws_.ping({});
            } catch (const std::exception&) {
                // Ping失败不直接断，等 no_data_timeout 判断
            }
        }
    }
    
    std::mutex lifecycle_mutex_;  // 串行化 connect() 和 disconnect()，防止并发访问 ws_
    std::mutex write_mutex_;
    std::atomic<bool> disconnect_notified_{false};
    
    // 心跳相关
    int ping_interval_ms_ = 1000;
    int no_data_timeout_ms_ = 3000;
    std::atomic<int64_t> last_recv_time_ms_{0};  // steady_clock的毫秒数
    std::thread heartbeat_thread_;
    std::mutex heartbeat_mutex_;
    std::condition_variable heartbeat_cv_;
    
    static int64_t now_ms() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(
            steady_clock::now().time_since_epoch()).count();
    }
    
    net::io_context ioc_;
    ssl::context ctx_;
    tcp::resolver resolver_;
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws_;
    
    UrlParts url_parts_;
    WebSocketClient* owner_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread read_thread_;
};

// WebSocketClient 实现
WebSocketClient::WebSocketClient() : impl_(std::make_unique<Impl>()) {}

WebSocketClient::~WebSocketClient() {
    disconnect();
}

bool WebSocketClient::connect(const std::string& url,
                              const std::string& proxy_url,
                              int ping_interval_ms,
                              int no_data_timeout_ms) {
    if (impl_->connect(url, proxy_url, this, ping_interval_ms, no_data_timeout_ms)) {
        connected_ = true;
        return true;
    }
    return false;
}

void WebSocketClient::disconnect() {
    if (connected_) {
        impl_->disconnect();
        connected_ = false;
    }
}

bool WebSocketClient::send(const std::string& message) {
    return connected_ && impl_->send(message);
}

}  // namespace mds
