# Market Data System

一个用 C++17 写的行情数据系统，支持两种使用方式：

- 作为 CLI 可执行程序运行（live/replay/bench）
- 作为静态库 `libmds_core.a` 被其他 C++ 项目调用

## 这个项目是做什么的

- 实时接入 Binance WebSocket 行情（Trade / OrderBook）
- 自动重连、心跳、代理支持
- 二进制异步落盘（`trades.bin` / `orderbooks.bin`）
- 历史数据回放
- 运行时指标统计（吞吐、丢包、连接、延迟相关计数）

## 对外能力（库 API）

公共头文件在 `include/mds/`：

- `<mds/types.h>`：数据结构（`Trade`、`OrderBookSnapshot`）
- `<mds/feed.h>`：实时接收（`mds::MarketDataFeed`）
- `<mds/replayer.h>`：历史回放（`mds::Replayer`）

示例代码：

- `examples/recv_only.cpp`：实时接收示例
- `examples/replay_only.cpp`：回放示例

## 编译

支持可选构建，默认三者都编：

- 静态库：`mds_core`
- CLI：`market-data-system`
- 示例：`replay_only`、`recv_only`

### 1) 默认（库 + CLI + 示例）

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
```

### 2) 只编静态库

```bash
cmake -S . -B build-lib \
  -DMDS_BUILD_LIBRARY=ON \
  -DMDS_BUILD_CLI=OFF \
  -DMDS_BUILD_EXAMPLES=OFF
cmake --build build-lib -j$(nproc)
```

### 3) 只编 CLI

```bash
cmake -S . -B build-cli \
  -DMDS_BUILD_LIBRARY=OFF \
  -DMDS_BUILD_CLI=ON \
  -DMDS_BUILD_EXAMPLES=OFF
cmake --build build-cli -j$(nproc)
```

## 使用方式

### 作为 CLI 使用

```bash
# live：实时接入并落盘
./build/market-data-system

# replay：从 data/ 顺序回放
./build/market-data-system --replay

# 离线链路压测
./build/market-data-system --bench-pipeline 100000 --bench-write
```

### 作为库使用

优先参考 `examples/recv_only.cpp` 和 `examples/replay_only.cpp`。  
这两个示例就是“用户项目里最常见的写法”。

## 开发进度（简版）

- [x] 实时接入 + 解析 + 落盘
- [x] 回放能力
- [x] 静态库拆分（`mds_core`）
- [x] 公共 API（`feed` / `replayer`）与示例
- [ ] 库发布规范化（头文件安装、`find_package` 支持）

更完整的架构说明、当前不足与后续路线，请查看 `ROADMAP.md`。

## License

MIT
