# 扩展点与技术债记录

主线开发优先，这里记录遇到的所有扩展点和技术债，完成主线后再逐一处理。

---

## 网络层（network/）

### 1. WebSocket 同步模式 → 全异步模式
- **现状**：同步 read/write + mutex 锁保护
- **问题**：
  - `ws_.close()` 与 `ws_.read()` 存在竞态（read阻塞时close不安全）
  - 多线程写需要锁，有性能损失
- **升级方案**：Boost.Asio 异步模式（io_context + strand）
  - 所有操作在同一个执行器里，天然串行化
  - 不需要mutex，性能更好
- **影响面**：只改 `WebSocketClient::Impl`，接口不变（Pimpl）

### 2. 心跳机制兼容性
- **现状**：使用标准 WebSocket Ping/Pong 帧
- **扩展**：部分服务端不自动回复Pong，需支持应用层心跳
  - 如 `{"type":"ping"}` / `{"type":"pong"}` JSON消息
- **配置化**：增加 `heartbeat_type` 配置项：`ws_frame` | `json_message`

### 3. 心跳容错
- **现状**：允许多次Ping丢失（no_data_timeout > 2-3倍 ping_interval）
- **扩展**：
  - 记录Ping丢失次数，辅助监控指标
  - 记录连续断线次数，告警

### 3b. 回调线程安全
- **现状**：`on_disconnect_` 可能在 read_loop 线程里调用，
  用户若在回调里销毁 client 会自己 join 自己导致死锁
- **当前处理**：文档警告，调用方约束
- **生产级改造**：
  - 方案1：将回调投递到独立的"通知线程"执行
  - 方案2：维护一个后台 GC 线程来 join 已退出的工作线程
  - 方案3：全异步模型（io_context），回调在 executor 里执行，自然解耦

---

## 数据源层

### 4. 多数据源双活
- **现状**：配置结构支持多源，当前只启用单源
- **扩展**：
  - 同时连接多个数据源
  - 按 trade_id 去重合并
  - 一个断了另一个继续收
- **需要的模块**：`DataSourceManager`（统一管理多个源）

### 5. 多交易所支持（适配器模式）
- **现状**：只对接 Binance
- **扩展**：抽象 `IMarketDataAdapter` 接口
  - `BinanceAdapter`（已有）
  - `OKXAdapter`、`ChinaStockAdapter`、`HKStockAdapter` 等
- **统一内部格式**：`Trade` / `OrderBookSnapshot` 不变

### 6. 断线后REST补数据
- **现状**：配置已预留 `enable_data_recovery`
- **扩展**：重连成功后，通过REST API拉取断线期间的数据
  - 需要HTTPS客户端（Boost.Beast HTTP 或 curl）
  - 需要知道断线的精确时间范围

---

## 配置层

### 7. 配置热更新
- **现状**：启动时加载一次
- **扩展**：inotify监听文件变化，动态更新
- **注意**：正在使用的配置不能直接改，需双缓冲或RCU

---

## 存储层（待开发时补充）

---

## 监控层（待开发时补充）

---

## 测试

### 8. 单元测试
- **现状**：无
- **扩展**：gtest + ctest
  - Trade/OrderBook 结构体大小/对齐
  - Config 加载/默认值
  - WebSocket mock 测试

### 9. 集成测试
- **现状**：无
- **扩展**：端到端测试脚本
  - 启动系统 → 连接Binance → 收数据 → 验证存储 → 回放验证

### 10. 性能测试
- **现状**：无
- **扩展**：benchmark脚本
  - 每秒处理能力
  - P50/P99 延迟
  - 长时间运行稳定性
