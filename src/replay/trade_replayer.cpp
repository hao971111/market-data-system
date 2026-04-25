#include "trade_replayer.h"
#include "../storage/binary_trade_reader.h"
#include <exception>
#include <iostream>

namespace mds {

ReplayResult TradeReplayer::replay_all(const std::string& data_dir,
                                       ReplayTradeCallback callback) {
    ReplayResult result;
    if (!callback) {
        std::cerr << "[ERROR] Replay callback is empty" << std::endl;
        result.callback_error = true;
        return result;
    }

    BinaryTradeReader reader;
    if (!reader.open(data_dir)) {
        result.file_error = true;
        return result;
    }

    Trade trade;
    while (reader.read_next(trade)) {
        try {
            callback(trade);
            ++result.records_replayed;
        } catch (const std::exception& e) {
            result.callback_error = true;
            std::cerr << "[ERROR] Replay callback failed: " << e.what() << std::endl;
            break;
        } catch (...) {
            result.callback_error = true;
            std::cerr << "[ERROR] Replay callback failed with unknown exception" << std::endl;
            break;
        }
    }

    if (reader.has_error()) {
        result.file_error = true;
        std::cerr << "[ERROR] Replay stopped because trade file is corrupted" << std::endl;
    }

    reader.close();
    result.completed = !result.file_error && !result.callback_error;
    return result;
}

}  // namespace mds
