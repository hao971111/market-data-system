#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace mds {

// 单个数据源配置
struct DataSourceConfig {
    std::string name;        // 数据源名称，如 "binance_main"
    std::string ws_url;      // WebSocket地址
    int priority = 0;        // 优先级，数字小优先级高
    bool enabled = true;     // 是否启用
};

struct Config {
    // 数据源配置（架构支持多源，当前用单源）
    // 后续可扩展为双活模式：多源同时收数据，去重合并
    std::vector<DataSourceConfig> data_sources = {
        {"binance_main", "wss://stream.binance.com:9443/ws", 0, true},
        // {"binance_backup", "wss://stream.binance.com:443/ws", 1, false},  // 备用，暂不启用
    };
    
    // 订阅的交易对
    std::vector<std::string> symbols = {"btcusdt", "ethusdt"};
    
    // 存储配置
    std::string data_dir = "./data";
    
    // 缓冲区配置
    size_t ring_buffer_size = 100000;  // 环形缓冲区大小（10万条）
    
    // 连接配置
    int reconnect_interval_ms = 100;   // 初始重连间隔100ms，指数退避
    int max_reconnect_attempts = 0;    // 0表示无限重试
    
    // 心跳检测配置
    // 场景：交易所行情订阅，正常情况下数据每秒几十条
    //   - 数据本身即心跳（有数据 = 连接活的）
    //   - no_data_timeout 是主要检测手段
    //   - ping 作为兜底：冷门币种/临时数据中断时保活
    // 参数约束：ping_interval < no_data_timeout（否则ping没机会发就先超时了）
    int ping_interval_ms = 1000;       // 1秒发一次Ping（兜底保活）
    int no_data_timeout_ms = 3000;     // 3秒无数据（容忍2次Ping失败）
    
    // 数据补全配置
    bool enable_data_recovery = true;  // 断线重连后是否补拉数据
    std::string rest_api_url = "https://api.binance.com";
    
    // 日志级别: 0=DEBUG, 1=INFO, 2=WARN, 3=ERROR
    int log_level = 1;
    
    // 从JSON文件加载配置，覆盖默认值
    // 文件不存在或字段缺失时使用默认值
    void load(const std::string& filepath) {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            return;  // 文件不存在，用默认值
        }
        
        nlohmann::json j;
        try {
            file >> j;
        } catch (const nlohmann::json::parse_error& e) {
            throw std::runtime_error("Config parse error: " + std::string(e.what()));
        }
        
        // 只覆盖JSON中存在的字段
        if (j.contains("symbols")) {
            symbols = j["symbols"].get<std::vector<std::string>>();
        }
        if (j.contains("data_dir")) {
            data_dir = j["data_dir"].get<std::string>();
        }
        if (j.contains("ring_buffer_size")) {
            ring_buffer_size = j["ring_buffer_size"].get<size_t>();
        }
        if (j.contains("reconnect_interval_ms")) {
            reconnect_interval_ms = j["reconnect_interval_ms"].get<int>();
        }
        if (j.contains("ping_interval_ms")) {
            ping_interval_ms = j["ping_interval_ms"].get<int>();
        }
        if (j.contains("no_data_timeout_ms")) {
            no_data_timeout_ms = j["no_data_timeout_ms"].get<int>();
        }
        if (j.contains("log_level")) {
            log_level = j["log_level"].get<int>();
        }
        
        // 数据源配置
        if (j.contains("data_sources")) {
            data_sources.clear();
            for (const auto& ds : j["data_sources"]) {
                DataSourceConfig cfg;
                cfg.name = ds.value("name", "unnamed");
                cfg.ws_url = ds.value("ws_url", "");
                cfg.priority = ds.value("priority", 0);
                cfg.enabled = ds.value("enabled", true);
                data_sources.push_back(cfg);
            }
        }
    }
};

}  // namespace mds
