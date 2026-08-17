#include "mds/replayer.h"
#include "storage/binary_trade_writer.h"
#include "storage/file_roll.h"
#include "storage/trade_file_format.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include <gtest/gtest.h>

namespace {

class ReplayerTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("mds_replayer_test_" + std::to_string(::getpid()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    std::filesystem::path trades_path() const {
        const auto files = mds::list_record_files(
            dir_, mds::TradeFileHeader::file_prefix, mds::TradeFileHeader::file_name);
        return files.empty() ? dir_ / "missing.bin" : files.front();
    }

    void write_trades(int count) {
        mds::BinaryTradeWriter writer;
        ASSERT_TRUE(writer.open(dir_.string(), static_cast<size_t>(count) + 8));
        for (int i = 0; i < count; ++i) {
            mds::Trade t{};
            t.exchange_ts_us = 1'700'000'000'000'000LL + i;
            t.recv_ts_us = t.exchange_ts_us;
            t.trade_id = i + 1;
            t.price = 100.0 + i;
            t.quantity = 0.01 * (i + 1);
            t.is_buyer_maker = (i % 2) == 0;
            t.set_symbol("BTCUSDT");
            ASSERT_TRUE(writer.write(t));
        }
        writer.close();
        ASSERT_EQ(writer.records_written(), static_cast<uint64_t>(count));
        ASSERT_EQ(writer.has_error(), false);
    }

    void patch_header_record_count(uint64_t count) {
        std::fstream file(trades_path(), std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(file.is_open());
        file.seekp(static_cast<std::streamoff>(mds::TradeFileHeader::record_count_offset()));
        file.write(reinterpret_cast<const char*>(&count), sizeof(count));
        ASSERT_TRUE(file.good());
    }

    std::filesystem::path dir_;
};

}  // namespace

TEST_F(ReplayerTest, EmptyCallbackReturnsError) {
    write_trades(3);

    mds::Replayer replayer(dir_.string());
    const auto result = replayer.replay_trades(mds::Replayer::TradeCallback{});

    EXPECT_TRUE(result.callback_error);
    EXPECT_FALSE(result.completed);
    EXPECT_FALSE(result.file_error);
    EXPECT_FALSE(result.count_mismatch);
    EXPECT_EQ(result.records_replayed, 0u);
}

TEST_F(ReplayerTest, DetectsHeaderCountMismatch) {
    write_trades(3);
    // 文件实际只有 3 条，把 header 改成 5 → 应检出 count_mismatch
    patch_header_record_count(5);

    mds::Replayer replayer(dir_.string());
    int seen = 0;
    const auto result = replayer.replay_trades([&](const mds::Trade&) { ++seen; });

    EXPECT_EQ(seen, 3);
    EXPECT_EQ(result.records_replayed, 3u);
    EXPECT_EQ(result.header_record_count, 5u);
    EXPECT_TRUE(result.count_mismatch);
    EXPECT_FALSE(result.file_error);
    EXPECT_FALSE(result.callback_error);
    EXPECT_FALSE(result.completed);
}

TEST_F(ReplayerTest, DetectsTruncatedFile) {
    write_trades(3);

    const auto full_size = sizeof(mds::TradeFileHeader) + 3 * sizeof(mds::Trade);
    // 截断到第 3 条的一半，触发 truncated record → file_error
    const auto truncated_size =
        sizeof(mds::TradeFileHeader) + 2 * sizeof(mds::Trade) + sizeof(mds::Trade) / 2;
    ASSERT_LT(truncated_size, full_size);
    std::filesystem::resize_file(trades_path(), truncated_size);

    mds::Replayer replayer(dir_.string());
    int seen = 0;
    const auto result = replayer.replay_trades([&](const mds::Trade&) { ++seen; });

    EXPECT_EQ(seen, 2);
    EXPECT_EQ(result.records_replayed, 2u);
    EXPECT_TRUE(result.file_error);
    EXPECT_FALSE(result.callback_error);
    // file_error 时不额外标 count_mismatch（实现约定）
    EXPECT_FALSE(result.count_mismatch);
    EXPECT_FALSE(result.completed);
}
