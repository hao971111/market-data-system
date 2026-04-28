#pragma once

#include <atomic>
#include <cstdint>

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
// 跟生产级的差距：
//   - 没有直方图 / 分位数（p50/p99）：用 HdrHistogram 之类的库才靠谱
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

    // 业务回调层
    std::atomic<uint64_t> callback_errors{0};   // 用户回调抛异常被 safe_invoke 接住的次数

    // 禁拷贝（atomic 本身不可拷贝；显式声明让出错信息更清晰）
    Metrics() = default;
    Metrics(const Metrics&) = delete;
    Metrics& operator=(const Metrics&) = delete;
};

}  // namespace mds
