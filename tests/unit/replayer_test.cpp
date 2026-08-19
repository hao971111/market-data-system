#include "mds/replayer.h"
#include "storage/binary_trade_writer.h"
#include "storage/file_roll.h"
#include "storage/trade_file_format.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

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
            dir_, mds::TradeFileHeader::file_prefix);
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

    mds::Trade make_trade(int64_t ts_us, int64_t trade_id) const {
        mds::Trade t{};
        t.exchange_ts_us = ts_us;
        t.recv_ts_us = ts_us;
        t.trade_id = trade_id;
        t.price = 100.0;
        t.quantity = 0.01;
        t.is_buyer_maker = false;
        t.set_symbol("BTCUSDT");
        return t;
    }

    void write_trade_list(const std::vector<mds::Trade>& trades) {
        mds::BinaryTradeWriter writer;
        ASSERT_TRUE(writer.open(dir_.string(), trades.size() + 8));
        for (const auto& t : trades) {
            ASSERT_TRUE(writer.write(t));
        }
        writer.close();
        ASSERT_EQ(writer.records_written(), trades.size());
        ASSERT_FALSE(writer.has_error());
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
    const auto truncated_size =
        sizeof(mds::TradeFileHeader) + 2 * sizeof(mds::Trade) + sizeof(mds::Trade) / 2;
    ASSERT_LT(truncated_size, full_size);
    std::filesystem::resize_file(trades_path(), truncated_size);

    mds::Replayer replayer(dir_.string());
    int seen = 0;
    const auto result = replayer.replay_trades([&](const mds::Trade&) { ++seen; });

    EXPECT_EQ(seen, 2);
    EXPECT_EQ(result.records_replayed, 2u);
    EXPECT_FALSE(result.file_error);
    EXPECT_FALSE(result.callback_error);
    // header 仍写着 3，尾巴被当成崩溃残留丢掉
    EXPECT_EQ(result.header_record_count, 3u);
    EXPECT_TRUE(result.count_mismatch);
    EXPECT_FALSE(result.completed);
}

TEST_F(ReplayerTest, CrcMismatchIsFileError) {
    write_trades(3);
    std::fstream file(trades_path(), std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(file.is_open());
    file.seekp(static_cast<std::streamoff>(
        sizeof(mds::TradeFileHeader) + sizeof(mds::Trade) +
        offsetof(mds::Trade, price)));
    const char flip = 0x7F;
    file.write(&flip, 1);
    ASSERT_TRUE(file.good());
    file.close();

    mds::Replayer replayer(dir_.string());
    int seen = 0;
    const auto result = replayer.replay_trades([&](const mds::Trade&) { ++seen; });

    EXPECT_EQ(seen, 1);
    EXPECT_EQ(result.records_replayed, 1u);
    EXPECT_TRUE(result.file_error);
    EXPECT_FALSE(result.count_mismatch);
    EXPECT_FALSE(result.completed);
}

TEST_F(ReplayerTest, TimeWindowFiltersAcrossHours) {
    constexpr int64_t kHourUs = 3'600'000'000LL;
    constexpr int64_t kH0 = 1'000'000;
    constexpr int64_t kH1 = kHourUs + 1'000'000;
    write_trade_list({
        make_trade(kH0, 1),
        make_trade(kH0 + 100, 2),
        make_trade(kH1, 3),
        make_trade(kH1 + 100, 4),
    });

    mds::Replayer replayer(dir_.string());

    std::vector<int64_t> ids;
    auto first_hour = replayer.replay_trades(
        [&](const mds::Trade& t) { ids.push_back(t.trade_id); }, kH0, kH0 + 100);
    EXPECT_TRUE(first_hour.completed);
    EXPECT_FALSE(first_hour.file_error);
    EXPECT_FALSE(first_hour.count_mismatch);
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], 1);
    EXPECT_EQ(ids[1], 2);

    ids.clear();
    auto second_hour = replayer.replay_trades(
        [&](const mds::Trade& t) { ids.push_back(t.trade_id); }, kH1, kH1 + 100);
    EXPECT_TRUE(second_hour.completed);
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], 3);
    EXPECT_EQ(ids[1], 4);

    ids.clear();
    auto both = replayer.replay_trades(
        [&](const mds::Trade& t) { ids.push_back(t.trade_id); }, kH0, kH1 + 100);
    EXPECT_TRUE(both.completed);
    ASSERT_EQ(ids.size(), 4u);

    ids.clear();
    auto full = replayer.replay_trades(
        [&](const mds::Trade& t) { ids.push_back(t.trade_id); });
    EXPECT_TRUE(full.completed);
    EXPECT_EQ(full.records_replayed, 4u);
    EXPECT_EQ(full.header_record_count, 4u);
    ASSERT_EQ(ids.size(), 4u);
}

