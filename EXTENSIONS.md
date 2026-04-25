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

## 解析层（parser/）

### 11. OrderBook 时间戳精度
- **现状**：depth 消息无交易所时间戳，用本地时间代替
- **问题**：本地时间和交易所时间有偏差（几ms），无法精确对齐 trade 和 orderbook
- **生产级**：用 `lastUpdateId` 做序号对齐，或订阅 `@bookTicker` 获取带时间戳的版本

---

### 12. Combined Stream 精确 symbol 分发

- **现状**：depth 消息无 symbol，`on_raw_message` 遍历所有 symbol 逐个尝试（第一个成功即返回），多 symbol 时归属可能错误
- **生产级**：改用 Binance Combined Stream（`wss://...?streams=btcusdt@depth/ethusdt@trade`），外层有 `{"stream":"btcusdt@depth","data":{...}}`，先取 `stream` 字段直接定位 symbol，精确分发
- **改造点**：`build_subscribe_msg` 改为 combined stream URL，`on_raw_message` 先路由 stream 再解析

---

## 存储层

### 13. 写盘线程模型

- **现状**：`BinaryTradeWriter::write` 只把 `Trade` 放入有界队列，后台写盘线程批量写入 `trades.bin`
- **问题**：当前队列是 `std::deque + mutex`，队列满时直接返回失败；还没有暴露队列积压、写入延迟、丢弃计数等运行时指标
- **生产级**：替换为无锁/低锁 SPSC 队列，批量写入策略可配置，并把队列深度、写入耗时、丢弃数量接入监控

### 14. 文件切分与索引

- **现状**：启动时覆盖写入单个 `trades.bin`
- **问题**：长时间运行后文件过大，也不方便按时间范围查询
- **生产级**：按日期/小时/symbol 分文件，并维护时间索引，支持快速回放定位

### 15. 文件完整性校验

- **现状**：`BinaryTradeReader` 只校验文件头 magic/version/record_size，顺序读取 `Trade`
- **问题**：如果进程崩溃或磁盘写入异常，文件尾部可能出现半条记录，目前只能读到失败为止，无法区分正常 EOF 和损坏
- **生产级**：文件头记录 record_count，文件块增加 checksum/CRC，启动时可扫描并截断损坏尾部

---

## 回放层

### 16. 回放时间控制

- **现状**：`TradeReplayer::replay_all` 只按文件顺序最快速度回放全部 Trade
- **问题**：不能按时间范围过滤，也不能按原始时间间隔或倍速回放
- **生产级**：结合时间索引快速定位起止位置，并根据 `timestamp_us` 控制回放节奏，支持 1x/10x/最快模式

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
