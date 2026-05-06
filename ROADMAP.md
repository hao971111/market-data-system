# 项目后续路线图

本文档记录主线完成后的优化方向。原则是：**先建立指标，再根据指标优化**，避免凭感觉改代码。

---

## 1. 可观测性与指标体系

目标：先把系统运行状态看清楚。

- [x] 吞吐指标：`msgs/s`、`trades/s`、`books/s`
- [x] 写盘指标：`trade_written/s`、`book_written/s`、`dropped/s`、writer error
- [x] 外部延迟：`LATENCY_EXT`，交易所时间戳到本机处理时间
- [x] 内部延迟：`LATENCY_INT`，`on_raw_message()` 内部处理耗时
- [x] LATENCY 聚合窗口加长：从每秒 1 次改为每 30 秒 1 次，提升 P99 稳定性
- [ ] writer 队列深度：当前积压量、历史最大积压量
- [ ] 外部延迟异常样本：负延迟丢弃数、时间校准偏移
- [ ] 分段延迟：JSON parse、业务解析、callback、writer enqueue、disk write（**下一步重点**）
- [ ] 资源指标：CPU、RSS 内存、磁盘写入 MB/s、文件增长速率

---

## 2. 数据链路稳定性

目标：让系统能长时间稳定接行情。

- [x] WebSocket 心跳保活
- [x] 无数据超时检测
- [x] 自动重连
- [x] HTTP CONNECT 代理
- [ ] 交易所时间校准：server time offset、校准 RTT、offset 抖动
- [ ] no-data timeout 次数、连续断线次数、重连耗时
- [ ] 断线后 REST 补数据
- [ ] 多数据源主备 / 双活
- [ ] 应用层心跳兼容：用于不标准回复 Pong 的服务端

---

## 3. 存储与回放能力

目标：数据能可靠落盘、查询、回放和验证。

- [x] Trade / OrderBook 异步二进制写盘
- [x] Trade / OrderBook 二进制读取与回放
- [x] 端到端脚本：live -> replay -> 记录数对账
- [ ] writer 队列深度与写盘延迟监控
- [ ] 文件头记录 `record_count`
- [ ] 文件块 checksum / CRC
- [ ] 启动时检测并截断损坏尾部
- [ ] 按日期 / 小时 / symbol 文件切分
- [ ] 时间索引：支持按时间范围快速回放
- [ ] 回放控制：1x、10x、最快、暂停、恢复

---

## 4. 性能优化方向

目标：根据指标定位瓶颈，再做针对性优化。

优先级建议：

1. [x] 去掉 `outer["data"].dump()` + Parser 内部二次 parse（已做，未观察到稳定 P99 改善，原因见第 7 节）
2. [ ] 增加 JSON parse / callback / writer enqueue 分段延迟（**下一步重点**）
3. [ ] 进一步降低日志路径干扰：live 明细打印降频或后台异步化
4. [ ] writer 队列从 `std::deque + mutex` 升级为 SPSC ring queue
5. [ ] cache 从 mutex ring buffer 升级为 seq-lock / lock-free snapshot
6. [ ] 如果 JSON 仍是瓶颈，再评估 simdjson 或二进制协议

---

## 5. 配置与工程化

目标：让项目更像可维护的工程系统。

- [ ] `trade_ring_buffer_size` / `orderbook_ring_buffer_size` 分开配置
- [ ] metrics/log level 可配置
- [ ] 代理配置增强：认证、`no_proxy`、SOCKS5
- [ ] 单元测试：Config、Parser、BinaryRecordReader/Writer、RingBuffer
- [x] benchmark 脚本：`tests/benchmark.sh` + 离线 `--bench-pipeline/--bench-write/--bench-gap-us`
- [ ] README 更新：架构图、运行方式、指标口径、生产级差距

---

## 6. 简历与面试准备

目标：把项目讲成工程能力，而不是只讲代码功能。

- [ ] 项目架构图：Network -> Parser -> Cache -> Storage -> Replay -> Monitor
- [ ] 数据流图：实时接入、写盘、回放对账
- [ ] 指标口径说明：`LATENCY_EXT` vs `LATENCY_INT`
- [ ] 当前瓶颈说明：代理网络、JSON 二次解析、writer 队列
- [ ] 优化记录：每次优化前后指标对比
- [ ] 面试问答：为什么这样设计、生产级差距在哪、下一步怎么优化

---

## 7. 当前观察到的延迟假设

目标：把"代码优化"和"场景效应"分开看，避免误归因。

- live 与离线 bench 的 INT P99 存在差距（live 偏高），**当前推断主要原因是"间歇到包 + 冷启动效应"**：
  - live 多数时间线程阻塞在 epoll/cv，等到来包再被唤醒；CPU cache、TLB、分支预测都不"热"，单条处理尾延迟更容易被拉长
  - 离线 bench 默认连续喂数，CPU/缓存常驻热路径，P99 较低
- 已用 `--bench-gap-us 4000` 在 bench 模式中模拟 live 到包节奏，P99 接近 live，初步支持上述假设
- **后续验证方向**：
  - 完成"分段延迟"后，看尾延迟主要落在哪一段（parse / callback / writer enqueue）
  - 同机原生 Linux vs WSL2 对比，验证 VM 调度抖动占比
  - 必要时用真实录制 JSON 替代合成 JSON 做离线压测

