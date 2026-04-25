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

# 头文件目录
include_directories(${PROJECT_SOURCE_DIR}/include)
include_directories(${PROJECT_SOURCE_DIR}/src)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

# 源文件收集
file(GLOB_RECURSE SOURCES 
    "src/*.cpp"
)

# 主程序
add_executable(${PROJECT_NAME} ${SOURCES})

target_link_libraries(${PROJECT_NAME}
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
)

# 数据目录
set(DATA_DIR "${PROJECT_SOURCE_DIR}/data")
file(MAKE_DIRECTORY ${DATA_DIR})

# 安装规则
install(TARGETS ${PROJECT_NAME} DESTINATION bin)

# 测试支持（后续添加）
# enable_testing()
# add_subdirectory(tests)

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
