#include "binary_trade_writer.h"
#include <filesystem>
#include <iostream>
#include <vector>

namespace mds {

BinaryTradeWriter::~BinaryTradeWriter() {
    close();
}

bool BinaryTradeWriter::open(const std::string& data_dir, size_t queue_capacity) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(data_dir, ec);
    if (ec) {
        std::cerr << "[ERROR] Failed to create data dir: " << ec.message() << std::endl;
        return false;
    }

    const auto path = std::filesystem::path(data_dir) / "trades.bin";
    file_.open(path, std::ios::binary | std::ios::trunc);
    if (!file_.is_open()) {
        std::cerr << "[ERROR] Failed to open trade file: " << path << std::endl;
        return false;
    }

    TradeFileHeader header;
    file_.write(reinterpret_cast<const char*>(&header), sizeof(header));
    if (!file_.good()) {
        std::cerr << "[ERROR] Failed to write trade file header: " << path << std::endl;
        file_.close();
        return false;
    }

    queue_capacity_ = queue_capacity;
    records_written_.store(0, std::memory_order_relaxed);
    records_dropped_.store(0, std::memory_order_relaxed);
    write_error_.store(false, std::memory_order_relaxed);
    running_ = true;
    accepting_ = true;
    worker_ = std::thread(&BinaryTradeWriter::writer_loop, this);
    return true;
}

bool BinaryTradeWriter::write(const Trade& trade) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!accepting_) {
        return false;
    }

    if (queue_.size() >= queue_capacity_) {
        records_dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    queue_.push_back(trade);
    cv_.notify_one();
    return true;
}

void BinaryTradeWriter::close() {
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
        file_.flush();
        file_.close();
        if (file_.fail()) {
            write_error_.store(true, std::memory_order_relaxed);
            std::cerr << "[ERROR] Trade file flush/close failed; final batch may be lost"
                      << std::endl;
        }
    }
}

uint64_t BinaryTradeWriter::records_written() const {
    return records_written_.load(std::memory_order_relaxed);
}

uint64_t BinaryTradeWriter::records_dropped() const {
    return records_dropped_.load(std::memory_order_relaxed);
}

bool BinaryTradeWriter::has_error() const {
    return write_error_.load(std::memory_order_relaxed);
}

void BinaryTradeWriter::writer_loop() {
    std::vector<Trade> batch;
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
            std::cerr << "[ERROR] Trade writer stopped after file write failure" << std::endl;
            break;
        }

        batch.clear();
    }
}

bool BinaryTradeWriter::write_to_file(const Trade& trade) {
    file_.write(reinterpret_cast<const char*>(&trade), sizeof(trade));
    return file_.good();
}

}  // namespace mds
