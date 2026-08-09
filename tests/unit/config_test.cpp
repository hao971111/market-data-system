#include "../../src/config/config.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>

#include <gtest/gtest.h>

namespace {

class ConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("mds_config_test_" + std::to_string(::getpid()));
        std::filesystem::create_directories(dir_);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    std::filesystem::path write_json(const std::string& name, const std::string& body) {
        const auto path = dir_ / name;
        std::ofstream out(path);
        out << body;
        out.close();
        return path;
    }

    std::filesystem::path dir_;
};

}  // namespace

TEST_F(ConfigTest, LoadAppliesPresentFields) {
    const auto path = write_json("full.json", R"({
        "symbols": ["solusdt"],
        "data_dir": "/tmp/mds-test-data",
        "ring_buffer_size": 1234,
        "reconnect_interval_ms": 250,
        "ping_interval_ms": 2000,
        "no_data_timeout_ms": 5000,
        "log_level": 2,
        "proxy_url": "http://127.0.0.1:8080",
        "data_sources": [
            {
                "name": "primary",
                "ws_url": "wss://example.test/stream",
                "priority": 3,
                "enabled": false
            }
        ]
    })");

    mds::Config cfg;
    cfg.load(path.string());

    ASSERT_EQ(cfg.symbols.size(), 1u);
    EXPECT_EQ(cfg.symbols[0], "solusdt");
    EXPECT_EQ(cfg.data_dir, "/tmp/mds-test-data");
    EXPECT_EQ(cfg.ring_buffer_size, 1234u);
    EXPECT_EQ(cfg.reconnect_interval_ms, 250);
    EXPECT_EQ(cfg.ping_interval_ms, 2000);
    EXPECT_EQ(cfg.no_data_timeout_ms, 5000);
    EXPECT_EQ(cfg.log_level, 2);
    EXPECT_EQ(cfg.proxy_url, "http://127.0.0.1:8080");
    ASSERT_EQ(cfg.data_sources.size(), 1u);
    EXPECT_EQ(cfg.data_sources[0].name, "primary");
    EXPECT_EQ(cfg.data_sources[0].ws_url, "wss://example.test/stream");
    EXPECT_EQ(cfg.data_sources[0].priority, 3);
    EXPECT_FALSE(cfg.data_sources[0].enabled);
}

TEST_F(ConfigTest, LoadMissingFieldsKeepsDefaults) {
    unsetenv("https_proxy");
    unsetenv("HTTPS_PROXY");

    const auto path = write_json("empty.json", "{}");

    mds::Config cfg;
    const auto defaults = cfg;
    cfg.load(path.string());

    EXPECT_EQ(cfg.symbols, defaults.symbols);
    EXPECT_EQ(cfg.data_dir, defaults.data_dir);
    EXPECT_EQ(cfg.ring_buffer_size, defaults.ring_buffer_size);
    EXPECT_EQ(cfg.reconnect_interval_ms, defaults.reconnect_interval_ms);
    EXPECT_EQ(cfg.ping_interval_ms, defaults.ping_interval_ms);
    EXPECT_EQ(cfg.no_data_timeout_ms, defaults.no_data_timeout_ms);
    EXPECT_EQ(cfg.log_level, defaults.log_level);
    EXPECT_EQ(cfg.proxy_url, defaults.proxy_url);
    ASSERT_EQ(cfg.data_sources.size(), defaults.data_sources.size());
    EXPECT_EQ(cfg.data_sources[0].name, defaults.data_sources[0].name);
    EXPECT_EQ(cfg.data_sources[0].ws_url, defaults.data_sources[0].ws_url);
}

TEST_F(ConfigTest, LoadMissingFileKeepsDefaults) {
    mds::Config cfg;
    const auto defaults = cfg;
    cfg.load((dir_ / "does_not_exist.json").string());

    EXPECT_EQ(cfg.symbols, defaults.symbols);
    EXPECT_EQ(cfg.ring_buffer_size, defaults.ring_buffer_size);
    EXPECT_EQ(cfg.data_dir, defaults.data_dir);
}

