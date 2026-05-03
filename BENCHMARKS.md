# 性能测试记录

这里只记录每次迭代最关键的对比结果。详细输出临时看 `build/benchmark_summary.md`。

## 怎么看

- **主要看 `INT p99`**：这是本进程内部处理延迟，优化代码时优先看它有没有下降。
- **同时看 `msgs/s`**：确认吞吐没有明显下降。
- **`msgs/s` 不是严格压测结果**：这里接的是真实行情，消息量会随市场波动变化；严格吞吐对比需要后续用同一份录制数据做离线回放压测。
- **必须看 `dropped` 和 `e2e`**：掉数必须为 0，回放对账必须 PASS。
- **暂不看 `LATENCY_EXT`**：外部延迟受本机时间校准、网络和代理影响，先不作为优化判断依据。

## 迭代记录

| 日期 | 改动 | Commit | 时长 | msgs/s(avg/max) | INT p99(avg/max) | INT p99 avg变化 | dropped | e2e | 结论 |
|---|---|---|---:|---:|---:|---:|---:|---|---|
| 2026-05-02 | Baseline v0 | `dd5ea45` | 60s | 71 / 692 | 581us / 1024us | 基准 | 0 | PASS | 基准版本 |
| 2026-05-03 | OrderBook 去二次 JSON 解析 | `6d608d8` | 180s | 51 / 499 | 457us / 8192us | avg -21.3% | 0 | PASS | INT p99 avg下降，max有尖刺 |
| 2026-05-03 | Writer 空队列才 notify 实验 | `aab4bfd` | 180s | 69 / 841 | 514us / 2048us | avg +12.5% | 0 | SKIP | 未见改善，avg回退 |

## 备注

- 2026-05-02：perf 显示 orderbook 二次 JSON parse/dump 是潜在热点。
- 2026-05-03：perf 显示 orderbook 二次解析热点明显下降；剩余热点主要是收包、futex 唤醒和 strtod。实时行情和测试时长不同，msgs/s 变化只作参考。
- 2026-05-03：Writer 空队列才 notify 后，futex/cond_signal 热点仍明显；实时 benchmark 下 INT p99 avg 回退约 12.5%。需要离线压测验证高负载场景是否受益。

## 每次迭代怎么记录

1. 跑：`bash tests/benchmark.sh --duration 60`
2. 看：`build/benchmark_summary.md`
3. 追加一行到上表，长解释写到“备注”，不要塞进表格。

