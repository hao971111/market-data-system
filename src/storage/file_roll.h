#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mds {

// 按 UTC 小时切分：trades_YYYYMMDD_HH.bin / orderbooks_YYYYMMDD_HH.bin。
// 文件名按字典序即时间序。只认这种小时文件，其它名字（含旧的 trades.bin）忽略。

struct TimeWindow {
    std::optional<int64_t> from_us;  // 含端点；nullopt = 不限制
    std::optional<int64_t> to_us;

    bool empty() const { return !from_us.has_value() && !to_us.has_value(); }
    bool valid() const {
        return !(from_us.has_value() && to_us.has_value() && *from_us > *to_us);
    }
    bool contains(int64_t ts_us) const {
        if (from_us.has_value() && ts_us < *from_us) {
            return false;
        }
        if (to_us.has_value() && ts_us > *to_us) {
            return false;
        }
        return true;
    }
};

inline std::string hourly_file_name(const char* prefix, int64_t ts_us) {
    if (ts_us < 0) {
        ts_us = 0;
    }
    const std::time_t sec = static_cast<std::time_t>(ts_us / 1'000'000);
    std::tm tm{};
    if (gmtime_r(&sec, &tm) == nullptr) {
        tm = {};
    }
    char buf[80];
    std::snprintf(buf, sizeof(buf), "%s_%04d%02d%02d_%02d.bin",
                  prefix,
                  tm.tm_year + 1900,
                  tm.tm_mon + 1,
                  tm.tm_mday,
                  tm.tm_hour);
    return std::string(buf);
}

inline bool is_hourly_file_name(std::string_view prefix, std::string_view name) {
    const std::string head = std::string(prefix) + "_";
    constexpr std::string_view tail = ".bin";
    // prefix_ + YYYYMMDD + _ + HH + .bin
    const std::size_t expect = head.size() + 8 + 1 + 2 + tail.size();
    if (name.size() != expect) {
        return false;
    }
    if (name.compare(0, head.size(), head) != 0) {
        return false;
    }
    if (name.compare(name.size() - tail.size(), tail.size(), tail) != 0) {
        return false;
    }
    const char* p = name.data() + head.size();
    for (int i = 0; i < 8; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(p[i]))) {
            return false;
        }
    }
    if (p[8] != '_') {
        return false;
    }
    return std::isdigit(static_cast<unsigned char>(p[9])) &&
           std::isdigit(static_cast<unsigned char>(p[10]));
}

// 小时文件名按字典序即时间序，和 hourly_file_name(from/to) 做闭区间比较。
inline bool hourly_file_in_window(const char* prefix, std::string_view name,
                                  const TimeWindow& window) {
    if (!is_hourly_file_name(prefix, name)) {
        return false;
    }
    if (window.from_us.has_value() &&
        name < hourly_file_name(prefix, *window.from_us)) {
        return false;
    }
    if (window.to_us.has_value() &&
        name > hourly_file_name(prefix, *window.to_us)) {
        return false;
    }
    return true;
}

inline std::vector<std::filesystem::path> list_record_files(
    const std::filesystem::path& dir,
    const char* prefix,
    const TimeWindow& window = {}) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
        return out;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec || !entry.is_regular_file(ec)) {
            continue;
        }
        const auto name = entry.path().filename().string();
        if (hourly_file_in_window(prefix, name, window)) {
            out.push_back(entry.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace mds
