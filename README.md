# Market Data System

一个用 C++17 写的行情数据系统，支持两种使用方式：

- 作为 CLI 可执行程序运行（live/replay/bench）
- 作为静态库 `libmds_core.a` 被其他 C++ 项目调用

## 这个项目是做什么的

- 实时接入 Binance WebSocket 行情（Trade / OrderBook）
- 自动重连、心跳、代理支持
- 二进制异步落盘（按 UTC 小时切分 `trades_YYYYMMDD_HH.bin` / `orderbooks_YYYYMMDD_HH.bin`）
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

## 使用流程（照抄即可）

下面分成两条完整流程：  
1）只想把项目跑起来（可执行文件）；  
2）想把静态库接到你自己的项目里。

### 流程一：编译并使用可执行文件（从 GitHub 开始）

**第 0 步（仅第一次）：安装依赖**

```bash
sudo apt update
sudo apt install -y build-essential cmake libssl-dev libboost-all-dev nlohmann-json3-dev
```

**第 1 步：克隆项目**

```bash
# HTTPS
git clone https://github.com/hao971111/market-data-system.git market-data-system
# 或 SSH
#git clone git@github.com:hao971111/market-data-system.git market-data-system
cd market-data-system
```

**第 2 步：编译可执行文件**

```bash
cmake -S . -B build-cli \
  -DMDS_BUILD_LIBRARY=OFF \
  -DMDS_BUILD_CLI=ON \
  -DMDS_BUILD_EXAMPLES=OFF
cmake --build build-cli -j$(nproc)
```

**第 3 步：运行**

```bash
# live：实时接入并落盘
./build-cli/market-data-system

# replay：从 data/ 顺序回放
./build-cli/market-data-system --replay

# 离线压测
./build-cli/market-data-system --bench-pipeline 100000 --bench-write
```

---

### 流程二：编译静态库并接入你自己的项目

**第 0 步（仅第一次）：安装依赖**

```bash
sudo apt update
sudo apt install -y build-essential cmake libssl-dev libboost-all-dev nlohmann-json3-dev
```

**第 1 步：在库仓库里一键安装本地静态库**

```bash
# HTTPS
git clone https://github.com/hao971111/market-data-system.git market-data-system
# 或 SSH
# git clone git@github.com:hao971111/market-data-system.git market-data-system
cd market-data-system
bash scripts/install_local_lib.sh
```

执行后会生成本地安装目录：

- `market-data-system/_install/include/mds/*.h`
- `market-data-system/_install/lib/libmds_core.a`
- `market-data-system/_install/lib/cmake/mds/mdsConfig.cmake`

**第 2 步：在你的项目里写代码（示例 `main.cpp`）**

```cpp
#include <mds/replayer.h>
#include <mds/types.h>

int main() {
    mds::Replayer r("./data");
    auto result = r.replay_trades([](const mds::Trade&) {});
    return result.completed ? 0 : 1;
}
```

**第 3 步：编译你的项目（两种方式，任选一种）**

方式 A（最直接，不写 CMake）：

```bash
g++ -std=c++17 main.cpp \
  -I /绝对路径/market-data-system/_install/include \
  -L /绝对路径/market-data-system/_install/lib \
  -lmds_core -lssl -lcrypto -lpthread \
  -o my_app
```

方式 B（标准 CMake，用 `find_package`）：

你的 `CMakeLists.txt`：

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_app LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(mds CONFIG REQUIRED)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE mds::mds_core)
```

编译命令：

```bash
cmake -S . -B build \
  -DCMAKE_PREFIX_PATH=/绝对路径/market-data-system/_install
cmake --build build -j$(nproc)
```

完整代码可直接参考：

- `examples/recv_only.cpp`（实时接收）
- `examples/replay_only.cpp`（回放）

## 开发进度（简版）

- 实时接入 + 解析 + 落盘
- 回放能力
- 静态库拆分（`mds_core`）
- 公共 API（`feed` / `replayer`）与示例
- 库发布规范化基础版（头文件安装、`find_package` 支持）

更完整的架构说明、当前不足与后续路线，请查看 `ROADMAP.md`。

## License

MIT