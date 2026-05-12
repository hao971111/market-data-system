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

如果你不熟 CMake，按下面 3 步直接复制就能用。

#### 步骤 1：在本仓库里“一键安装”库到本地目录

```bash
cd /path/to/market-data-system
bash scripts/install_local_lib.sh
```

执行完成后，库会被安装到：

- `./_install/include/mds/*.h`
- `./_install/lib/libmds_core.a`

#### 步骤 2：在你的项目里写 C++ 代码

示例：

```cpp
#include <mds/feed.h>
#include <mds/replayer.h>
```

完整用法参考：

- `examples/recv_only.cpp`（实时接收）
- `examples/replay_only.cpp`（回放）

#### 步骤 3：用 `g++` 直接链接（不需要你写 CMake）

```bash
g++ -std=c++17 your_main.cpp \
  -I /path/to/market-data-system/_install/include \
  -L /path/to/market-data-system/_install/lib \
  -lmds_core -lssl -lcrypto -lpthread \
  -o your_app
```

---

如果你熟悉 CMake，也可以用标准方式：

```cmake
find_package(mds CONFIG REQUIRED)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE mds::mds_core)
```

## 开发进度（简版）

- [x] 实时接入 + 解析 + 落盘
- [x] 回放能力
- [x] 静态库拆分（`mds_core`）
- [x] 公共 API（`feed` / `replayer`）与示例
- [x] 库发布规范化基础版（头文件安装、`find_package` 支持）

更完整的架构说明、当前不足与后续路线，请查看 `ROADMAP.md`。

## License

MIT
