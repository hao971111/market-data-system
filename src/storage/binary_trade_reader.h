#pragma once

#include "../common/types.h"
#include "trade_file_format.h"
#include <fstream>
#include <string>

namespace mds {

class BinaryTradeReader {
public:
    bool open(const std::string& data_dir);
    bool read_next(Trade& trade);
    void close();
    bool has_error() const { return read_error_; }

private:
    bool validate_header(const TradeFileHeader& header) const;

    std::ifstream file_;
    bool read_error_ = false;
};

}  // namespace mds
