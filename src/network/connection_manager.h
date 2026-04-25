#pragma once

#include "websocket_client.h"
#include "../config/config.h"
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <string>

namespace mds {

// ConnectionManager 职责：
//   - 管理 WebSocketClient 的生命周期
//   - 断线后自动重连（指数退避）
//   - 对上层屏蔽重连细节，上层只看到"数据流"
class ConnectionManager {
public:
    explicit ConnectionManager(const Config& config);
    ~ConnectionManager();

    // 启动连接（会自动重连，直到 stop() 或达到最大重试次数）
    void start(const std::string& url);
    
    // 停止并清理
    void stop();

    // 设置消息回调（连接期间持续有效，重连后自动绑定）
    void set_message_callback(MessageCallback cb) { on_message_ = std::move(cb); }

private:
    void reconnect_loop(const std::string& url);
    int64_t calc_backoff_ms(int attempt) const;
    std::string build_subscribe_msg(const std::vector<std::string>& symbols) const;

    const Config& config_;
    MessageCallback on_message_;

    std::unique_ptr<WebSocketClient> client_;
    std::mutex client_mutex_;                  // 保护 client_ 的跨线程访问
    std::atomic<bool> running_{false};
    std::atomic<bool> need_reconnect_{false};

    std::thread reconnect_thread_;
    std::mutex cv_mutex_;
    std::condition_variable cv_;
};

}  // namespace mds
