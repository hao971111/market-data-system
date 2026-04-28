# 高性能行情数据系统

## 项目概述

这是一个单机高性能行情数据接入、缓存、存储与回放系统，使用 C++17 开发。

### 核心功能

1. **实时数据接入** — 从 Binance WebSocket combined stream 接入 Trade 和 OrderBook 数据
2. **连接保活与重连** — 支持 Ping 心跳、无数据超时检测、自动重连、HTTP CONNECT 代理
3. **高效解析缓存** — JSON 解析后分发为固定结构体，Trade 写入内存环形缓冲
4. **二进制存储** — Trade 和 OrderBook 数据异步落盘为二进制文件
5. **运行时监控** — 输出消息速率、解析成功数、解析错误数、回调错误数、连接成功率
6. **数据回放** — 已具备 Trade 二进制读取基础，回放控制能力仍在迭代中

### 系统架构

```
┌─────────────────────────────────────────────────────────────┐
│                     Market Data System                       │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌──────────┐    ┌──────────┐    ┌──────────┐              │
│  │ Network  │───▶│  Parser  │───▶│  Cache   │              │
│  │(WebSocket│    │  (JSON)  │    │(RingBuf) │              │
│  └──────────┘    └──────────┘    └────┬─────┘              │
│                                       │                      │
│                                       ▼                      │
│                               ┌──────────────┐              │
│                               │   Storage    │              │
│                               │  (Binary)    │              │
│                               └──────┬───────┘              │
│                                      │                       │
│                                      ▼                       │
│                               ┌──────────────┐              │
│                               │   Replay     │              │
│                               │   Engine     │              │
│                               └──────────────┘              │
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

## 技术栈

- **语言**: C++17
- **构建**: CMake 3.16+
- **平台**: Linux (WSL2 / Native)
- **依赖**: 
  - Boost.Beast (WebSocket / HTTP CONNECT)
  - nlohmann/json (JSON解析)
  - OpenSSL (TLS)

## 快速开始

### 编译

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
```

### 运行

```bash
cd ~/market-data-system
./build/market-data-system
```

> 注意：当前程序默认从运行目录读取 `config.json`，建议从项目根目录启动。
> 如果你的网络环境需要代理，可以在 `config.json` 里配置 `proxy_url`，否则会 fallback 到 `https_proxy` / `HTTPS_PROXY` 环境变量。

运行成功后会看到类似输出：

```text
[ConnectionManager] Connected.
[TRADE] btcusdt price=...
[BOOK]  ethusdt bid=... ask=...
[METRICS] msgs/s=... trades/s=... books/s=... parse_err/s=... cb_err/s=... | conn=1/1 | total: ...
```

## 目录结构

```
market-data-system/
├── CMakeLists.txt          # 构建配置
├── README.md               # 本文件
├── src/
│   ├── main.cpp            # 程序入口
│   ├── config/             # 配置模块
│   ├── network/            # WebSocket网络层
│   ├── parser/             # JSON解析器
│   ├── cache/              # 内存环形缓冲
│   ├── storage/            # 二进制存储
│   ├── monitor/            # 运行时指标
│   ├── replay/             # 数据回放
│   └── common/             # 公共工具类
├── include/                # 公共头文件
├── data/                   # 数据存储目录
└── tests/                  # 测试代码
```

## 模块说明

| 模块 | 功能 | 状态 |
|------|------|------|
| config | JSON 配置加载、参数校验、代理配置 | 已完成基础版 |
| network | WebSocket 连接、心跳、重连、HTTP CONNECT 代理 | 已完成基础版 |
| parser | Binance Trade / OrderBook JSON 解析 | 已完成基础版 |
| cache | Trade 内存环形缓冲 | 已完成基础版 |
| storage | Trade / OrderBook 异步二进制写入 | 已完成基础版 |
| monitor | 运行时计数器与每秒 reporter 输出 | 已完成基础版 |
| replay | Trade 二进制读取回放 | 部分完成 |
| common | 固定布局行情数据结构 | 已完成基础版 |

## 性能目标

- 处理能力: > 10,000 条/秒
- 接收到写入延迟: < 1ms (P99)
- 内存占用: 可配置的环形缓冲区

## 与生产系统的差异

本项目是接近生产级的Demo，与真实生产系统的差异：

1. 生产系统通常需要多数据源冗余与跨源校验
2. 生产系统需要更完整的故障处理、限频日志和告警系统
3. 生产系统需要按时间分片、索引、压缩或分布式存储
4. 生产系统需要 p50 / p99 / p999 延迟直方图，而不仅是计数器
5. 生产系统需要和策略、风控、交易执行系统对接

详细扩展点和技术债记录在 `EXTENSIONS.md`。

## 开发进度

- [x] 第1步: 项目骨架
- [x] 第2步: 数据结构定义
- [x] 第3步: 配置模块
- [x] 第4步: WebSocket连接
- [x] 第5步: JSON解析
- [x] 第6步: 内存缓存（Trade）
- [x] 第7步: 二进制存储（Trade / OrderBook）
- [ ] 第8步: 数据回放（部分完成）
- [x] 第9步: 监控统计（基础计数器）
- [ ] 第10步: 整合测试

## License

MIT
