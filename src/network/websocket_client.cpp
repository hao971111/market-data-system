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
#include <iostream>
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
    
    bool connect(const std::string& url, WebSocketClient* owner,
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
                
                auto results = resolver_.resolve(url_parts_.host, url_parts_.port);
                beast::get_lowest_layer(ws_).connect(results);
                
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
                              int ping_interval_ms,
                              int no_data_timeout_ms) {
    if (impl_->connect(url, this, ping_interval_ms, no_data_timeout_ms)) {
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
