#pragma once

#include "file_roll.h"
#include "record_integrity.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <type_traits>
#include <vector>

namespace mds {

// 顺序读取二进制记录文件的通用模板：和 BinaryRecordWriter 对称。
// 打开目录后按时间序读完所有小时文件。
// 传入 TimeWindow 时只打开与窗口相交的小时文件，并按 recv_ts_us 丢掉窗外记录。
// Header 需要提供：
//   - static constexpr const char* file_prefix
//   - 可默认构造，对象内的字段与文件内已有头匹配（magic / version / record_size）
template <typename Record, typename Header>
class BinaryRecordReader {
    static_assert(std::is_trivially_copyable<Record>::value,
                  "BinaryRecordReader Record must be trivially copyable");
    static_assert(std::is_standard_layout<Record>::value,
                  "BinaryRecordReader Record must have standard layout");
    static_assert(std::is_trivially_copyable<Header>::value,
                  "BinaryRecordReader Header must be trivially copyable");
    static_assert(std::is_standard_layout<Header>::value,
                  "BinaryRecordReader Header must have standard layout");

public:
    bool open(const std::string& data_dir) {
        return open(data_dir, TimeWindow{});
    }

    // window 为空：扫全部小时文件。
    // window 非空：只打开文件名与窗口相交的小时文件；目录里没有这类文件算成功（0 条）。
    bool open(const std::string& data_dir, const TimeWindow& window) {
        close();
        read_error_ = false;
        if (!window.valid()) {
            std::cerr << "[ERROR] Invalid time window: from > to" << std::endl;
            return false;
        }
        window_ = window;
        windowed_ = !window.empty();

        files_ = list_record_files(data_dir, Header::file_prefix, window_);
        if (files_.empty()) {
            if (windowed_) {
                return true;
            }
            std::cerr << "[ERROR] No binary record files in " << data_dir
                      << " (expected " << Header::file_prefix
                      << "_YYYYMMDD_HH.bin)" << std::endl;
            return false;
        }

        for (const auto& path : files_) {
            std::ifstream probe(path, std::ios::binary);
            if (!probe.is_open()) {
                std::cerr << "[ERROR] Failed to open binary file for reading: " << path
                          << std::endl;
                files_.clear();
                return false;
            }
            Header header{};
            if (!read_and_check_header(probe, path, header)) {
                files_.clear();
                return false;
            }
            total_record_count_ += header.get_record_count();
        }

        return open_index(0);
    }

    // 所有已打开文件 header 中 record_count 之和（各文件 close 时回填）。
    // 0 表示旧格式、写盘异常中止或写入零条。
    uint64_t get_record_count() const { return total_record_count_; }

    bool read_next(Record& record) {
        while (file_.is_open()) {
            file_.read(reinterpret_cast<char*>(&record), sizeof(record));
            const auto bytes_read = file_.gcount();
            if (bytes_read == static_cast<std::streamsize>(sizeof(record))) {
                if (!record_crc_ok(record)) {
                    read_error_ = true;
                    std::cerr << "[ERROR] CRC mismatch in " << files_[file_index_]
                              << std::endl;
                    return false;
                }
                if (!window_.contains(record.recv_ts_us)) {
                    continue;
                }
                return true;
            }

            if (bytes_read > 0) {
                // 尾部半条：崩溃残留，不是中间损坏。丢掉尾巴，换下一个文件。
                std::cerr << "[WARN] Truncated record at end of " << files_[file_index_]
                          << ": read " << bytes_read << " bytes, expected "
                          << sizeof(record) << std::endl;
            }

            ++file_index_;
            if (file_index_ >= files_.size()) {
                file_.close();
                return false;
            }
            if (!open_index(file_index_)) {
                read_error_ = true;
                return false;
            }
        }
        return false;
    }

    void close() {
        if (file_.is_open()) {
            file_.close();
        }
        files_.clear();
        file_index_ = 0;
        total_record_count_ = 0;
        header_ = Header{};
        window_ = {};
        windowed_ = false;
    }

    bool has_error() const { return read_error_; }

    // 打开时带了 from/to。窗口回放时 header 条数和回调条数对不上是预期行为。
    bool time_window_active() const { return windowed_; }

private:
    static bool read_and_check_header(std::ifstream& file,
                                      const std::filesystem::path& path,
                                      Header& header) {
        file.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!file.good()) {
            std::cerr << "[ERROR] Invalid binary file header: " << path << std::endl;
            return false;
        }
        const Header expected;
        if (std::memcmp(header.magic, expected.magic, sizeof(expected.magic)) != 0) {
            std::cerr << "[ERROR] Invalid binary file magic: " << path << std::endl;
            return false;
        }
        if (header.version != expected.version) {
            std::cerr << "[ERROR] Unsupported file version " << header.version
                      << " (need " << expected.version
                      << "); old data/*.bin is incompatible: " << path << std::endl;
            return false;
        }
        if (header.record_size != sizeof(Record)) {
            std::cerr << "[ERROR] record_size mismatch: file=" << header.record_size
                      << " expected=" << sizeof(Record) << " path=" << path << std::endl;
            return false;
        }
        return true;
    }

    bool open_index(std::size_t index) {
        if (file_.is_open()) {
            file_.close();
        }
        if (index >= files_.size()) {
            return false;
        }
        const auto& path = files_[index];
        file_.open(path, std::ios::binary);
        if (!file_.is_open()) {
            std::cerr << "[ERROR] Failed to open binary file for reading: " << path
                      << std::endl;
            return false;
        }
        if (!read_and_check_header(file_, path, header_)) {
            file_.close();
            return false;
        }
        return true;
    }

    std::ifstream file_;
    Header header_{};
    std::vector<std::filesystem::path> files_;
    std::size_t file_index_ = 0;
    uint64_t total_record_count_ = 0;
    bool read_error_ = false;
    TimeWindow window_{};
    bool windowed_ = false;
};

}  // namespace mds
