#pragma once

#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <string>

namespace mds {

struct ReplayResult {
    uint64_t records_replayed = 0;
    bool completed = false;
    bool file_error = false;
    bool callback_error = false;
};

// 顺序回放二进制记录文件的通用模板。
// Reader 需提供：bool open(const std::string&); bool read_next(Record&); void close(); bool has_error() const;
template <typename Record, typename Reader>
class RecordReplayer {
public:
    using Callback = std::function<void(const Record&)>;

    ReplayResult replay_all(const std::string& data_dir, Callback callback) {
        ReplayResult result;
        if (!callback) {
            std::cerr << "[ERROR] Replay callback is empty" << std::endl;
            result.callback_error = true;
            return result;
        }

        Reader reader;
        if (!reader.open(data_dir)) {
            result.file_error = true;
            return result;
        }

        Record record;
        while (reader.read_next(record)) {
            try {
                callback(record);
                ++result.records_replayed;
            } catch (const std::exception& e) {
                result.callback_error = true;
                std::cerr << "[ERROR] Replay callback failed: " << e.what() << std::endl;
                break;
            } catch (...) {
                result.callback_error = true;
                std::cerr << "[ERROR] Replay callback failed with unknown exception"
                          << std::endl;
                break;
            }
        }

        if (reader.has_error()) {
            result.file_error = true;
            std::cerr << "[ERROR] Replay stopped because file is corrupted" << std::endl;
        }

        reader.close();
        result.completed = !result.file_error && !result.callback_error;
        return result;
    }
};

}  // namespace mds
