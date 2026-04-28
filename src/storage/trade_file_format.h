#pragma once

#include "../common/types.h"
#include <cstdint>

namespace mds {

struct TradeFileHeader {
    static constexpr const char* file_name = "trades.bin";

    char     magic[8] = {'M', 'D', 'S', 'T', 'R', 'D', '1', '\0'};
    uint32_t version = 1;
    uint32_t record_size = sizeof(Trade);
};

struct OrderBookFileHeader {
    static constexpr const char* file_name = "orderbooks.bin";

    char     magic[8] = {'M', 'D', 'S', 'O', 'B', 'K', '1', '\0'};
    uint32_t version = 1;
    uint32_t record_size = sizeof(OrderBookSnapshot);
};

}  // namespace mds
