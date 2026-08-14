#include "connection_manager.h"
#include "../monitor/cpu_affinity.h"
#include "../parser/parser.h"
#include <chrono>
#include <iostream>
#include <algorithm>
#include <nlohmann/json.hpp>

namespace mds {

namespace {

class PipelineLatencyRecorder {
public:
    // 默认构造：起点 = 当前时间（用于不需要与外部分段对齐的场景）
    explicit PipelineLatencyRecorder(LatencyHistogram& histogram)
        : histogram_(histogram), start_(std::chrono::steady_clock::now()) {}

    // 显式起点构造：让 pipeline 与 SEG 段共用同一个 t_enter，
    // 这样口径上 pipeline.start == json_parse.start，避免出现"pipeline 起点早于
    // 三段总和起点"导致的几十~几百 ns 系统性偏差。
    PipelineLatencyRecorder(LatencyHistogram& histogram,
                            std::chrono::steady_clock::time_point start,
                            std::atomic<uint64_t>* over_500us = nullptr,
                            std::atomic<uint64_t>* over_1000us = nullptr,
                            std::atomic<uint64_t>* anomaly_count = nullptr)
        : histogram_(histogram),
          start_(start),
          over_500us_(over_500us),
          over_1000us_(over_1000us),
          anomaly_count_(anomaly_count) {}

