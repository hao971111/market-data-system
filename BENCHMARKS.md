# 性能测试记录

优化对比以**离线基线**为准。live 行情表仅作历史参考（消息量随市场波动，不可严格对比）。

## 怎么看（离线）

- **主要看 `p99` / `p99.9`**：本进程内部处理延迟（`--bench-pipeline`）。
- **同时看 `msgs/s` 与 `dropped`**：吞吐与是否丢数；`dropped != 0` 脚本直接失败。
- **必须看 `write` 列**：默认基线是 **no-write**；打开写盘 msgs/s 会差一倍量级，不可混比。
- **统一口径**：`messages=50000`，`gap_us=0`，`write=no`（默认）。
- **数据集固定**：`pipeline_benchmark.cpp` 内确定性合成 trade/depth JSON（非真实行情）。
- 同一 commit 建议：`bash tests/benchmark.sh --runs 3`。P99 用指数桶，相邻桶允许 2× 差；超出一个桶阶再按相对波动判失败。

## 离线基线


| 日期 | Commit | Build | messages | gap_us | write | msgs/s | p50 | p99 | p99.9 | dropped | 结论 |
| --- | --- | --- | ---: | ---: | --- | ---: | --- | --- | --- | ---: | --- |
| 2026-08-09 | `63d0c35` | Release | 50000 | 0 | no | 93763 | 32us | 43us | 85us | 0 | offline deterministic; runs=3; p99_range=32-64 |


## 历史 live 记录（不可比）


| 日期         | 改动                    | Commit    | 时长   | msgs/s(avg/max) | INT p99(avg/max) | INT p99 avg变化 | dropped | e2e  | 结论                   |
| ---------- | --------------------- | --------- | ---- | --------------- | ---------------- | ------------- | ------- | ---- | -------------------- |
| 2026-05-02 | Baseline v0           | `dd5ea45` | 60s  | 71 / 692        | 581us / 1024us   | 基准            | 0       | PASS | 基准版本                 |
| 2026-05-03 | OrderBook 去二次 JSON 解析 | `6d608d8` | 180s | 51 / 499        | 457us / 8192us   | avg -21.3%    | 0       | PASS | INT p99 avg下降，max有尖刺 |
| 2026-05-03 | Writer 空队列才 notify 实验 | `c345dbb`   | 180s | 69 / 841        | 514us / 2048us   | 延迟+12.5% vs上轮 | 0       | SKIP | INT p99未改善             |
| 2026-05-16 | from_chars 替换 stod（Trade/OrderBook） | `7a9882d` | 180s | 38 / 406 | 512us / 512us | ~0%（同量级） | 0 | SKIP | perf 显示 strtod 热点下降，但 INT p99 未明显改善 |


## 备注

- 2026-05-02：perf 显示 orderbook 二次 JSON parse/dump 是潜在热点。
- 2026-05-03：perf 显示 orderbook 二次解析热点明显下降；剩余热点主要是收包、futex 唤醒和 strtod。实时行情和测试时长不同，msgs/s 变化只作参考。
- 2026-05-03：Writer 空队列才 notify 后，perf 采样显示 futex/cond_signal 热点仍明显；本轮 INT p99 avg 高于上一轮，但 msgs/s avg 也更高，不能直接判断为本次变更造成的性能退化。需要离线压测验证高负载场景是否受益。
- 2026-05-05：当前观测到 live 与离线 bench 的 INT p99 差异，可能主要与“间歇到包 vs 连续喂数”的场景差异有关，仍需在统一喂数模式下继续复测确认；这不代表主处理链路代码不一致。
- 2026-05-05：trade 路径去掉 data.dump() 二次解析，属于减少冗余解析开销的代码优化；当前 bench/live P99 未见稳定改善，尾延迟仍主要受间歇到包场景影响。
- 2026-05-06：`--bench-pipeline` 加 `--bench-gap-us` 固定消息间隔后，离线 INT 尾延迟可与 live 同量级，印证差异主要来自「间歇到包 / cache 冷」而非单段代码热点；已加 `LATENCY_SEG` 分段与可选 `--pin-cpu`（WSL 上收益不明显，专机可再试）。
- 2026-05-16：浮点解析切到 `from_chars` 后，perf 中 `strtod` 已不再是主热点；但 live benchmark 的 INT p99 仍在同量级。当前主要热点更偏向 nlohmann JSON DOM 分配/`scan_string` 与 writer/callback 路径。
- 2026-08-09（Step 8）：离线基线默认 `50000 / gap0 / no-write`；表含 `write` 列；`dropped!=0` 硬失败。

## 每次迭代怎么记录

1. 跑：`bash tests/benchmark.sh --runs 3`（默认 50000、no-write）
2. 看：`build/benchmark_offline_summary.md`
3. 把 `build/benchmark_offline_row.md` 追加到「离线基线」表（确认 `write` 列一致再比 msgs/s）。
