#include "connection_manager.h"
#include <iostream>
#include <algorithm>
#include <nlohmann/json.hpp>

namespace mds {

ConnectionManager::ConnectionManager(const Config& config) 
    : config_(config) {}

ConnectionManager::~ConnectionManager() {
    stop();
}

void ConnectionManager::start(const std::string& url) {
    // 严重-2修复：防重入，已启动则忽略
    if (running_.exchange(true)) return;
    reconnect_thread_ = std::thread([this, url]() {
        reconnect_loop(url);
    });
}

void ConnectionManager::stop() {
    running_ = false;
    
    // 严重-3修复：先 disconnect 客户端，触发 on_disconnect_ → cv_.notify_all()
    // 这样 reconnect_loop 里的 cv_.wait 能被唤醒，join 才不会挂死
    {
        std::lock_guard<std::mutex> lock(client_mutex_);  // 严重-4修复
        if (client_) {
            client_->disconnect();
        }
    }
    
    cv_.notify_all();  // 兜底：覆盖"正在退避等待"的场景
    if (reconnect_thread_.joinable()) {
        reconnect_thread_.join();
    }
}

void ConnectionManager::reconnect_loop(const std::string& url) {
    int attempt = 0;  // 重试次数，同时用作退避指数

    while (running_) {
        // 每次重连重建 WebSocketClient（旧状态不可复用）
        {
            std::lock_guard<std::mutex> lock(client_mutex_);  // 严重-4修复
            client_ = std::make_unique<WebSocketClient>();
            client_->set_message_callback(on_message_);
            client_->set_disconnect_callback([this]() {
                need_reconnect_ = true;
                cv_.notify_all();
            });
            client_->set_error_callback([](const std::string& err) {
                std::cerr << "[WS ERROR] " << err << std::endl;
            });
        }

        // 轻微-1修复：symbols 为空时跳过订阅
        if (config_.symbols.empty()) {
            std::cerr << "[ConnectionManager] No symbols configured, aborting." << std::endl;
            break;
        }

        std::cout << "[ConnectionManager] Connecting (attempt " 
                  << attempt + 1 << ")..." << std::endl;

        bool ok = client_->connect(url, config_.ping_interval_ms, config_.no_data_timeout_ms);

        if (ok) {
            std::cout << "[ConnectionManager] Connected." << std::endl;
            attempt = 0;
            need_reconnect_ = false;
            
            // 连接成功后发送订阅请求
            std::string sub_msg = build_subscribe_msg(config_.symbols);
            bool sent = client_->send(sub_msg);
            // 中等-1修复：订阅失败直接触发重连
            if (!sent) {
                std::cerr << "[ConnectionManager] Subscribe failed, reconnecting." << std::endl;
                need_reconnect_ = true;
            } else {
                std::cout << "[ConnectionManager] Subscribed: " << sub_msg << std::endl;
                // 等待断线通知
                std::unique_lock<std::mutex> lock(cv_mutex_);
                cv_.wait(lock, [this]() {
                    return need_reconnect_.load() || !running_.load();
                });
            }
        } else {
            std::cerr << "[ConnectionManager] Connect failed." << std::endl;
        }

        if (!running_) break;

        // 检查最大重试次数（0 表示无限）
        if (config_.max_reconnect_attempts > 0 &&
            attempt >= config_.max_reconnect_attempts) {
            std::cerr << "[ConnectionManager] Max reconnect attempts reached." << std::endl;
            break;
        }

        // 指数退避等待
        int64_t wait_ms = calc_backoff_ms(attempt);
        std::cout << "[ConnectionManager] Reconnecting in " << wait_ms << "ms..." << std::endl;
        {
            std::unique_lock<std::mutex> lock(cv_mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(wait_ms),
                         [this]() { return !running_.load(); });
        }
        attempt++;
    }
}

// 构造 Binance 订阅消息
// 每个 symbol 订阅两个流：
//   @trade      — 逐笔成交
//   @depth20    — 订单簿20档快照（100ms更新一次）
std::string ConnectionManager::build_subscribe_msg(
        const std::vector<std::string>& symbols) const {
    nlohmann::json params = nlohmann::json::array();
    for (const auto& sym : symbols) {
        params.push_back(sym + "@trade");
        params.push_back(sym + "@depth20@100ms");
    }
    nlohmann::json msg = {
        {"method", "SUBSCRIBE"},
        {"params", params},
        {"id", 1}
    };
    return msg.dump();
}

// 指数退避：100ms, 200ms, 400ms, 800ms, ... 最大 30s
int64_t ConnectionManager::calc_backoff_ms(int attempt) const {
    int64_t base = config_.reconnect_interval_ms;  // 100ms
    int64_t backoff = base * (1LL << std::min(attempt, 8));  // 最多左移8位
    return std::min(backoff, (int64_t)30000);  // 上限 30s
}

}  // namespace mds
