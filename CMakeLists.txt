cmake_minimum_required(VERSION 3.16)
project(market-data-system VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

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

# Release 模式优化；Debug 模式只保留调试信息（不再绑死 ASAN）
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fno-omit-frame-pointer")

# 警告选项：严格警告有助于发现潜在问题
add_compile_options(-Wall -Wextra -Wpedantic -Werror)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Boost REQUIRED)
find_package(nlohmann_json REQUIRED)

# 构建开关：
# - MDS_BUILD_LIBRARY=ON  : 构建静态库 mds_core
# - MDS_BUILD_CLI=ON      : 构建 CLI 可执行文件 market-data-system
# - MDS_BUILD_EXAMPLES=ON : 构建示例程序（replay_only / recv_only）
# - MDS_BUILD_TESTS=ON    : 构建 gtest 单元测试并通过 ctest 跑
# - MDS_ENABLE_ASAN/UBSAN/TSAN : sanitizer，与 Debug/Release 正交，按需单独打开
#
# 默认前三者都开；测试与 sanitizer 默认关。
option(MDS_BUILD_LIBRARY "Build mds_core static library" ON)
option(MDS_BUILD_CLI "Build market-data-system executable" ON)
option(MDS_BUILD_EXAMPLES "Build examples" ON)
option(MDS_BUILD_TESTS "Build unit tests (gtest + ctest)" OFF)
option(MDS_ENABLE_ASAN "Enable AddressSanitizer (compile + link)" OFF)
option(MDS_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer (compile + link)" OFF)
option(MDS_ENABLE_TSAN "Enable ThreadSanitizer (compile + link)" OFF)

# ASAN 与 TSAN 不能同时开（运行时互斥）
if(MDS_ENABLE_ASAN AND MDS_ENABLE_TSAN)
    message(FATAL_ERROR "MDS_ENABLE_ASAN and MDS_ENABLE_TSAN cannot be enabled together")
endif()

# sanitizer 必须同时加到编译和链接；只写进 CXX_FLAGS_DEBUG 时链接侧常会漏掉
set(MDS_SANITIZER_FLAGS "")
if(MDS_ENABLE_ASAN)
    list(APPEND MDS_SANITIZER_FLAGS -fsanitize=address)
endif()
if(MDS_ENABLE_UBSAN)
    list(APPEND MDS_SANITIZER_FLAGS -fsanitize=undefined)
endif()
if(MDS_ENABLE_TSAN)
    list(APPEND MDS_SANITIZER_FLAGS -fsanitize=thread)
endif()
if(MDS_SANITIZER_FLAGS)
    # 栈帧指针便于 sanitizer 给出可读堆栈；与 Debug 正交，开 sanitizer 时一并打开
    add_compile_options(${MDS_SANITIZER_FLAGS} -fno-omit-frame-pointer)
    add_link_options(${MDS_SANITIZER_FLAGS})
endif()

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
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
            ${PROJECT_SOURCE_DIR}/src
    )

    target_link_libraries(mds_core
        PUBLIC
            Threads::Threads
            OpenSSL::SSL
            OpenSSL::Crypto
            Boost::headers
            nlohmann_json::nlohmann_json
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
                Boost::headers
                nlohmann_json::nlohmann_json
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
        EXPORT mdsTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
elseif(MDS_BUILD_LIBRARY)
    install(TARGETS mds_core
        EXPORT mdsTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
elseif(MDS_BUILD_CLI)
    install(TARGETS ${PROJECT_NAME}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

if(MDS_BUILD_LIBRARY)
    # 公共头安装：
    # - include/mds/*：用户直接 include <mds/...>
    install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/mds
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    # 导出 targets，供 find_package(mds CONFIG) 使用
    install(EXPORT mdsTargets
        FILE mdsTargets.cmake
        NAMESPACE mds::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mds
    )

    # 生成并安装 mdsConfig.cmake / mdsConfigVersion.cmake
    configure_package_config_file(
        ${PROJECT_SOURCE_DIR}/cmake/mdsConfig.cmake.in
        ${PROJECT_BINARY_DIR}/mdsConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mds
    )
    write_basic_package_version_file(
        ${PROJECT_BINARY_DIR}/mdsConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    install(FILES
        ${PROJECT_BINARY_DIR}/mdsConfig.cmake
        ${PROJECT_BINARY_DIR}/mdsConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mds
    )
endif()

if(MDS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
