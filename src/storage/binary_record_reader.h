#pragma once

#include "file_roll.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <type_traits>
#include <vector>

namespace mds {

// 顺序读取二进制记录文件的通用模板：和 BinaryRecordWriter 对称。
// 打开目录后按时间序读完所有小时文件（以及旧的单文件 Header::file_name）。
// Header 需要提供：
//   - static constexpr const char* file_prefix / file_name
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
        close();
        read_error_ = false;
        total_record_count_ = 0;
        file_index_ = 0;

        files_ = list_record_files(data_dir, Header::file_prefix, Header::file_name);
        if (files_.empty()) {
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
                return true;
            }

            if (bytes_read > 0) {
                read_error_ = true;
                std::cerr << "[ERROR] Truncated record: read " << bytes_read
                          << " bytes, expected " << sizeof(record) << std::endl;
                return false;
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
    }

    bool has_error() const { return read_error_; }

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
};

}  // namespace mds
