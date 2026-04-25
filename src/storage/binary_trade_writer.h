#pragma once

#include "../common/types.h"
#include "trade_file_format.h"
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace mds {

class BinaryTradeWriter {
public:
    BinaryTradeWriter() = default;
    ~BinaryTradeWriter();

    bool open(const std::string& data_dir, size_t queue_capacity = 100000);
    bool write(const Trade& trade);
    void close();

    uint64_t records_written() const;
    uint64_t records_dropped() const;
    bool has_error() const;

private:
    void writer_loop();
    bool write_to_file(const Trade& trade);

    std::ofstream file_;
    std::mutex lifecycle_mutex_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Trade> queue_;
    std::thread worker_;
    bool running_ = false;
    bool accepting_ = false;
    size_t queue_capacity_ = 100000;
    std::atomic<uint64_t> records_written_{0};
    std::atomic<uint64_t> records_dropped_{0};
    std::atomic<bool> write_error_{false};
};

}  // namespace mds
