#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
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
// Header 需要提供：
//   - static constexpr const char* file_name
//   - 默认构造对象可直接写入文件头
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

        const auto path = std::filesystem::path(data_dir) / Header::file_name;
        file_.open(path, std::ios::binary | std::ios::trunc);
        if (!file_.is_open()) {
            std::cerr << "[ERROR] Failed to open binary file: " << path << std::endl;
            return false;
        }

        Header header;
        file_.write(reinterpret_cast<const char*>(&header), sizeof(header));
        if (!file_.good()) {
            std::cerr << "[ERROR] Failed to write binary file header: " << path << std::endl;
            file_.close();
            return false;
        }

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
        if (file_.is_open()) {
            // worker 已 join，所有记录都已写入；回填 header 里的 record_count。
            // 只在无写盘错误时回填，避免用不完整计数覆盖掉已损坏文件的头部。
            // 只写 record_count 字段本身（8 字节），不重写整个 header，
            // 避免默认构造 Header 后回写覆盖 magic/version 等字段的风险。
            if (!write_error_.load(std::memory_order_relaxed)) {
                const uint64_t n = records_written_.load(std::memory_order_relaxed);
                file_.seekp(static_cast<std::streamoff>(Header::record_count_offset()));
                file_.write(reinterpret_cast<const char*>(&n), sizeof(n));
                if (file_.fail()) {
                    write_error_.store(true, std::memory_order_relaxed);
                    std::cerr << "[ERROR] Failed to write record_count to file header"
                              << std::endl;
                }
            }
            file_.flush();
            file_.close();
            if (file_.fail()) {
                write_error_.store(true, std::memory_order_relaxed);
                std::cerr << "[ERROR] Binary file flush/close failed; final batch may be lost"
                          << std::endl;
            }
        }
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

    bool write_to_file(const Record& record) {
        file_.write(reinterpret_cast<const char*>(&record), sizeof(record));
        return file_.good();
    }

    std::ofstream file_;
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
