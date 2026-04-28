#pragma once

#include "../common/types.h"
#include "../storage/binary_order_book_reader.h"
#include "record_replayer.h"

namespace mds {

using OrderBookReplayer = RecordReplayer<OrderBookSnapshot, BinaryOrderBookReader>;

}  // namespace mds
