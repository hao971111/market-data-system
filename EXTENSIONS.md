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

### 3c. HTTP CONNECT 代理增强

- **现状**：支持 `config.json` 里的 `proxy_url` 字段，没配置时 fallback 读 `https_proxy`/`HTTPS_PROXY` 环境变量；
  解析在 `Config::load()` 里启动单线程完成（绕开 `getenv` 的多线程不安全问题），仅支持 `http://host:port` 格式
- **不支持**：
  - 用户名密码：`http://user:pass@host:port`（需要加 `Proxy-Authorization: Basic <b64>` 头）
  - HTTPS 代理：`https://host:port`（连代理本身要先做一次 TLS）
  - SOCKS5 代理：协议完全不同，需要单独实现握手
  - `no_proxy` 黑名单：某些目标想绕过代理直连
- **生产级方案**：把代理配置抽象为 `ProxyConfig` 类，支持上述全部模式，配置可来自环境变量或 `config.json`
- **改造点**：在 `WebSocketClient::Impl::connect()` 里加上 auth header 即可解决最常见场景

### 3a. connect() 阻塞期间无法被外部打断 + IPv6 happy-eyeballs

- **现状**：`Impl::connect()` 在持有 `lifecycle_mutex_` 期间调用同步阻塞的
  `resolver_.resolve()` / `tcp_stream::connect()` / `handshake()`。
  - `expires_after(10s)` 只对 connect/handshake/read/write 起作用，**对 `resolver_.resolve()` 没用**
  - 用户 Ctrl+C 时 `disconnect()` 拿不到 `lifecycle_mutex_`，最坏要等 30s+ 的 DNS 超时
- **当前已处理**：强制 IPv4 解析，避开 WSL2 等环境无 IPv6 路由的"Network unreachable / timeout"问题
- **生产级方案**：
  - **happy-eyeballs**：v4/v6 并发 async_resolve + async_connect，谁先成功用谁
  - **可中断 connect**：把整个握手流程改成 async + io_context，stop() 调用 `ioc_.stop()` 立刻打断
  - **DNS 自管超时**：用 deadline_timer 给 resolve 加超时
- **改造点**：把 `Impl::connect()` 整体改成异步链式回调，配合扩展点 #1（全异步模式）一起做

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

### 12. Combined Stream 精确 symbol 分发 [已完成]

- **现状**：使用 Binance combined stream 端点（`/stream`），消息外层带 `stream` 字段；`on_raw_message` 从 `stream` 名前缀拿 symbol 再分发到 trade / orderbook
- **解决了**：之前所有 OrderBook 都被打成第一个 symbol 标签的 bug

### 12a. Parser 接受 json 引用避免重复序列化

- **现状**：`on_raw_message` 收到 `{"stream":..,"data":..}` 后，把 `data` 子对象 `dump()` 成字符串再传给 `Parser::parse_trade/parse_orderbook`；Parser 内部又 `parse()` 一次回 json
- **代价**：每条消息多一次序列化（dump）+ 一次反序列化（parse），HFT 场景里这是显著浪费
- **生产级**：让 Parser 接受 `const nlohmann::json&` 入参，直接复用外层已经解析好的对象。或者继续保留 string 版作为兼容入口
- **改造点**：`Parser::parse_trade(const nlohmann::json&)`、`Parser::parse_orderbook(const nlohmann::json&, std::string_view)` 增加重载

---

## 存储层

### 13. 写盘线程模型

- **现状**：`BinaryRecordWriter<Record, Header>` 把 Trade / OrderBook 放入有界队列，后台写盘线程分别写入 `trades.bin` / `orderbooks.bin`
- **问题**：当前队列是 `std::deque + mutex`，队列满时直接返回失败；已经暴露写入数、丢弃数和错误标志，但还没有暴露队列深度和写入延迟
- **生产级**：替换为无锁/低锁 SPSC 队列，批量写入策略可配置，并把队列深度、写入耗时接入监控

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

## 缓存层

### 17. 环形缓冲：mutex → lock-free 升级

- **场景定位**：当前 `TradeRingBuffer` 是 **SPMC（1 写 N 读）环形缓冲**——网络线程单写，策略/监控等多个线程并发读快照
- **现状**：用一把 mutex 保护 push/snapshot/size，正确但每次操作都有 ~50ns 锁开销
- **问题**：HFT 路径上单条 Trade 处理预算 1µs 级，mutex 在 contention 高时会显著放大延迟
- **生产级方案**（按复杂度递增）：
  - **seq-lock**：写者前后各 +1 一个 atomic 序号（奇=写中，偶=稳定）；读者拷贝完检查序号有没有变，变了重读。SPMC 场景天然契合，写者几乎零开销
  - **RCU / hazard pointer**：高频读+高频写时再考虑，复杂度上一个量级
- **不适用方案**：SPSC 队列（如 LMAX Disruptor 单段）——它是 1 读 1 写、pop 后消失的语义，无法支持多读者重复读快照
- **改造点**：保持 `push/snapshot` 接口不变，替换内部实现即可

### 18. 多 symbol 隔离

- **现状**：所有 symbol 共用同一个 `TradeRingBuffer`，"最近 N 条"是混合的
- **问题**：策略一般按 symbol 查最近 N 条，混合缓冲不够用
- **生产级**：`unordered_map<string, TradeRingBuffer>`，按 symbol 分桶，避免热门币种淹没冷门币种

### 19. Trade / OrderBook 缓冲容量拆分

- **现状**：`TradeRingBuffer` 和 `OrderBookRingBuffer` 都复用 `config.ring_buffer_size`
- **问题**：`OrderBookSnapshot` 比 `Trade` 大得多（664B vs 56B），相同条数会占用更多内存
- **生产级**：配置拆成 `trade_ring_buffer_size` 和 `orderbook_ring_buffer_size`，并按 symbol / 更新频率估算容量

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
