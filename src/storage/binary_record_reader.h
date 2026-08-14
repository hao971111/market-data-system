#pragma once

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <type_traits>

namespace mds {

// 顺序读取二进制记录文件的通用模板：和 BinaryRecordWriter 对称。
// Header 需要提供：
//   - static constexpr const char* file_name
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

        const auto path = std::filesystem::path(data_dir) / Header::file_name;
        file_.open(path, std::ios::binary);
        if (!file_.is_open()) {
            std::cerr << "[ERROR] Failed to open binary file for reading: " << path
                      << std::endl;
            return false;
        }

        file_.read(reinterpret_cast<char*>(&header_), sizeof(header_));
        if (!file_.good()) {
            std::cerr << "[ERROR] Invalid binary file header: " << path << std::endl;
            close();
            return false;
        }
        const Header expected;
        if (std::memcmp(header_.magic, expected.magic, sizeof(expected.magic)) != 0) {
            std::cerr << "[ERROR] Invalid binary file magic: " << path << std::endl;
            close();
            return false;
        }
        if (header_.version != expected.version) {
            std::cerr << "[ERROR] Unsupported file version " << header_.version
                      << " (need " << expected.version
                      << "); old data/*.bin is incompatible: " << path << std::endl;
            close();
            return false;
        }
        if (header_.record_size != sizeof(Record)) {
            std::cerr << "[ERROR] record_size mismatch: file=" << header_.record_size
                      << " expected=" << sizeof(Record) << " path=" << path << std::endl;
            close();
            return false;
        }

        return true;
    }

    // 返回 header 中记录的条数（由写盘方 close() 时回填）。
    // 0 表示旧格式文件、写盘异常中止或写入零条。
    uint64_t get_record_count() const { return header_.get_record_count(); }

    bool read_next(Record& record) {
        if (!file_.is_open()) {
            return false;
        }

        file_.read(reinterpret_cast<char*>(&record), sizeof(record));
        const auto bytes_read = file_.gcount();
        if (bytes_read == static_cast<std::streamsize>(sizeof(record))) {
            return true;
        }

        if (bytes_read > 0) {
            read_error_ = true;
            std::cerr << "[ERROR] Truncated record: read " << bytes_read
                      << " bytes, expected " << sizeof(record) << std::endl;
        }

        return false;
    }

    void close() {
        if (file_.is_open()) {
            file_.close();
        }
    }

    bool has_error() const { return read_error_; }

private:
    std::ifstream file_;
    Header header_{};
    bool read_error_ = false;
};

}  // namespace mds
