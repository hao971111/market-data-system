#pragma once

#include <string>
#include <functional>
#include <memory>
#include <atomic>
#include <thread>

namespace mds {

// 消息回调类型
using MessageCallback = std::function<void(const std::string& message)>;
using ErrorCallback = std::function<void(const std::string& error)>;
using ConnectCallback = std::function<void()>;

class WebSocketClient {
public:
    WebSocketClient();
    ~WebSocketClient();
    
    // 禁止拷贝
    WebSocketClient(const WebSocketClient&) = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;
    
    // 连接到WebSocket服务器
    // url格式: wss://stream.binance.com:9443/ws/btcusdt@trade
    // proxy_url: HTTP CONNECT 代理，格式 http://host:port，空字符串=直连
    // ping_interval_ms: 心跳Ping间隔（兜底保活）
    // no_data_timeout_ms: 无数据超时（主要断线检测手段）
    bool connect(const std::string& url,
                 const std::string& proxy_url = "",
                 int ping_interval_ms = 1000,
                 int no_data_timeout_ms = 3000);
    
    // 断开连接
    void disconnect();
    
    // 发送消息
    bool send(const std::string& message);
    
    // 是否已连接
    bool is_connected() const { return connected_.load(); }
    
    // 设置回调
    // 注意：回调在内部线程调用，不要在回调里：
    //   1. 销毁本对象（会死锁，因为析构要join自己）
    //   2. 阻塞太久（会阻塞读线程）
    // 如需销毁/重连，建议在回调里仅设置标志位，主线程处理
    void set_message_callback(MessageCallback cb) { on_message_ = std::move(cb); }
    void set_error_callback(ErrorCallback cb) { on_error_ = std::move(cb); }
    void set_connect_callback(ConnectCallback cb) { on_connect_ = std::move(cb); }
    void set_disconnect_callback(ConnectCallback cb) { on_disconnect_ = std::move(cb); }

private:
    std::atomic<bool> connected_{false};
    
    MessageCallback on_message_;
    ErrorCallback on_error_;
    ConnectCallback on_connect_;
    ConnectCallback on_disconnect_;
    
    // 实现细节（后续填充）
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mds
