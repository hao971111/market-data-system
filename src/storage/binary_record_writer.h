#pragma once

#include "file_roll.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace mds {

// 异步二进制写盘模板：生产者只入队，后台线程批量落盘。
// 按 UTC 小时切文件（Header::file_prefix_YYYYMMDD_HH.bin），启动不 trunc。
// Header 需要提供：
//   - static constexpr const char* file_prefix / file_name
//   - 默认构造对象可直接写入文件头
//   - record_count_offset()
// Record 需要有 recv_ts_us（<=0 时用写入时刻的 wall clock）。
template <typename Record, typename Header>
class BinaryRecordWriter {
    static_assert(std::is_trivially_copyable<Record>::value,
                  "BinaryRecordWriter Record must be trivially copyable");
    static_assert(std::is_standard_layout<Record>::value,
                  "BinaryRecordWriter Record must have standard layout");
    static_assert(std::is_trivially_copyable<Header>::value,
                  "BinaryRecordWriter Header must be trivially copyable");
    static_assert(std::is_standard_layout<Header>::value,
                  "BinaryRecordWriter Header must have standard layout");

public:
    BinaryRecordWriter() = default;

    ~BinaryRecordWriter() {
        close();
    }

    bool open(const std::string& data_dir, size_t queue_capacity = 100000) {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            return false;
        }
        if (queue_capacity == 0) {
            std::cerr << "[ERROR] Binary writer queue_capacity must be > 0" << std::endl;
            return false;
        }

        std::error_code ec;
        std::filesystem::create_directories(data_dir, ec);
        if (ec) {
            std::cerr << "[ERROR] Failed to create data dir: " << ec.message() << std::endl;
            return false;
        }

        data_dir_ = data_dir;
        current_name_.clear();
        file_record_count_ = 0;
        queue_capacity_ = queue_capacity;
        records_written_.store(0, std::memory_order_relaxed);
        records_dropped_.store(0, std::memory_order_relaxed);
        write_error_.store(false, std::memory_order_relaxed);
        queue_depth_max_ = 0;
        running_ = true;
        accepting_ = true;
        worker_ = std::thread(&BinaryRecordWriter::writer_loop, this);
        return true;
    }

    bool write(const Record& record) {
        bool should_notify = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!accepting_ || !running_) {
                return false;
            }

            if (queue_.size() >= queue_capacity_) {
                records_dropped_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }

            should_notify = queue_.empty();
            queue_.push_back(record);
            const uint64_t depth_after_push = static_cast<uint64_t>(queue_.size());
            if (depth_after_push > queue_depth_max_) {
                queue_depth_max_ = depth_after_push;
            }
        }
        if (should_notify) {
            cv_.notify_one();
        }
        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            accepting_ = false;
            running_ = false;
        }
        cv_.notify_all();

        if (worker_.joinable()) {
            worker_.join();
        }

        std::lock_guard<std::mutex> lock(mutex_);
        close_current_file();
    }

    uint64_t records_written() const {
        return records_written_.load(std::memory_order_relaxed);
    }

    uint64_t records_dropped() const {
        return records_dropped_.load(std::memory_order_relaxed);
    }

    bool has_error() const {
        return write_error_.load(std::memory_order_relaxed);
    }

    uint64_t queue_depth_current() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<uint64_t>(queue_.size());
    }

    uint64_t queue_depth_max() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_depth_max_;
    }

