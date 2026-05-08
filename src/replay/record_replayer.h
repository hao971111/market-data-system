#pragma once

#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <string>

namespace mds {

struct ReplayResult {
    uint64_t records_replayed = 0;
    uint64_t header_record_count = 0;  // header 里记录的条数（writer close 时回填）
    bool completed = false;
    bool file_error = false;
    bool callback_error = false;
    bool count_mismatch = false;       // replayed != header_record_count（且 header 非 0）
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

        result.header_record_count = reader.get_record_count();
        // file_error 时 replayed 必然 < header_count，是读取截断导致的，
        // 不额外设 count_mismatch（file_error 本身已说明问题）。
        if (!result.file_error &&
            result.header_record_count > 0 &&
            result.records_replayed != result.header_record_count) {
            result.count_mismatch = true;
            std::cerr << "[WARN] Record count mismatch: header=" << result.header_record_count
                      << " replayed=" << result.records_replayed << std::endl;
        }

        reader.close();
        result.completed = !result.file_error && !result.callback_error && !result.count_mismatch;
        return result;
    }
};

}  // namespace mds
