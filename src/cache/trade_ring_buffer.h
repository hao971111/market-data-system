#pragma once

#include "../common/types.h"
#include "ring_buffer.h"

namespace mds {

// 保存最近 N 条 Trade，写满后覆盖最旧的。
// 设计场景：SPMC（1 写 N 读）——网络线程单写，策略/监控等多线程并发读快照。
using TradeRingBuffer = RingBuffer<Trade>;

}  // namespace mds
