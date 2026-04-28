#pragma once

#include "../common/types.h"
#include "binary_record_reader.h"
#include "trade_file_format.h"

namespace mds {

using BinaryOrderBookReader =
    BinaryRecordReader<OrderBookSnapshot, OrderBookFileHeader>;

}  // namespace mds
