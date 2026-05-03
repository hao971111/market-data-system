# 性能测试记录

这里只记录每次迭代最关键的对比结果。详细输出临时看 `build/benchmark_summary.md`。

## 怎么看

- **主要看 `INT p99`**：这是本进程内部处理延迟，优化代码时优先看它有没有下降。
- **同时看 `msgs/s`**：确认吞吐没有明显下降。
- **`msgs/s` 不是严格压测结果**：这里接的是真实行情，消息量会随市场波动变化；严格吞吐对比需要后续用同一份录制数据做离线回放压测。
- **必须看 `dropped` 和 `e2e`**：掉数必须为 0，回放对账必须 PASS。
- **暂不看 `LATENCY_EXT`**：外部延迟受本机时间校准、网络和代理影响，先不作为优化判断依据。

## 迭代记录

| 日期 | 改动 | Commit | 构建 | 时长 | msgs/s(avg/max) | INT p99(avg/max) | INT p99变化 | dropped | e2e | perf结论 | 结论 |
|---|---|---|---|---:|---:|---:|---:|---:|---|---|---|
| 2026-05-02 | Baseline v0：主线闭环，未做专项优化 | `dd5ea45` | Release | 60s | 71 / 692 | 581us / 1024us | 基准 | 0 | PASS | orderbook 二次 JSON parse/dump 是潜在热点 | 作为后续优化基准；潜在瓶颈待分段指标或 profiling 验证 |
| 2026-05-03 | OrderBook 解析优化：去掉 data.dump() + 二次 json::parse | `6d608d8` | Release | 180s | 51 / 499 | 457us / 8192us | avg -21.3%，max 不可比 | 0 | PASS | orderbook 二次解析热点明显下降；剩余热点转向收包、futex 唤醒和 strtod | INT p99 平均值更低，但有一次 8ms 级尖刺；因实时行情和测试时长不同，吞吐变化只作参考 |

## 每次迭代怎么记录

1. 跑：`bash tests/benchmark.sh --duration 60`
2. 看：`build/benchmark_summary.md`
3. 追加一行到上表，重点填：改了什么、`msgs/s`、`INT p99`、`INT p99变化`、`dropped`、`e2e`、`perf结论`

