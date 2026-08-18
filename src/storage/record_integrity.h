#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <type_traits>

namespace mds {

// IEEE CRC-32（poly 0xEDB88320），与 zlib/PNG 相同。
// 落盘字节序：little-endian（本仓库只在 LE 上构建）。
inline uint32_t crc32_ieee(const void* data, std::size_t len) {
    auto p = static_cast<const unsigned char*>(data);
    uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b) {
            const uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

// CRC 覆盖整条记录，计算时把 crc32 字段视为 0。
template <typename Record>
uint32_t compute_record_crc(const Record& record) {
    static_assert(std::is_trivially_copyable<Record>::value,
                  "compute_record_crc requires trivially copyable Record");
    Record tmp = record;
    tmp.crc32 = 0;
    return crc32_ieee(&tmp, sizeof(tmp));
}

template <typename Record>
void stamp_record_crc(Record& record) {
    record.crc32 = compute_record_crc(record);
}

// crc32==0：Step 13–15 未盖章的旧记录，不验（避免重启把当小时文件全部截掉）。
template <typename Record>
bool record_crc_ok(const Record& record) {
    if (record.crc32 == 0) {
        return true;
    }
    return record.crc32 == compute_record_crc(record);
}

struct FileRepairResult {
    bool ok = false;
    uint64_t valid_records = 0;
    uint64_t bytes_discarded = 0;
};

// 把文件截到最后一条完整且 CRC 合格的记录，并回填 header.record_count。
// header 非法时失败、不改文件。
template <typename Record, typename Header>
FileRepairResult repair_record_file(const std::filesystem::path& path) {
    FileRepairResult out;
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file.is_open()) {
        std::cerr << "[ERROR] Failed to open for repair: " << path << std::endl;
        return out;
    }

    Header header{};
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!file.good()) {
        std::cerr << "[ERROR] Invalid binary file header: " << path << std::endl;
        return out;
    }
    const Header expected;
    if (std::memcmp(header.magic, expected.magic, sizeof(expected.magic)) != 0 ||
        header.version != expected.version ||
        header.record_size != sizeof(Record)) {
        std::cerr << "[ERROR] File header mismatch, refusing to repair: " << path
                  << std::endl;
        return out;
    }

    std::error_code ec;
    const auto file_size = std::filesystem::file_size(path, ec);
    if (ec || file_size < sizeof(Header)) {
        std::cerr << "[ERROR] Cannot stat file for repair: " << path << std::endl;
        return out;
    }

    uint64_t valid = 0;
    std::uint64_t valid_end = sizeof(Header);
    while (valid_end + sizeof(Record) <= file_size) {
        Record rec{};
        file.read(reinterpret_cast<char*>(&rec), sizeof(rec));
        if (file.gcount() != static_cast<std::streamsize>(sizeof(rec))) {
            break;
        }
        if (!record_crc_ok(rec)) {
            break;
        }
        ++valid;
        valid_end += sizeof(Record);
    }

    const auto discarded = file_size - valid_end;
    if (discarded > 0) {
        file.close();
        std::filesystem::resize_file(path, valid_end, ec);
        if (ec) {
            std::cerr << "[ERROR] Failed to truncate damaged tail: " << path
                      << " (" << ec.message() << ")" << std::endl;
            return out;
        }
        file.open(path, std::ios::binary | std::ios::in | std::ios::out);
        if (!file.is_open()) {
            std::cerr << "[ERROR] Failed to reopen after truncate: " << path << std::endl;
            return out;
        }
        std::cerr << "[WARN] Truncated " << discarded << " trailing bytes in " << path
                  << " (kept " << valid << " records)" << std::endl;
    }

    file.clear();
    file.seekp(static_cast<std::streamoff>(Header::record_count_offset()));
    file.write(reinterpret_cast<const char*>(&valid), sizeof(valid));
    file.flush();
    if (file.fail()) {
        std::cerr << "[ERROR] Failed to write record_count during repair: " << path
                  << std::endl;
        return out;
    }

    out.ok = true;
    out.valid_records = valid;
    out.bytes_discarded = discarded;
    return out;
}

}  // namespace mds
