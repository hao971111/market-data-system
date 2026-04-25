#pragma once

#include "../common/types.h"
#include <cstdint>
#include <functional>
#include <string>

namespace mds {

using ReplayTradeCallback = std::function<void(const Trade&)>;

struct ReplayResult {
    uint64_t records_replayed = 0;
    bool completed = false;
    bool file_error = false;
    bool callback_error = false;
};

class TradeReplayer {
public:
    // 最简顺序回放：从 trades.bin 读出全部 Trade，并逐条回调给上层。
    ReplayResult replay_all(const std::string& data_dir, ReplayTradeCallback callback);
};

}  // namespace mds
