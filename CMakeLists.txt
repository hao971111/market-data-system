cmake_minimum_required(VERSION 3.16)
project(market-data-system VERSION 0.1.0 LANGUAGES CXX)

# C++17标准，现代C++特性支持
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 编译选项
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # 生成compile_commands.json，方便IDE

# 编译模式设置
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Release模式优化，Debug模式调试信息
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fsanitize=address -fno-omit-frame-pointer")

# 警告选项：严格警告有助于发现潜在问题
add_compile_options(-Wall -Wextra -Wpedantic -Werror)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

# 构建开关：
# - MDS_BUILD_LIBRARY=ON  : 构建静态库 mds_core
# - MDS_BUILD_CLI=ON      : 构建 CLI 可执行文件 market-data-system
# - MDS_BUILD_EXAMPLES=ON : 构建示例程序（replay_only / recv_only）
#
# 默认三者都开，保持当前行为不变。
option(MDS_BUILD_LIBRARY "Build mds_core static library" ON)
option(MDS_BUILD_CLI "Build market-data-system executable" ON)
option(MDS_BUILD_EXAMPLES "Build examples" ON)

# 核心库源码：显式列出，新增/删除文件时必须改这里，避免 GLOB 漏感知
set(MDS_CORE_SOURCES
    src/network/connection_manager.cpp
    src/network/websocket_client.cpp
    src/parser/parser.cpp
    src/monitor/cpu_affinity.cpp
    src/benchmark/pipeline_benchmark.cpp
    src/api/replayer.cpp
    src/api/feed.cpp
)

if(MDS_BUILD_LIBRARY)
    add_library(mds_core STATIC ${MDS_CORE_SOURCES})

    target_include_directories(mds_core
        PUBLIC
            ${PROJECT_SOURCE_DIR}/include
            ${PROJECT_SOURCE_DIR}/src
    )

    target_link_libraries(mds_core
        PUBLIC
            Threads::Threads
            OpenSSL::SSL
            OpenSSL::Crypto
    )
endif()

# CLI 构建：
# - 若已构建 mds_core：CLI 只编 main.cpp 并链接库（库化路径）
# - 若未构建 mds_core：CLI 直接编 main.cpp + core sources（独立可执行路径）
if(MDS_BUILD_CLI)
    if(MDS_BUILD_LIBRARY)
        add_executable(${PROJECT_NAME} src/main.cpp)
        target_link_libraries(${PROJECT_NAME}
            PRIVATE
                mds_core
        )
    else()
        add_executable(${PROJECT_NAME}
            src/main.cpp
            ${MDS_CORE_SOURCES}
        )
        target_include_directories(${PROJECT_NAME}
            PRIVATE
                ${PROJECT_SOURCE_DIR}/include
                ${PROJECT_SOURCE_DIR}/src
        )
        target_link_libraries(${PROJECT_NAME}
            PRIVATE
                Threads::Threads
                OpenSSL::SSL
                OpenSSL::Crypto
        )
    endif()
endif()

# 示例程序依赖库 API（<mds/*.h> + facade 实现），要求 mds_core 可用
if(MDS_BUILD_EXAMPLES)
    if(NOT MDS_BUILD_LIBRARY)
        message(FATAL_ERROR "MDS_BUILD_EXAMPLES=ON requires MDS_BUILD_LIBRARY=ON")
    endif()
    add_executable(replay_only examples/replay_only.cpp)
    target_link_libraries(replay_only PRIVATE mds_core)
    add_executable(recv_only examples/recv_only.cpp)
    target_link_libraries(recv_only PRIVATE mds_core)
endif()

# 数据目录
set(DATA_DIR "${PROJECT_SOURCE_DIR}/data")
file(MAKE_DIRECTORY ${DATA_DIR})

# 安装规则
if(MDS_BUILD_LIBRARY AND MDS_BUILD_CLI)
    install(TARGETS ${PROJECT_NAME} mds_core
        RUNTIME DESTINATION bin
        ARCHIVE DESTINATION lib
    )
elseif(MDS_BUILD_LIBRARY)
    install(TARGETS mds_core
        ARCHIVE DESTINATION lib
    )
elseif(MDS_BUILD_CLI)
    install(TARGETS ${PROJECT_NAME}
        RUNTIME DESTINATION bin
    )
endif()

# 测试支持（后续添加）
# enable_testing()
# add_subdirectory(tests)

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