TEST_F(ConfigTest, LoadRejectsInvalidRingBufferSize) {
    mds::Config cfg;

    EXPECT_THROW(
        cfg.load(write_json("ring_zero.json", R"({"ring_buffer_size": 0})").string()),
        std::invalid_argument);
    EXPECT_THROW(
        cfg.load(write_json("ring_neg.json", R"({"ring_buffer_size": -1})").string()),
        std::invalid_argument);
    EXPECT_THROW(
        cfg.load(write_json("ring_huge.json", R"({"ring_buffer_size": 100000001})").string()),
        std::invalid_argument);
    EXPECT_THROW(
        cfg.load(write_json("ring_float.json", R"({"ring_buffer_size": 1.5})").string()),
        std::invalid_argument);
    EXPECT_THROW(
        cfg.load(write_json("ring_str.json", R"({"ring_buffer_size": "100"})").string()),
        std::invalid_argument);
}

TEST_F(ConfigTest, LoadRejectsNonStringProxyUrl) {
    mds::Config cfg;

    EXPECT_THROW(
        cfg.load(write_json("proxy_num.json", R"({"proxy_url": 123})").string()),
        std::invalid_argument);
    EXPECT_THROW(
        cfg.load(write_json("proxy_null.json", R"({"proxy_url": null})").string()),
        std::invalid_argument);
    EXPECT_THROW(
        cfg.load(write_json("proxy_obj.json", R"({"proxy_url": {"host": "x"}})").string()),
        std::invalid_argument);
}

TEST_F(ConfigTest, LoadRejectsNonPositiveTimingFields) {
    mds::Config cfg;

    EXPECT_THROW(
        cfg.load(write_json("reconnect_zero.json",
                             R"({"reconnect_interval_ms": 0})").string()),
        std::invalid_argument);
    EXPECT_THROW(
        cfg.load(write_json("reconnect_neg.json",
                             R"({"reconnect_interval_ms": -1})").string()),
        std::invalid_argument);
    EXPECT_THROW(
        cfg.load(write_json("ping_zero.json", R"({"ping_interval_ms": 0})").string()),
        std::invalid_argument);
    EXPECT_THROW(
        cfg.load(write_json("timeout_zero.json",
                             R"({"no_data_timeout_ms": 0})").string()),
        std::invalid_argument);
}

TEST_F(ConfigTest, LoadRejectsPingIntervalNotLessThanNoDataTimeout) {
    mds::Config cfg;

    // 默认 no_data_timeout_ms=3000，ping=3000 不满足 <
    EXPECT_THROW(
        cfg.load(write_json("ping_eq.json",
                             R"({"ping_interval_ms": 3000})").string()),
        std::invalid_argument);
    EXPECT_THROW(
        cfg.load(write_json("ping_gt.json", R"({
            "ping_interval_ms": 5000,
            "no_data_timeout_ms": 3000
        })").string()),
        std::invalid_argument);
}

TEST_F(ConfigTest, LoadWarnsOnUnknownField) {
    mds::Config cfg;

    std::stringstream buffer;
    auto* old = std::cerr.rdbuf(buffer.rdbuf());
    cfg.load(write_json("unknown.json", R"({
        "pong_timeout_ms": 1000,
        "ping_interval_ms": 1000,
        "no_data_timeout_ms": 3000
    })").string());
    std::cerr.rdbuf(old);

    EXPECT_NE(buffer.str().find("pong_timeout_ms"), std::string::npos);
    EXPECT_NE(buffer.str().find("[WARN]"), std::string::npos);
}

TEST_F(ConfigTest, LoadWarnsOnUnknownDataSourceField) {
    mds::Config cfg;

    std::stringstream buffer;
    auto* old = std::cerr.rdbuf(buffer.rdbuf());
    cfg.load(write_json("unknown_ds.json", R"({
        "data_sources": [
            {
                "name": "primary",
                "ws_ur": "wss://example.test/stream",
                "priority": 0,
                "enabled": true
            }
        ]
    })").string());
    std::cerr.rdbuf(old);

    EXPECT_NE(buffer.str().find("data_sources[0].ws_ur"), std::string::npos);
    EXPECT_NE(buffer.str().find("[WARN]"), std::string::npos);
    ASSERT_EQ(cfg.data_sources.size(), 1u);
    EXPECT_EQ(cfg.data_sources[0].name, "primary");
    EXPECT_EQ(cfg.data_sources[0].ws_url, "");  // 错字字段未生效，回落默认空串
}