TEST_F(ReplayerTest, TimeWindowInclusiveEndpointsAndIntraFileSkip) {
    constexpr int64_t kH0 = 1'000'000;
    write_trade_list({
        make_trade(kH0, 1),
        make_trade(kH0 + 50, 2),
        make_trade(kH0 + 100, 3),
    });

    std::vector<int64_t> ids;
    mds::Replayer replayer(dir_.string());
    const auto result = replayer.replay_trades(
        [&](const mds::Trade& t) { ids.push_back(t.trade_id); },
        kH0 + 50, kH0 + 100);

    EXPECT_TRUE(result.completed);
    EXPECT_FALSE(result.count_mismatch);
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], 2);
    EXPECT_EQ(ids[1], 3);
}

TEST_F(ReplayerTest, TimeWindowDoesNotOpenOutOfRangeHourFile) {
    constexpr int64_t kHourUs = 3'600'000'000LL;
    constexpr int64_t kH0 = 1'000'000;
    constexpr int64_t kH1 = kHourUs + 1'000'000;
    write_trade_list({make_trade(kH0, 1)});

    {
        const auto bad = dir_ / mds::hourly_file_name("trades", kH1);
        std::ofstream(bad, std::ios::binary) << "not-a-valid-header";
    }

    mds::Replayer replayer(dir_.string());
    int seen = 0;
    const auto windowed = replayer.replay_trades(
        [&](const mds::Trade&) { ++seen; }, kH0, kH0 + 100);
    EXPECT_EQ(seen, 1);
    EXPECT_TRUE(windowed.completed);
    EXPECT_FALSE(windowed.file_error);

    seen = 0;
    const auto full = replayer.replay_trades([&](const mds::Trade&) { ++seen; });
    EXPECT_TRUE(full.file_error);
    EXPECT_FALSE(full.completed);
}

TEST_F(ReplayerTest, EmptyTimeWindowReturnsZeroRecords) {
    constexpr int64_t kH0 = 1'000'000;
    write_trade_list({make_trade(kH0, 1)});

    mds::Replayer replayer(dir_.string());
    int seen = 0;
    const auto result = replayer.replay_trades(
        [&](const mds::Trade&) { ++seen; },
        7'200'000'000LL, 7'200'000'100LL);

    EXPECT_EQ(seen, 0);
    EXPECT_EQ(result.records_replayed, 0u);
    EXPECT_TRUE(result.completed);
    EXPECT_FALSE(result.file_error);
    EXPECT_FALSE(result.count_mismatch);
}

TEST_F(ReplayerTest, InvalidTimeWindowIsFileError) {
    constexpr int64_t kH0 = 1'000'000;
    write_trade_list({make_trade(kH0, 1)});

    mds::Replayer replayer(dir_.string());
    int seen = 0;
    const auto result = replayer.replay_trades(
        [&](const mds::Trade&) { ++seen; }, kH0 + 100, kH0);

    EXPECT_EQ(seen, 0);
    EXPECT_TRUE(result.file_error);
    EXPECT_FALSE(result.completed);
    EXPECT_FALSE(result.callback_error);
}

TEST_F(ReplayerTest, IgnoresNonHourlyFilesInDirectory) {
    constexpr int64_t kH0 = 1'000'000;
    write_trade_list({make_trade(kH0, 1)});
    std::ofstream(dir_ / "trades.bin", std::ios::binary) << "old-single-file";

    mds::Replayer replayer(dir_.string());
    int seen = 0;
    const auto windowed = replayer.replay_trades(
        [&](const mds::Trade&) { ++seen; }, kH0, kH0 + 100);
    EXPECT_EQ(seen, 1);
    EXPECT_TRUE(windowed.completed);

    seen = 0;
    const auto full = replayer.replay_trades([&](const mds::Trade&) { ++seen; });
    EXPECT_EQ(seen, 1);
    EXPECT_TRUE(full.completed);
    EXPECT_FALSE(full.file_error);
}
