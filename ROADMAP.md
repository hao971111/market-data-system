# 项目架构与开发路线

本文档面向项目协作与技术演进，说明：

- 这个项目的目标和定位
- 当前系统架构与实现手段
- 目前已知不足
- 接下来的开发路线

---

## 1. 项目目标

Market Data System 是一个行情数据基础设施项目，核心目标是：

1. 稳定接入实时行情（Trade / OrderBook）
2. 可靠落盘并可验证回放
3. 为上层策略/研究系统提供可复用 C++ 库能力

项目支持两种使用方式：

- CLI 模式：`market-data-system`（live / replay / bench）
- Library 模式：`libmds_core.a` + `include/mds/*.h`

---

## 2. 当前架构

### 2.1 数据流

```text
WebSocket -> ConnectionManager -> Parser -> Callback -> Writer -> Binary Files
                                                     \
                                                      -> User Handler (feed API)

Binary Files -> Reader -> Replayer -> User Handler
```

### 2.2 模块划分

- `src/network`：WebSocket 连接、重连、心跳、代理
- `src/parser`：JSON 到领域结构体解析
- `src/storage`：二进制异步写入 / 读取
- `src/replay`：顺序回放与结果校验
- `src/monitor`：吞吐/延迟等运行指标
- `src/api`：对外 facade（`feed` / `replayer`）
- `include/mds`：公共头文件
- `examples`：最小可运行用法

---

## 3. 关键实现手段

1. **连接稳定性**
   - 自动重连
   - 无数据超时检测
   - Ping 保活
   - 代理支持（HTTP CONNECT）

2. **数据结构与存储**
   - 固定布局结构体（适合高效落盘与回放）
   - Writer 后台线程异步写盘
   - 文件头记录 `record_count`，回放时做计数一致性校验

3. **可观测性**
   - 吞吐计数：消息/解析/写入/丢弃
   - 延迟统计：外部延迟 + 内部流水线延迟
   - 异常计数：时钟异常、回调异常等

4. **库化设计**
   - `mds::MarketDataFeed`：实时接收 facade
   - `mds::Replayer`：回放 facade
   - 示例：`examples/recv_only.cpp`、`examples/replay_only.cpp`

---

## 4. 当前状态

### 已完成

- [x] 实时接入链路（network + parser + callback）
- [x] 异步二进制落盘（Trade / OrderBook）
- [x] 回放能力与对账校验
- [x] 静态库拆分（`mds_core`）
- [x] 公共 API（`feed` / `replayer`）
- [x] 可选构建模式（只编库 / 只编 CLI / 全部编译）

### 已知不足

1. 库边界仍待收紧：`src/` 目前仍在 PUBLIC include 路径
2. 安装与分发能力不足：尚未完成公共头安装与 `find_package` 支持
3. 错误事件模型较弱：当前 feed API 仅暴露 trade/orderbook 回调
4. 性能诊断粒度还不够：分段延迟未完整打通

---

## 5. 后续开发路线

### P0（优先）

1. **库发布规范化**
   - 安装公共头（`include/mds`）
   - 补齐 CMake package 导出（支持 `find_package`）

2. **库边界收敛**
   - `src/` 从 PUBLIC 收回 PRIVATE
   - 类型定义逐步迁移到公共头，减少外部对内部路径感知

3. **可观测性增强**
   - 补齐分段延迟（parse / callback / enqueue / write）
   - 提供更稳定的 metrics 对外查询口径

### P1（增强）

1. 存储增强：checksum、损坏尾部处理、按时间切分
2. 回放增强：按时间范围、变速回放
3. 稳定性增强：连接耗时统计、重连诊断、时间偏移校准

### P2（中长期）

1. 多数据源主备 / 双活
2. 更高性能数据通路（必要时评估 simdjson 或二进制协议）
3. 与上层策略/研究系统接口标准化

---

## 6. 文档导航

- 项目入口说明：`README.md`
- 扩展与技术债：`EXTENSIONS.md`
- 基准结果记录：`BENCHMARKS.md`

