#pragma once

#include "../common/types.h"
#include "../storage/binary_trade_reader.h"
#include "record_replayer.h"

namespace mds {

using TradeReplayer = RecordReplayer<Trade, BinaryTradeReader>;

}  // namespace mds
