#pragma once

#include "../common/types.h"
#include <cstdint>

namespace mds {

struct TradeFileHeader {
    static constexpr const char* file_prefix = "trades";
    static constexpr const char* file_name = "trades.bin";  // 旧单文件名，Reader 仍识别

    char     magic[8] = {'M', 'D', 'S', 'T', 'R', 'D', '1', '\0'};
    uint32_t version = 2;
    uint32_t record_size = sizeof(Trade);
    // 写盘完成后由 BinaryRecordWriter::close() 回填；0 表示异常中止或旧格式文件
    uint64_t record_count = 0;

    void set_record_count(uint64_t n) { record_count = n; }
    uint64_t get_record_count() const { return record_count; }
    // BinaryRecordWriter 用此偏移做精确回写，避免重新构造整个 header
    static constexpr std::size_t record_count_offset() {
        return offsetof(TradeFileHeader, record_count);
    }
};

struct OrderBookFileHeader {
    static constexpr const char* file_prefix = "orderbooks";
    static constexpr const char* file_name = "orderbooks.bin";  // 旧单文件名，Reader 仍识别

    char     magic[8] = {'M', 'D', 'S', 'O', 'B', 'K', '1', '\0'};
    uint32_t version = 2;
    uint32_t record_size = sizeof(OrderBookSnapshot);
    // 写盘完成后由 BinaryRecordWriter::close() 回填；0 表示异常中止或旧格式文件
    uint64_t record_count = 0;

    void set_record_count(uint64_t n) { record_count = n; }
    uint64_t get_record_count() const { return record_count; }
    static constexpr std::size_t record_count_offset() {
        return offsetof(OrderBookFileHeader, record_count);
    }
};

}  // namespace mds
