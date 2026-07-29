# 项目架构与开发路线

面向协作与演进：目标、架构、已完成能力、真实缺口、后续方向。  
细项技术债见 `EXTENSIONS.md`；用法见 `README.md`；性能数字见 `BENCHMARKS.md`。

---

## 1. 目标

行情基础设施：稳定接入 Trade / OrderBook，可靠落盘与可验证回放，并作为可复用 C++ 库给上层用。

- CLI：`market-data-system`（live / replay / bench）
- Library：`libmds_core.a` + `include/mds/*.h`（支持安装与 `find_package`）

---

## 2. 架构

```text
WebSocket -> ConnectionManager -> Parser -> Callback -> Writer -> Binary Files
                                                     \
                                                      -> User Handler (feed API)

Binary Files -> Reader -> Replayer -> User Handler
```

| 模块 | 职责 |
|---|---|
| `src/network` | WebSocket、重连、心跳、代理 |
| `src/parser` | JSON → 领域结构体 |
| `src/storage` | 二进制异步写 / 读 |
| `src/replay` | 顺序回放与计数校验 |
| `src/monitor` | 吞吐 / 延迟直方图 / 异常计数 |
| `src/api` + `include/mds` | 对外 facade（`feed` / `replayer`） |
| `examples` | 最小用法 |

实现要点：自动重连与 Ping、HTTP CONNECT 代理、固定布局结构体落盘、写盘线程异步写、`record_count` 回填校验、内部流水线分段延迟（json / biz / callback）。

---

## 3. 当前状态

### 已完成

- [x] 实时接入（network + parser + callback）
- [x] 异步二进制落盘（Trade / OrderBook）
- [x] 回放与 header `record_count` 对账
- [x] 静态库 `mds_core` + 公共 API
- [x] 可选构建（库 / CLI / examples）
- [x] 库边界：`src/` 为 PRIVATE include；公共头安装 + `find_package`
- [x] 分段延迟直方图（json_parse / biz_parse / callback + pipeline / trade）
- [x] 离线脚本：`tests/e2e.sh`、`tests/benchmark.sh`（尚无 gtest 单元测试）

### 已知不足（仍真实存在）

1. **错误事件模型弱**：feed API 基本只有 trade/orderbook 回调，连接/解析失败等事件暴露不足
2. **无单元测试**：仅有 e2e / benchmark 脚本
3. **存储与回放能力偏基础**：单文件覆盖写、无 checksum、回放无时间范围/倍速（细节见 `EXTENSIONS.md`）
4. **WebSocket 仍为同步 read 模型**：竞态与可中断性有限（细节见 `EXTENSIONS.md`）
5. **配置有预留未落地项**：如数据恢复、部分超时字段未真正生效（后续步骤会清理）

---

## 4. 后续方向（高层）

按优先级，细项 backlog 在 `EXTENSIONS.md`：

| 优先级 | 方向 |
|---|---|
| P0 | 测试骨架（gtest）、配置清理、错误/事件模型、可观测口径稳定 |
| P1 | 存储增强（checksum / 切分）、回放增强、代理与连接诊断 |
| P2 | 多源主备、更高性能解析通路、与上层策略接口标准化 |

---

## 5. 文档分工

| 文件 | 作用 |
|---|---|
| `README.md` | 怎么装、怎么编、怎么跑 |
| `ROADMAP.md` | 架构 + 状态 + 方向（本文） |
| `EXTENSIONS.md` | 未完成的扩展点 / 技术债清单 |
| `BENCHMARKS.md` | 性能对比记录 |
