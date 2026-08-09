#pragma once

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

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
        {"binance_main", "wss://stream.binance.com:9443/stream", 0, true},
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
    
    // HTTP CONNECT 代理 URL，格式 http://host:port
    // 空 = 直连。优先级：config.json["proxy_url"] > 环境变量 https_proxy/HTTPS_PROXY
    // 在 load() 里启动单线程读完，运行期间只读不写——绕开 getenv() 多线程不安全的坑
    std::string proxy_url;
    
    // 心跳检测配置
    // 场景：交易所行情订阅，正常情况下数据每秒几十条
    //   - 数据本身即心跳（有数据 = 连接活的）
    //   - no_data_timeout 是主要检测手段
    //   - ping 作为兜底：冷门币种/临时数据中断时保活
    // 参数约束：ping_interval < no_data_timeout（否则ping没机会发就先超时了）
    int ping_interval_ms = 1000;       // 1秒发一次Ping（兜底保活）
    int no_data_timeout_ms = 3000;     // 3秒无数据（容忍2次Ping失败）
    
    // reserved（Step 22）：断线补数，当前 load 可读入但尚未接入业务逻辑
    bool enable_data_recovery = true;
    std::string rest_api_url = "https://api.binance.com";
    
    // reserved（Step 21）：日志级别 0=DEBUG, 1=INFO, 2=WARN, 3=ERROR；当前未驱动实际日志
    int log_level = 1;

    // 把"消息处理线程"（live = io_context 线程；bench = main 线程）钉到指定 CPU 核。
    // -1 = 不绑核（默认）；>= 0 = 绑到该核。
    // 故意不在 load() 里读 config.json：单一来源 = CLI（--pin-cpu）。
    // 这个字段是性能验证用的临时开关，不应在配置文件里"半永久持久化"。
    int pin_cpu = -1;
    
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

        // 不认识的顶层字段打 WARN，避免"配了却没生效"
        static const std::unordered_set<std::string> kKnownKeys = {
            "symbols",
            "data_dir",
            "ring_buffer_size",
            "reconnect_interval_ms",
            "ping_interval_ms",
            "no_data_timeout_ms",
            "log_level",
            "proxy_url",
            "data_sources",
            "enable_data_recovery",
            "rest_api_url",
        };
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (kKnownKeys.find(it.key()) == kKnownKeys.end()) {
                std::cerr << "[WARN] Config unknown field ignored: " << it.key()
                          << std::endl;
            }
        }
        
        // 只覆盖JSON中存在的字段
        if (j.contains("symbols")) {
            symbols = j["symbols"].get<std::vector<std::string>>();
        }
        if (j.contains("data_dir")) {
            data_dir = j["data_dir"].get<std::string>();
        }
        if (j.contains("ring_buffer_size")) {
            // 上限 1e8（按 sizeof(Trade) ≈ 56B 估算约 5.6GB），挡住误配
            constexpr int64_t kMaxRingBufferSize = 100'000'000;
            const auto& v = j["ring_buffer_size"];

            // 1) 类型必须是整数（避免 get<int64_t>() 抛出不含字段名的 type_error）
            if (!v.is_number_integer()) {
                throw std::invalid_argument(
                    "Config invalid ring_buffer_size: must be integer");
            }
            // 2) 大于 INT64_MAX 的 unsigned 数会被截断成负数，先按 uint64 拦截
            if (v.is_number_unsigned() &&
                v.get<uint64_t>() > static_cast<uint64_t>(kMaxRingBufferSize)) {
                throw std::invalid_argument(
                    "Config invalid ring_buffer_size=" + std::to_string(v.get<uint64_t>()) +
                    " (must be 1.." + std::to_string(kMaxRingBufferSize) + ")");
            }
            // 3) 走到这里 raw 一定能如实反映用户原值（不会被截断）
            const auto raw = v.get<int64_t>();
            if (raw <= 0 || raw > kMaxRingBufferSize) {
                throw std::invalid_argument(
                    "Config invalid ring_buffer_size=" + std::to_string(raw) +
                    " (must be 1.." + std::to_string(kMaxRingBufferSize) + ")");
            }
            ring_buffer_size = static_cast<size_t>(raw);
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
        if (j.contains("enable_data_recovery")) {
            enable_data_recovery = j["enable_data_recovery"].get<bool>();
        }
        if (j.contains("rest_api_url")) {
            rest_api_url = j["rest_api_url"].get<std::string>();
        }

        // 代理：配置文件优先，没配置则 fallback 到环境变量
        // 注意 contains 区分"key 不存在"和"key 存在值为空"——
        //   显式写 "proxy_url": "" 表示用户主动声明"不要代理"，即使有环境变量也不走
        //   "proxy_url": null 走下面的类型校验抛错（与 ring_buffer_size 风格一致）
        if (j.contains("proxy_url")) {
            const auto& v = j["proxy_url"];
            // 类型校验：与同文件 ring_buffer_size 风格对齐，错误信息带字段名便于排查
            // nlohmann 自带的 get<std::string>() 抛 type_error 不含字段名，用户难定位
            if (!v.is_string()) {
                throw std::invalid_argument("Config invalid proxy_url: must be string");
            }
            proxy_url = v.get<std::string>();
        } else {
            const char* env = std::getenv("https_proxy");
            if (!env || *env == '\0') env = std::getenv("HTTPS_PROXY");
            if (env && *env != '\0') proxy_url = env;
        }

        // 数据源配置
        if (j.contains("data_sources")) {
            static const std::unordered_set<std::string> kKnownDataSourceKeys = {
                "name",
                "ws_url",
                "priority",
                "enabled",
            };
            data_sources.clear();
            std::size_t index = 0;
            for (const auto& ds : j["data_sources"]) {
                if (ds.is_object()) {
                    for (auto it = ds.begin(); it != ds.end(); ++it) {
                        if (kKnownDataSourceKeys.find(it.key()) ==
                            kKnownDataSourceKeys.end()) {
                            std::cerr
                                << "[WARN] Config unknown field ignored: data_sources["
                                << index << "]." << it.key() << std::endl;
                        }
                    }
                }
                DataSourceConfig cfg;
                cfg.name = ds.value("name", "unnamed");
                cfg.ws_url = ds.value("ws_url", "");
                cfg.priority = ds.value("priority", 0);
                cfg.enabled = ds.value("enabled", true);
                data_sources.push_back(cfg);
                ++index;
            }
        }

        // 时间参数范围：都必须 > 0，且 ping_interval_ms < no_data_timeout_ms
        if (reconnect_interval_ms <= 0) {
            throw std::invalid_argument(
                "Config invalid reconnect_interval_ms=" +
                std::to_string(reconnect_interval_ms) + " (must be > 0)");
        }
        if (ping_interval_ms <= 0) {
            throw std::invalid_argument(
                "Config invalid ping_interval_ms=" +
                std::to_string(ping_interval_ms) + " (must be > 0)");
        }
        if (no_data_timeout_ms <= 0) {
            throw std::invalid_argument(
                "Config invalid no_data_timeout_ms=" +
                std::to_string(no_data_timeout_ms) + " (must be > 0)");
        }
        if (!(ping_interval_ms < no_data_timeout_ms)) {
            throw std::invalid_argument(
                "Config invalid timing: ping_interval_ms=" +
                std::to_string(ping_interval_ms) +
                " must be < no_data_timeout_ms=" +
                std::to_string(no_data_timeout_ms));
        }
    }
};

}  // namespace mds
