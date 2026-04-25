#pragma once

#include "../common/types.h"
#include <cstdint>

namespace mds {

struct TradeFileHeader {
    char     magic[8] = {'M', 'D', 'S', 'T', 'R', 'D', '1', '\0'};
    uint32_t version = 1;
    uint32_t record_size = sizeof(Trade);
};

}  // namespace mds
