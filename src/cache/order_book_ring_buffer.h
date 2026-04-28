#pragma once

#include "../common/types.h"
#include "ring_buffer.h"

namespace mds {

// 保存最近 N 个 OrderBookSnapshot，写满后覆盖最旧的。
// 设计场景与 TradeRingBuffer 对称：行情回调线程写，策略/监控线程读快照。
using OrderBookRingBuffer = RingBuffer<OrderBookSnapshot>;

}  // namespace mds