private:
    void writer_loop() {
        std::vector<Record> batch;
        batch.reserve(1024);

        while (true) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return !running_ || !queue_.empty(); });
                if (!running_ && queue_.empty()) {
                    break;
                }

                while (!queue_.empty() && batch.size() < 1024) {
                    batch.push_back(queue_.front());
                    queue_.pop_front();
                }
            }

            uint64_t written = 0;
            bool write_failed = false;
            for (size_t i = 0; i < batch.size(); ++i) {
                if (!write_to_file(batch[i])) {
                    write_failed = true;
                    break;
                }
                ++written;
            }
            records_written_.fetch_add(written, std::memory_order_relaxed);

            if (write_failed) {
                std::lock_guard<std::mutex> lock(mutex_);
                write_error_.store(true, std::memory_order_relaxed);
                accepting_ = false;
                running_ = false;
                records_dropped_.fetch_add(batch.size() - written + queue_.size(),
                                           std::memory_order_relaxed);
                queue_.clear();
                std::cerr << "[ERROR] Binary writer stopped after file write failure"
                          << std::endl;
                break;
            }

            batch.clear();
        }
    }

    static int64_t record_ts_us(const Record& record) {
        if (record.recv_ts_us > 0) {
            return record.recv_ts_us;
        }
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    bool write_to_file(const Record& record) {
        const std::string name =
            hourly_file_name(Header::file_prefix, record_ts_us(record));
        if (name != current_name_) {
            if (!close_current_file()) {
                return false;
            }
            if (!open_file(name)) {
                return false;
            }
        }

        file_.write(reinterpret_cast<const char*>(&record), sizeof(record));
        if (!file_.good()) {
            return false;
        }
        ++file_record_count_;
        return true;
    }

    bool close_current_file() {
        if (!file_.is_open()) {
            current_name_.clear();
            file_record_count_ = 0;
            return true;
        }

        bool ok = true;
        if (!write_error_.load(std::memory_order_relaxed)) {
            file_.clear();
            file_.seekp(static_cast<std::streamoff>(Header::record_count_offset()));
            const uint64_t n = file_record_count_;
            file_.write(reinterpret_cast<const char*>(&n), sizeof(n));
            if (file_.fail()) {
                ok = false;
                write_error_.store(true, std::memory_order_relaxed);
                std::cerr << "[ERROR] Failed to write record_count to file header"
                          << std::endl;
            }
        }
        file_.flush();
        file_.close();
        if (file_.fail()) {
            ok = false;
            write_error_.store(true, std::memory_order_relaxed);
            std::cerr << "[ERROR] Binary file flush/close failed; final batch may be lost"
                      << std::endl;
        }
        current_name_.clear();
        file_record_count_ = 0;
        return ok;
    }

    bool open_file(const std::string& name) {
        const auto path = std::filesystem::path(data_dir_) / name;
        std::error_code ec;
        const bool exists = std::filesystem::exists(path, ec);

        if (exists) {
            file_.open(path, std::ios::binary | std::ios::in | std::ios::out);
            if (!file_.is_open()) {
                std::cerr << "[ERROR] Failed to reopen binary file: " << path << std::endl;
                return false;
            }

            Header header{};
            file_.read(reinterpret_cast<char*>(&header), sizeof(header));
            if (!file_.good()) {
                std::cerr << "[ERROR] Invalid binary file header: " << path << std::endl;
                file_.close();
                return false;
            }
            const Header expected;
            if (std::memcmp(header.magic, expected.magic, sizeof(expected.magic)) != 0 ||
                header.version != expected.version ||
                header.record_size != sizeof(Record)) {
                std::cerr << "[ERROR] Existing file header mismatch, refusing to append: "
                          << path << std::endl;
                file_.close();
                return false;
            }

            const auto sz = std::filesystem::file_size(path, ec);
            if (ec || sz < sizeof(Header) ||
                (sz - sizeof(Header)) % sizeof(Record) != 0) {
                std::cerr << "[ERROR] Existing file size is not a complete record set: "
                          << path << std::endl;
                file_.close();
                return false;
            }
            file_record_count_ = static_cast<uint64_t>(
                (sz - sizeof(Header)) / sizeof(Record));
            file_.clear();
            file_.seekp(0, std::ios::end);
            if (!file_.good()) {
                std::cerr << "[ERROR] Failed to seek to end: " << path << std::endl;
                file_.close();
                return false;
            }
        } else {
            file_.open(path, std::ios::binary | std::ios::out | std::ios::trunc);
            if (!file_.is_open()) {
                std::cerr << "[ERROR] Failed to open binary file: " << path << std::endl;
                return false;
            }
            Header header;
            file_.write(reinterpret_cast<const char*>(&header), sizeof(header));
            if (!file_.good()) {
                std::cerr << "[ERROR] Failed to write binary file header: " << path
                          << std::endl;
                file_.close();
                return false;
            }
            file_record_count_ = 0;
        }

        current_name_ = name;
        return true;
    }

    std::fstream file_;
    std::string data_dir_;
    std::string current_name_;
    uint64_t file_record_count_ = 0;
    std::mutex lifecycle_mutex_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Record> queue_;
    std::thread worker_;
    bool running_ = false;
    bool accepting_ = false;
    size_t queue_capacity_ = 100000;
    std::atomic<uint64_t> records_written_{0};
    std::atomic<uint64_t> records_dropped_{0};
    std::atomic<bool> write_error_{false};
    uint64_t queue_depth_max_ = 0;
};

}  // namespace mds
