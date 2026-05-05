#include "connection_manager.h"
#include "../parser/parser.h"
#include <chrono>
#include <iostream>
#include <algorithm>
#include <nlohmann/json.hpp>

namespace mds {

namespace {

class PipelineLatencyRecorder {
public:
    explicit PipelineLatencyRecorder(LatencyHistogram& histogram)
        : histogram_(histogram), start_(std::chrono::steady_clock::now()) {}

    ~PipelineLatencyRecorder() {
        const auto end = std::chrono::steady_clock::now();
        const auto lat_us = std::chrono::duration_cast<std::chrono::microseconds>(
            end - start_).count();
        if (lat_us >= 0) {
            histogram_.record(static_cast<uint64_t>(lat_us));
        }
    }

    PipelineLatencyRecorder(const PipelineLatencyRecorder&) = delete;
    PipelineLatencyRecorder& operator=(const PipelineLatencyRecorder&) = delete;

private:
    LatencyHistogram& histogram_;
    std::chrono::steady_clock::time_point start_;
};

}  // namespace

ConnectionManager::ConnectionManager(const Config& config, Metrics& metrics)
    : config_(config), metrics_(metrics) {}

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
            client_->set_message_callback([this](const std::string& msg) {
                on_raw_message(msg);
            });
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

        // 监控：记录每一次尝试（包括失败）。relaxed 即可——这里不参与同步，
        // 只是给 reporter 读取做趋势分析。
        metrics_.connect_attempts.fetch_add(1, std::memory_order_relaxed);

        bool ok = client_->connect(url, config_.proxy_url,
                                   config_.ping_interval_ms, config_.no_data_timeout_ms);

        if (ok) {
            std::cout << "[ConnectionManager] Connected." << std::endl;
            metrics_.connect_successes.fetch_add(1, std::memory_order_relaxed);
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

// 解析原始消息并分发给上层回调
//
// Combined stream 端点（/stream）的消息格式：
//   {"stream": "btcusdt@depth20@100ms", "data": { ... 真正的业务字段 ... }}
//
// 这里我们：
//   1) 剥外层，校验 stream / data 字段都在
//   2) 从 stream 名前缀拿 symbol（btcusdt@xxx -> btcusdt），解决了之前
//      depth 消息没 symbol 字段、只能瞎猜导致全部贴标 btcusdt 的 bug
//   3) 按 @后缀 区分流类型，分发到 trade / orderbook 解析器
//   4) 订阅 ack 等控制消息没有 stream 字段，直接丢弃（不算错误）
void ConnectionManager::on_raw_message(const std::string& msg) {
    // 内部处理延迟：从 on_raw_message 进入到函数退出。
    // RAII 保证 parse error / control ack / trade / orderbook 等所有返回路径都记录。
    PipelineLatencyRecorder pipeline_timer(metrics_.pipeline_latency);

    // 监控：每条入站消息（含控制消息）都计入。reporter 用 delta 算 msgs/s
    metrics_.msgs_recv.fetch_add(1, std::memory_order_relaxed);

    nlohmann::json outer;
    try {
        outer = nlohmann::json::parse(msg);
    } catch (const nlohmann::json::exception& e) {
        // 外层 JSON 解析失败：外层都坏了基本就是协议异常，计 parse_error
        metrics_.parse_errors.fetch_add(1, std::memory_order_relaxed);
        std::cerr << "[ConnectionManager] outer JSON parse error: "
                  << e.what() << std::endl;
        return;
    }

    // 控制消息（订阅 ack: {"result":null,"id":1}）没有 stream/data 字段，正常忽略
    // 注意：这条不算 parse_error——它是合法的控制帧
    const auto stream_it = outer.find("stream");
    const auto data_it = outer.find("data");
    if (stream_it == outer.end() || data_it == outer.end()) {
        return;
    }

    // stream 形如 "btcusdt@trade" 或 "btcusdt@depth20@100ms"
    // 第一个 '@' 之前是 symbol，之后是流类型描述
    std::string stream_name;
    try {
        stream_name = stream_it->get<std::string>();
    } catch (const nlohmann::json::exception&) {
        metrics_.parse_errors.fetch_add(1, std::memory_order_relaxed);
        return;  // stream 字段类型不对，丢弃
    }
    const auto at_pos = stream_name.find('@');
    if (at_pos == std::string::npos || at_pos == 0) {
        metrics_.parse_errors.fetch_add(1, std::memory_order_relaxed);
        return;  // 格式异常，丢弃
    }
    const std::string symbol      = stream_name.substr(0, at_pos);
    const std::string stream_type = stream_name.substr(at_pos + 1);

    // 用户回调隔离：on_trade_ / on_orderbook_ 是上层注入的 lambda，
    // 任何抛出的异常都不能冲垮 WebSocket 读线程（否则 std::terminate 进程死）。
    // safe_invoke 把异常吞进 callback_errors 计数器，让坏的一条不影响后续。
    // 把 metrics 一起传进 lambda（捕获 &，但避免 [&] 捕获太多东西）
    Metrics& metrics = metrics_;
    auto safe_invoke = [&metrics](auto& cb, auto& payload, const char* tag) {
        if (!cb) return;
        try {
            cb(payload);
        } catch (const std::exception& e) {
            metrics.callback_errors.fetch_add(1, std::memory_order_relaxed);
            std::cerr << "[ConnectionManager] " << tag
                      << " callback threw: " << e.what() << std::endl;
        } catch (...) {
            metrics.callback_errors.fetch_add(1, std::memory_order_relaxed);
            std::cerr << "[ConnectionManager] " << tag
                      << " callback threw unknown exception" << std::endl;
        }
    };

    // rfind(prefix, 0) 是判前缀的标准 C++ 写法；starts_with 要 C++20
    if (stream_type.rfind("trade", 0) == 0) {
        if (auto t = Parser::parse_trade(*data_it)) {
            // 端到端延迟：本机时间 - 交易所时间戳。
            // 用 system_clock 不用 steady_clock：因为 trade.timestamp_us 也是
            // wall-clock 微秒（来自 binance），两边口径必须一致。
            // 时钟不同步可能让差值为负，丢弃即可——record 接 uint64_t，
            // 这里强转 negative 会变成天文数字、把 max 顶到爆，必须先挡住。
            const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            const int64_t lat_us = now_us - t->timestamp_us;
            if (lat_us >= 0) {
                metrics_.trade_latency.record(static_cast<uint64_t>(lat_us));
            }
            metrics_.trades_parsed.fetch_add(1, std::memory_order_relaxed);
            safe_invoke(on_trade_, *t, "trade");
        } else {
            // 业务字段缺失/格式错：parse_trade 返回 nullopt
            metrics_.parse_errors.fetch_add(1, std::memory_order_relaxed);
        }
    } else if (stream_type.rfind("depth", 0) == 0) {
        if (auto ob = Parser::parse_orderbook(*data_it, symbol)) {
            metrics_.orderbooks_parsed.fetch_add(1, std::memory_order_relaxed);
            safe_invoke(on_orderbook_, *ob, "orderbook");
        } else {
            metrics_.parse_errors.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // 其他流类型（kline / aggTrade 等）暂未启用，静默忽略（不计 error）
}

// 指数退避：100ms, 200ms, 400ms, 800ms, ... 最大 30s
int64_t ConnectionManager::calc_backoff_ms(int attempt) const {
    int64_t base = config_.reconnect_interval_ms;  // 100ms
    int64_t backoff = base * (1LL << std::min(attempt, 8));  // 最多左移8位
    return std::min(backoff, (int64_t)30000);  // 上限 30s
}

}  // namespace mds
