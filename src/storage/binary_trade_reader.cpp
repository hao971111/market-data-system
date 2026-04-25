#include "binary_trade_reader.h"
#include <cstring>
#include <filesystem>
#include <iostream>

namespace mds {

bool BinaryTradeReader::open(const std::string& data_dir) {
    close();
    read_error_ = false;

    const auto path = std::filesystem::path(data_dir) / "trades.bin";
    file_.open(path, std::ios::binary);
    if (!file_.is_open()) {
        std::cerr << "[ERROR] Failed to open trade file for reading: " << path << std::endl;
        return false;
    }

    TradeFileHeader header;
    file_.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!file_.good() || !validate_header(header)) {
        std::cerr << "[ERROR] Invalid trade file header: " << path << std::endl;
        close();
        return false;
    }

    return true;
}

bool BinaryTradeReader::read_next(Trade& trade) {
    if (!file_.is_open()) {
        return false;
    }

    file_.read(reinterpret_cast<char*>(&trade), sizeof(trade));
    const auto bytes_read = file_.gcount();
    if (bytes_read == static_cast<std::streamsize>(sizeof(trade))) {
        return true;
    }

    if (bytes_read > 0) {
        read_error_ = true;
        std::cerr << "[ERROR] Truncated trade record: read "
                  << bytes_read << " bytes, expected "
                  << sizeof(trade) << std::endl;
    }

    return false;
}

void BinaryTradeReader::close() {
    if (file_.is_open()) {
        file_.close();
    }
}

bool BinaryTradeReader::validate_header(const TradeFileHeader& header) const {
    const TradeFileHeader expected;
    return std::memcmp(header.magic, expected.magic, sizeof(expected.magic)) == 0
        && header.version == expected.version
        && header.record_size == sizeof(Trade);
}

}  // namespace mds
