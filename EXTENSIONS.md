# 扩展点与技术债

主线优先；这里只记**还没做完**的扩展与债。  
架构与总状态见 `ROADMAP.md`，不要两边各写一套「已完成清单」。

编号按层分组，方便检索；完成一项就删掉或挪到文末「已消化」。

---

## A. 网络层

### A1. WebSocket 同步 → 全异步
- **现状**：同步 read/write + mutex
- **问题**：`close` 与阻塞 `read` 竞态；多线程写要锁
- **方向**：Boost.Asio 异步（`io_context` + strand）；只改 `WebSocketClient::Impl`，Pimpl 对外不变

### A2. 心跳兼容与容错
- **现状**：标准 WebSocket Ping/Pong；靠 `no_data_timeout` 容许多次丢 Ping
- **方向**：可选应用层心跳（`heartbeat_type`: `ws_frame` | `json_message`）；记录丢 Ping / 连续断线供监控

### A3. 代理能力增强
- **现状**：`proxy_url` 或环境变量 `https_proxy`；仅 `http://host:port`
- **缺口**：Basic 鉴权、HTTPS 代理、SOCKS5、`no_proxy`
- **方向**：抽象 `ProxyConfig`；最常见场景先加 `Proxy-Authorization`

### A4. 可中断 connect + happy-eyeballs
- **现状**：同步 `resolve/connect/handshake` 持锁；DNS resolve 不受 `expires_after` 约束；Ctrl+C 最坏等 DNS 超时。已强制 IPv4 规避部分环境 IPv6 黑洞
- **方向**：整段握手异步化（配合 A1）；v4/v6 竞速；给 resolve 加 deadline

### A5. 回调线程安全
- **现状**：`on_disconnect_` 可能在 read 线程触发；回调里销毁 client 可能自 join 死锁
- **现状处理**：文档约束调用方
- **方向**：独立通知线程 / GC join / 或随全异步模型自然解耦

---

## B. 数据源

### B1. 多源双活
- **现状**：配置可写多源，运行只启单源
- **方向**：多源并行、按 `trade_id` 去重、单源故障切换（`DataSourceManager`）

### B2. 多交易所适配器
- **现状**：只接 Binance
- **方向**：`IMarketDataAdapter`；内部仍统一为 `Trade` / `OrderBookSnapshot`

### B3. 断线 REST 补数
- **现状**：配置预留 `enable_data_recovery`，未实现
- **方向**：重连后按时间窗拉 REST；需 HTTPS 客户端与断线时间记录

---

## C. 配置

### C1. 配置热更新
- **现状**：启动加载一次
- **方向**：文件变更监听 + 双缓冲/RCU；正在用的配置不可直接改

---

## D. 解析

### D1. OrderBook 时间戳对齐
- **现状**：depth 无交易所时间戳，用本地时间
- **问题**：与 trade 对齐有偏差
- **方向**：`lastUpdateId` 序号对齐，或换带时间戳的流（如 `@bookTicker`）

---

## E. 存储

### E1. 写盘队列与可观测
- **现状**：`deque + mutex` 有界队列；已暴露写入数/丢弃数/错误标志与**队列深度**（`queue_depth_current/max`）
- **缺口**：写入耗时未系统接入监控；队列仍非无锁
- **方向**：SPSC/低锁队列、可配置批量写、写入延迟进 metrics

### E2. 文件切分与索引
- **现状**：按 UTC 小时切分 `trades_YYYYMMDD_HH.bin` / `orderbooks_YYYYMMDD_HH.bin`；同小时重启追加；Reader 按文件名顺序读。旧单文件仍可回放。
- **方向**：按 symbol 再切 + 时间索引，便于范围回放

### E3. 完整性增强
- **现状**：header 有 magic/version/record_size，并回填 `record_count`；回放可做计数校验
- **缺口**：无 checksum/CRC；损坏尾部只能读失败，不能自动截断修复
- **方向**：块级 CRC；启动扫描并截断半条记录

---

## F. 回放

### F1. 时间范围与变速
- **现状**：按文件顺序最快全量回放
- **方向**：时间窗过滤；按 `timestamp_us` 做 1x / Nx / 最快

---

## G. 缓存

### G1. 环形缓冲 lock-free
- **现状**：SPMC（`mutex` 保护 `push/snapshot`）
- **方向**：优先 seq-lock；保持接口不变。不适用单纯 SPSC 队列语义

### G2. 多 symbol 隔离
- **现状**：多 symbol 共用一个 ring，最近 N 条是混合的
- **方向**：按 symbol 分桶

### G3. Trade / OrderBook 容量拆分
- **现状**：共用 `ring_buffer_size`；OrderBook 单条大得多
- **方向**：分配置项，按体积与更新频率估算

---

## H. 监控与日志

### H1. 交易所时间校准（外部延迟）
- **现状**：`LATENCY_EXT = local now - trade.timestamp_us`；本机落后会丢负样本
- **方向**：定期拉 server time 估 offset；内部 `LATENCY_INT`（steady_clock）仍是优化主指标

### H2. 统一日志（spdlog）
- **现状**：多处 `cout/cerr`；main 部分用锁防交错
- **方向**：统一 `log_*` 接口 → spdlog（异步、等级、多 sink、滚动）

---

## I. 测试与 API

### I1. 单元测试
- **现状**：无 gtest；有 `tests/e2e.sh`、`tests/benchmark.sh`
- **方向**：gtest + ctest（结构体、Config、Parser、storage round-trip、ring、replayer）

### I2. 错误 / 事件模型
- **现状**：feed 以 trade/orderbook 回调为主
- **方向**：连接、解析、写盘等错误事件可订阅、可观测

---

## 已消化（勿再当 TODO）

| 项 | 说明 |
|---|---|
| Combined Stream symbol 分发 | 从 `stream` 前缀取 symbol，OrderBook 不再错标 |
| Parser `json&` 重载 | 避免 `dump`+再 `parse` |
| 写盘队列深度查询 | `queue_depth_current/max` |
| header `record_count` | 写完回填，回放可对账 |
| 库边界与安装 | `src/` PRIVATE；公共头 + `find_package` |
| 分段延迟直方图 | json / biz / callback 等 |
| 集成/性能脚本 | `e2e.sh` / `benchmark.sh`（≠ 单元测试） |