    ~PipelineLatencyRecorder() {
        const auto end = std::chrono::steady_clock::now();
        const auto lat_us = std::chrono::duration_cast<std::chrono::microseconds>(
            end - start_).count();
        if (lat_us >= 0) {
            const uint64_t v = static_cast<uint64_t>(lat_us);
            histogram_.record(v);
            if (over_500us_ != nullptr && v > 500) {
                over_500us_->fetch_add(1, std::memory_order_relaxed);
            }
            if (over_1000us_ != nullptr && v > 1000) {
                over_1000us_->fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            // steady_clock 正常不会回拨；非零说明有系统级时钟异常
            if (anomaly_count_ != nullptr) {
                anomaly_count_->fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    PipelineLatencyRecorder(const PipelineLatencyRecorder&) = delete;
    PipelineLatencyRecorder& operator=(const PipelineLatencyRecorder&) = delete;

private:
    LatencyHistogram& histogram_;
    std::chrono::steady_clock::time_point start_;
    std::atomic<uint64_t>* over_500us_ = nullptr;
    std::atomic<uint64_t>* over_1000us_ = nullptr;
    std::atomic<uint64_t>* anomaly_count_ = nullptr;
};

// 把两个 steady_clock 时间点的差值（微秒）落到对应直方图。
// 负值（极少见的时钟回拨）不记录延迟，但计入 anomaly_count 以便感知。
inline void record_segment_us(LatencyHistogram& h,
                              std::chrono::steady_clock::time_point a,
                              std::chrono::steady_clock::time_point b,
                              std::atomic<uint64_t>* anomaly_count = nullptr) {
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
    if (us >= 0) {
        h.record(static_cast<uint64_t>(us));
    } else if (anomaly_count != nullptr) {
        anomaly_count->fetch_add(1, std::memory_order_relaxed);
    }
}

inline void update_atomic_max(std::atomic<uint64_t>& target, uint64_t value) {
    uint64_t prev = target.load(std::memory_order_relaxed);
    while (value > prev &&
           !target.compare_exchange_weak(prev, value,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {}
}

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

        const auto connect_begin = std::chrono::steady_clock::now();
        bool ok = client_->connect(url, config_.proxy_url,
                                   config_.ping_interval_ms, config_.no_data_timeout_ms);
        const auto connect_end = std::chrono::steady_clock::now();
        const auto connect_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            connect_end - connect_begin).count();
        if (connect_ms >= 0) {
            const uint64_t v = static_cast<uint64_t>(connect_ms);
            metrics_.connect_duration_samples.fetch_add(1, std::memory_order_relaxed);
            metrics_.connect_duration_ms_total.fetch_add(v, std::memory_order_relaxed);
            update_atomic_max(metrics_.connect_duration_ms_max, v);
        } else {
            metrics_.clock_anomaly_count.fetch_add(1, std::memory_order_relaxed);
        }

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
    // 第一次进入此函数时，把"消息处理线程"钉到 config_.pin_cpu。
    // 这是 INT 延迟实测的那个线程（live = io_context；bench = main 线程），
    // 在这里 pin 才能保证 pin 的就是被测线程，不会传染给 writer 后台线程
    // （它们在 main 启动时已经 spawn 完，affinity 不受当前线程影响）。
    //
    // 单线程语义：当前只有一条路径调用 on_raw_message（见头文件 pin_attempted_
    // 注释），首条消息时置位并尝试 pin 一次；失败后也置位，不重试（避免刷屏）。
    if (!pin_attempted_) {
        pin_attempted_ = true;
        if (config_.pin_cpu >= 0) {
            std::string err;
            if (pin_current_thread_to_cpu(config_.pin_cpu, err)) {
                std::cerr << "[ConnectionManager] pinned message thread to cpu "
                          << config_.pin_cpu << std::endl;
            } else {
                std::cerr << "[ConnectionManager] pin_cpu="
                          << config_.pin_cpu << " failed (" << err
                          << "), continuing unpinned." << std::endl;
            }
        }
    }

    // 分段计时基准：t_enter -> json::parse 完成 -> 业务结构解析完成 -> callback 完成。
    // 只在命中 trade/orderbook 分支时记录 biz/callback 段，避免控制消息污染分布。
    // 先取 t_enter，再用同一个时间点去构造 pipeline_timer，保证
    // pipeline 与 SEG 段共用起点；口径上恒有 pipeline ≥ json+biz+cb，
    // 差值 = 3 次 record_segment_us 调用开销 + 函数 return + RAII 析构（量级几百 ns）。
    const auto t_enter = std::chrono::steady_clock::now();

    // 内部处理延迟：起点 = t_enter，终点 = 函数退出（RAII 析构）。
    // RAII 保证 parse error / control ack / trade / orderbook 等所有返回路径都记录。
    PipelineLatencyRecorder pipeline_timer(metrics_.pipeline_latency, t_enter,
                                           &metrics_.pipeline_over_500us,
                                           &metrics_.pipeline_over_1000us,
                                           &metrics_.clock_anomaly_count);

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
    const auto t_after_json = std::chrono::steady_clock::now();

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
    // 统一小写：stream 前缀本就小写，这里再规范一次；Trade 的 s 在 Parser 里也会转小写
    std::string symbol = stream_name.substr(0, at_pos);
    for (char& ch : symbol) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    const std::string stream_type = stream_name.substr(at_pos + 1);

    // 不在订阅列表里的 symbol：计 parse_errors，不回调（避免脏数据进 cache/落盘）
    const bool subscribed = std::any_of(
        config_.symbols.begin(), config_.symbols.end(),
        [&](std::string s) {
            for (char& ch : s) {
                if (ch >= 'A' && ch <= 'Z') {
                    ch = static_cast<char>(ch - 'A' + 'a');
                }
            }
            return s == symbol;
        });
    if (!subscribed) {
        metrics_.parse_errors.fetch_add(1, std::memory_order_relaxed);
        return;
    }

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
        auto t = Parser::parse_trade(*data_it);
        const auto t_after_biz = std::chrono::steady_clock::now();
        record_segment_us(metrics_.json_parse_latency, t_enter, t_after_json,
                          &metrics_.clock_anomaly_count);
        record_segment_us(metrics_.biz_parse_latency, t_after_json, t_after_biz,
                          &metrics_.clock_anomaly_count);
        if (t) {
            const auto seq = trade_seq_gates_[symbol].on_live_trade(t->trade_id);
            if (seq.action != TradeSeqGate::Action::Accept) {
                if (seq.action == TradeSeqGate::Action::Duplicate) {
                    metrics_.duplicate_count.fetch_add(1, std::memory_order_relaxed);
                } else if (seq.action == TradeSeqGate::Action::Gap) {
                    metrics_.gap_count.fetch_add(1, std::memory_order_relaxed);
                    metrics_.missing_records.fetch_add(seq.missing_count(),
                                                       std::memory_order_relaxed);
                    metrics_.recovering_symbols.fetch_add(1, std::memory_order_relaxed);
                    std::cerr << "[ConnectionManager] trade gap " << symbol
                              << " missing [" << seq.missing_begin
                              << "," << seq.missing_end << "] count="
                              << seq.missing_count() << std::endl;
                }
                // Hold：已在 Recovering，只拦回调，不再加 gap/recovering
                return;
            }

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
            const auto t_after_cb = std::chrono::steady_clock::now();
            record_segment_us(metrics_.callback_latency, t_after_biz, t_after_cb,
                              &metrics_.clock_anomaly_count);
        } else {
            // 业务字段缺失/格式错：parse_trade 返回 nullopt
            metrics_.parse_errors.fetch_add(1, std::memory_order_relaxed);
        }
    } else if (stream_type.rfind("depth", 0) == 0) {
        auto ob = Parser::parse_orderbook(*data_it, symbol);
        const auto t_after_biz = std::chrono::steady_clock::now();
        record_segment_us(metrics_.json_parse_latency, t_enter, t_after_json,
                          &metrics_.clock_anomaly_count);
        record_segment_us(metrics_.biz_parse_latency, t_after_json, t_after_biz,
                          &metrics_.clock_anomaly_count);
        if (ob) {
            auto& last_id = last_orderbook_update_id_[symbol];
            const bool seen = last_id != 0;
            if (seen && ob->last_update_id < last_id) {
                metrics_.orderbook_id_rollback_count.fetch_add(
                    1, std::memory_order_relaxed);
                std::cerr << "[ConnectionManager] orderbook lastUpdateId rollback "
                          << symbol << " " << last_id << " -> "
                          << ob->last_update_id << std::endl;
                return;
            }
            last_id = ob->last_update_id;

            metrics_.orderbooks_parsed.fetch_add(1, std::memory_order_relaxed);
            safe_invoke(on_orderbook_, *ob, "orderbook");
            const auto t_after_cb = std::chrono::steady_clock::now();
            record_segment_us(metrics_.callback_latency, t_after_biz, t_after_cb,
                              &metrics_.clock_anomaly_count);
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
