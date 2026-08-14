#pragma once

#include <atomic>
#include <cstdint>

#include "latency_histogram.h"

namespace mds {

// 系统运行时指标（线程安全计数器集合）
//
// 设计理念：
//   - 所有字段都是 std::atomic<uint64_t>，多生产者并发 inc 安全
//   - 各组件按引用持有 const Metrics&（写）、main 里 reporter 线程读
//   - 不做单例：显式注入便于测试 / 多实例（未来若多 ConnectionManager）
//
// memory_order 选择：
//   - inc 用 memory_order_relaxed —— 这些计数器只用作"汇报"，
//     不参与同步关系；relaxed 在 x86 上就是普通 lock add，开销最小
//   - reporter 读取用 load(memory_order_relaxed) —— 偶尔少几条无所谓
//
// 已有能力：
//   - 固定桶 LatencyHistogram：trade / pipeline / json_parse / biz_parse / callback（可估 p50/p99/p99.9）
// 跟生产级的差距：
//   - 直方图精度与动态范围不如 HdrHistogram 一类专用库
//   - 没有 metric 导出（Prometheus）：生产用 prometheus-cpp client
//   - 没有 label / 维度：当前每个 symbol 没单独计数
struct Metrics {
    // 网络层
    std::atomic<uint64_t> msgs_recv{0};         // WebSocket 收到的消息总数（含控制消息）
    std::atomic<uint64_t> connect_attempts{0};  // 连接尝试次数（含失败）
    std::atomic<uint64_t> connect_successes{0}; // 连接握手成功次数

    // 解析层
    std::atomic<uint64_t> trades_parsed{0};     // 成功解析为 Trade 的消息数
    std::atomic<uint64_t> orderbooks_parsed{0}; // 成功解析为 OrderBookSnapshot 的消息数
    std::atomic<uint64_t> parse_errors{0};      // 外层 JSON 或 stream 字段异常的次数

    // Trade 序列门（Step 11）：按 symbol 检查 trade_id 连续性
    std::atomic<uint64_t> gap_count{0};            // 发现跳号的次数（进入 Recovering）
    std::atomic<uint64_t> duplicate_count{0};      // trade_id <= last 的次数（重复/乱序/回退）
    std::atomic<uint64_t> missing_records{0};      // 累计缺失条数（各次 gap 区间长度之和）
    std::atomic<uint64_t> recovering_symbols{0};   // 当前处于 Recovering 的 symbol 数（水位，非累加）

    // 业务回调层
    std::atomic<uint64_t> callback_errors{0};   // 用户回调抛异常被 safe_invoke 接住的次数
    // pipeline 延迟越界计数（按 on_raw_message 总耗时）
    std::atomic<uint64_t> pipeline_over_500us{0};
    std::atomic<uint64_t> pipeline_over_1000us{0};

    // reconnect 连接耗时统计（仅统计 connect() 调用本身耗时）
    std::atomic<uint64_t> connect_duration_samples{0};
    std::atomic<uint64_t> connect_duration_ms_total{0};
    std::atomic<uint64_t> connect_duration_ms_max{0};

    // steady_clock 负值异常计数（正常应恒为 0；非零说明有系统级时钟异常）
    // 覆盖：pipeline timer / json_parse / biz_parse / callback 分段 /
    //        connect 计时 / trade enqueue / orderbook enqueue
    std::atomic<uint64_t> clock_anomaly_count{0};

    // writer 入队耗时（从回调层调用 writer.write() 的耗时）
    std::atomic<uint64_t> trade_enqueue_samples{0};
    std::atomic<uint64_t> trade_enqueue_us_total{0};
    std::atomic<uint64_t> trade_enqueue_us_max{0};
    std::atomic<uint64_t> orderbook_enqueue_samples{0};
    std::atomic<uint64_t> orderbook_enqueue_us_total{0};
    std::atomic<uint64_t> orderbook_enqueue_us_max{0};

    // 端到端延迟（交易所时间戳 → 本地回调到达）
    // 口径：trade.timestamp_us 是 binance 服务端发出时刻（wall-clock 微秒）
    //       我们本地用 system_clock 的 us 作差。含网络往返 + 本地解析。
    // 注意：OrderBook depth 流不带交易所时间戳，timestamp_us 是 parser 在本地打的，
    //       算延迟近似 0 没意义，所以本版本只测 trade。
    LatencyHistogram trade_latency;

    // 内部处理延迟（on_raw_message 进入 → 函数退出）
    // 口径：使用 steady_clock，只衡量本进程内部的 JSON 解析、结构体解析、
    //       callback 分发和入队写盘等耗时，不包含交易所/公网/代理/NTP 影响。
    LatencyHistogram pipeline_latency;

    // 分段延迟（仅在 trade/orderbook 命中分支时记录）：
    //   json_parse_latency：外层 nlohmann::json::parse(msg) 耗时
    //   biz_parse_latency ：业务字段解析（Parser::parse_trade/parse_orderbook）耗时
    //   callback_latency  ：用户回调耗时（含 cache.push、writer.write 入队）
    // 三段之和 ≈ pipeline_latency。用来定位"尾延迟主要落在哪一段"。
    LatencyHistogram json_parse_latency;
    LatencyHistogram biz_parse_latency;
    LatencyHistogram callback_latency;

    // 禁拷贝（atomic 本身不可拷贝；显式声明让出错信息更清晰）
    Metrics() = default;
    Metrics(const Metrics&) = delete;
    Metrics& operator=(const Metrics&) = delete;
};

}  // namespace mds
