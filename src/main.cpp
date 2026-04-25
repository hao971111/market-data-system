/**
 * @file main.cpp
 * @brief 高性能行情数据系统入口
 * 
 * 系统功能：
 * 1. 从Binance WebSocket接入实时行情（Trade + OrderBook）
 * 2. 高效解析和缓存
 * 3. 二进制格式持久化存储
 * 4. 按时间回放
 * 5. 提供标准化API供下游模块调用
 */

#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>

// 全局运行标志，用于优雅关闭
std::atomic<bool> g_running{true};

// 信号处理函数：捕获SIGINT(Ctrl+C)和SIGTERM，实现优雅关闭
void signal_handler(int signum) {
    std::cout << "\n[INFO] Received signal " << signum << ", shutting down..." << std::endl;
    g_running = false;
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    // 注册信号处理
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "==================================" << std::endl;
    std::cout << " Market Data System v0.1.0" << std::endl;
    std::cout << "==================================" << std::endl;
    std::cout << "[INFO] System starting..." << std::endl;

    // TODO: 初始化配置模块
    // TODO: 初始化WebSocket连接
    // TODO: 初始化存储模块
    // TODO: 启动数据处理循环

    // 主循环（后续替换为事件驱动）
    while (g_running) {
        // 暂时sleep，后续改为事件驱动
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "[INFO] System shutdown complete." << std::endl;
    return 0;
}
