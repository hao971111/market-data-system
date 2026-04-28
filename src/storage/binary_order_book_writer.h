#pragma once

#include "../common/types.h"
#include "binary_record_writer.h"
#include "trade_file_format.h"

namespace mds {

using BinaryOrderBookWriter =
    BinaryRecordWriter<OrderBookSnapshot, OrderBookFileHeader>;

}  // namespace mds
