#include "storage/binary_order_book_reader.h"
#include "storage/binary_order_book_writer.h"
#include "storage/binary_trade_reader.h"
#include "storage/binary_trade_writer.h"
#include "storage/file_roll.h"
#include "storage/record_integrity.h"
#include "storage/trade_file_format.h"

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>

namespace {

class StorageRoundTripTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("mds_storage_rt_" + std::to_string(::getpid()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    std::filesystem::path dir_;
};

std::filesystem::path only_trade_file(const std::filesystem::path& dir) {
    const auto files = mds::list_record_files(dir, mds::TradeFileHeader::file_prefix,
                                              mds::TradeFileHeader::file_name);
    return files.size() == 1 ? files.front() : std::filesystem::path{};
}

template <typename T>
bool payload_equal(const T& a, const T& b) {
    T x = a;
    T y = b;
    x.crc32 = 0;
    y.crc32 = 0;
    return std::memcmp(&x, &y, sizeof(T)) == 0;
}

mds::Trade make_trade(int i) {
    mds::Trade t{};
    t.exchange_ts_us = 1'700'000'000'000'000LL + i;
    t.recv_ts_us = t.exchange_ts_us + 100;
    t.trade_id = 10'000 + i;
    t.price = 100.0 + i * 0.01;
    t.quantity = 0.001 + i * 0.0001;
    t.is_buyer_maker = (i % 2) == 0;
    t.set_symbol(i % 2 == 0 ? "BTCUSDT" : "ETHUSDT");
    return t;
}

mds::OrderBookSnapshot make_book(int i) {
    mds::OrderBookSnapshot book{};
    book.exchange_ts_us = 0;
    book.recv_ts_us = 1'700'000'000'000'000LL + i;
    book.last_update_id = 1000 + i;
    book.set_symbol(i % 2 == 0 ? "BTCUSDT" : "ETHUSDT");
    for (int level = 0; level < mds::ORDERBOOK_DEPTH; ++level) {
        book.bids[level].price = 100.0 - level - i * 0.001;
        book.bids[level].quantity = 1.0 + level + i * 0.01;
        book.asks[level].price = 101.0 + level + i * 0.001;
        book.asks[level].quantity = 2.0 + level + i * 0.01;
    }
    return book;
}

}  // namespace

TEST_F(StorageRoundTripTest, TradeWriteRead1000RecordsFieldExact) {
    constexpr int kCount = 1000;
    std::vector<mds::Trade> original;
    original.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        original.push_back(make_trade(i));
    }

    {
        mds::BinaryTradeWriter writer;
        ASSERT_TRUE(writer.open(dir_.string(), /*queue_capacity=*/kCount + 16));
        for (const auto& t : original) {
            ASSERT_TRUE(writer.write(t));
        }
        writer.close();
        EXPECT_EQ(writer.records_written(), static_cast<uint64_t>(kCount));
        EXPECT_EQ(writer.records_dropped(), 0u);
        EXPECT_EQ(writer.has_error(), false);
    }

    mds::BinaryTradeReader reader;
    ASSERT_TRUE(reader.open(dir_.string()));
    EXPECT_EQ(reader.get_record_count(), static_cast<uint64_t>(kCount));

    for (int i = 0; i < kCount; ++i) {
        mds::Trade got{};
        ASSERT_TRUE(reader.read_next(got)) << "failed at record " << i;
        EXPECT_TRUE(payload_equal(got, original[i])) << "mismatch at record " << i;
        EXPECT_EQ(got.crc32, mds::compute_record_crc(got));
        EXPECT_EQ(got.exchange_ts_us, original[i].exchange_ts_us);
        EXPECT_EQ(got.recv_ts_us, original[i].recv_ts_us);
        EXPECT_EQ(got.trade_id, original[i].trade_id);
        EXPECT_DOUBLE_EQ(got.price, original[i].price);
        EXPECT_DOUBLE_EQ(got.quantity, original[i].quantity);
        EXPECT_STREQ(got.symbol, original[i].symbol);
        EXPECT_EQ(got.is_buyer_maker, original[i].is_buyer_maker);
    }

    mds::Trade extra{};
    EXPECT_FALSE(reader.read_next(extra));
    EXPECT_FALSE(reader.has_error());
}

TEST_F(StorageRoundTripTest, OrderBookWriteRead1000RecordsFieldExact) {
    constexpr int kCount = 1000;
    std::vector<mds::OrderBookSnapshot> original;
    original.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        original.push_back(make_book(i));
    }

    {
        mds::BinaryOrderBookWriter writer;
        ASSERT_TRUE(writer.open(dir_.string(), /*queue_capacity=*/kCount + 16));
        for (const auto& book : original) {
            ASSERT_TRUE(writer.write(book));
        }
        writer.close();
        EXPECT_EQ(writer.records_written(), static_cast<uint64_t>(kCount));
        EXPECT_EQ(writer.records_dropped(), 0u);
        EXPECT_EQ(writer.has_error(), false);
    }

    mds::BinaryOrderBookReader reader;
    ASSERT_TRUE(reader.open(dir_.string()));
    EXPECT_EQ(reader.get_record_count(), static_cast<uint64_t>(kCount));

    for (int i = 0; i < kCount; ++i) {
        mds::OrderBookSnapshot got{};
        ASSERT_TRUE(reader.read_next(got)) << "failed at record " << i;
        EXPECT_TRUE(payload_equal(got, original[i])) << "mismatch at record " << i;
        EXPECT_EQ(got.crc32, mds::compute_record_crc(got));
        EXPECT_EQ(got.recv_ts_us, original[i].recv_ts_us);
        EXPECT_STREQ(got.symbol, original[i].symbol);
        for (int level = 0; level < mds::ORDERBOOK_DEPTH; ++level) {
            EXPECT_DOUBLE_EQ(got.bids[level].price, original[i].bids[level].price);
            EXPECT_DOUBLE_EQ(got.bids[level].quantity, original[i].bids[level].quantity);
            EXPECT_DOUBLE_EQ(got.asks[level].price, original[i].asks[level].price);
            EXPECT_DOUBLE_EQ(got.asks[level].quantity, original[i].asks[level].quantity);
        }
    }

    mds::OrderBookSnapshot extra{};
    EXPECT_FALSE(reader.read_next(extra));
    EXPECT_FALSE(reader.has_error());
}

TEST_F(StorageRoundTripTest, ReaderRejectsVersion1Header) {
    {
        mds::BinaryTradeWriter writer;
        ASSERT_TRUE(writer.open(dir_.string(), 16));
        ASSERT_TRUE(writer.write(make_trade(0)));
        writer.close();
    }

    {
        ASSERT_FALSE(only_trade_file(dir_).empty());
        std::fstream f(only_trade_file(dir_),
                       std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(f.is_open());
        f.seekp(static_cast<std::streamoff>(offsetof(mds::TradeFileHeader, version)));
        const uint32_t v1 = 1;
        f.write(reinterpret_cast<const char*>(&v1), sizeof(v1));
        ASSERT_TRUE(f.good());
    }

    mds::BinaryTradeReader reader;
    EXPECT_FALSE(reader.open(dir_.string()));
}

TEST_F(StorageRoundTripTest, SameHourRestartAppendsWithoutTrunc) {
    auto a = make_trade(0);
    auto b = make_trade(1);
    {
        mds::BinaryTradeWriter writer;
        ASSERT_TRUE(writer.open(dir_.string(), 16));
        ASSERT_TRUE(writer.write(a));
        writer.close();
        EXPECT_EQ(writer.records_written(), 1u);
    }
    {
        mds::BinaryTradeWriter writer;
        ASSERT_TRUE(writer.open(dir_.string(), 16));
        ASSERT_TRUE(writer.write(b));
        writer.close();
        EXPECT_EQ(writer.records_written(), 1u);
    }

    const auto files = mds::list_record_files(
        dir_, mds::TradeFileHeader::file_prefix, mds::TradeFileHeader::file_name);
    ASSERT_EQ(files.size(), 1u);

    mds::BinaryTradeReader reader;
    ASSERT_TRUE(reader.open(dir_.string()));
    EXPECT_EQ(reader.get_record_count(), 2u);
    mds::Trade got{};
    ASSERT_TRUE(reader.read_next(got));
    EXPECT_EQ(got.trade_id, a.trade_id);
    ASSERT_TRUE(reader.read_next(got));
    EXPECT_EQ(got.trade_id, b.trade_id);
    EXPECT_FALSE(reader.read_next(got));
}

TEST_F(StorageRoundTripTest, HourBoundaryCreatesTwoFilesAndReaderConcatenates) {
    constexpr int64_t kHourUs = 3'600'000'000LL;
    auto first = make_trade(0);
    auto second = make_trade(1);
    second.recv_ts_us = first.recv_ts_us + kHourUs;
    second.exchange_ts_us = first.exchange_ts_us + kHourUs;

    {
        mds::BinaryTradeWriter writer;
        ASSERT_TRUE(writer.open(dir_.string(), 16));
        ASSERT_TRUE(writer.write(first));
        ASSERT_TRUE(writer.write(second));
        writer.close();
        EXPECT_EQ(writer.records_written(), 2u);
        EXPECT_FALSE(writer.has_error());
    }

    const auto files = mds::list_record_files(
        dir_, mds::TradeFileHeader::file_prefix, mds::TradeFileHeader::file_name);
    ASSERT_EQ(files.size(), 2u);
    EXPECT_EQ(files[0].filename().string(),
              mds::hourly_file_name("trades", first.recv_ts_us));
    EXPECT_EQ(files[1].filename().string(),
              mds::hourly_file_name("trades", second.recv_ts_us));

    mds::BinaryTradeReader reader;
    ASSERT_TRUE(reader.open(dir_.string()));
    EXPECT_EQ(reader.get_record_count(), 2u);

    mds::Trade got{};
    ASSERT_TRUE(reader.read_next(got));
    EXPECT_EQ(got.trade_id, first.trade_id);
    EXPECT_EQ(got.recv_ts_us, first.recv_ts_us);
    ASSERT_TRUE(reader.read_next(got));
    EXPECT_EQ(got.trade_id, second.trade_id);
    EXPECT_EQ(got.recv_ts_us, second.recv_ts_us);
    EXPECT_FALSE(reader.read_next(got));
    EXPECT_FALSE(reader.has_error());
}

TEST_F(StorageRoundTripTest, RestartKeepsPreviousHourFile) {
    constexpr int64_t kHourUs = 3'600'000'000LL;
    auto hour_a = make_trade(0);
    auto hour_b = make_trade(1);
    hour_b.recv_ts_us = hour_a.recv_ts_us + kHourUs;

    {
        mds::BinaryTradeWriter writer;
        ASSERT_TRUE(writer.open(dir_.string(), 16));
        ASSERT_TRUE(writer.write(hour_a));
        writer.close();
    }
    {
        mds::BinaryTradeWriter writer;
        ASSERT_TRUE(writer.open(dir_.string(), 16));
        ASSERT_TRUE(writer.write(hour_b));
        writer.close();
    }

    const auto files = mds::list_record_files(
        dir_, mds::TradeFileHeader::file_prefix, mds::TradeFileHeader::file_name);
    ASSERT_EQ(files.size(), 2u);

    mds::BinaryTradeReader reader;
    ASSERT_TRUE(reader.open(dir_.string()));
    EXPECT_EQ(reader.get_record_count(), 2u);
    mds::Trade got{};
    ASSERT_TRUE(reader.read_next(got));
    EXPECT_EQ(got.trade_id, hour_a.trade_id);
    ASSERT_TRUE(reader.read_next(got));
    EXPECT_EQ(got.trade_id, hour_b.trade_id);
}

TEST_F(StorageRoundTripTest, ReaderIgnoresTruncatedTail) {
    {
        mds::BinaryTradeWriter writer;
        ASSERT_TRUE(writer.open(dir_.string(), 16));
        ASSERT_TRUE(writer.write(make_trade(0)));
        ASSERT_TRUE(writer.write(make_trade(1)));
        ASSERT_TRUE(writer.write(make_trade(2)));
        writer.close();
    }

    const auto path = only_trade_file(dir_);
    ASSERT_FALSE(path.empty());
    const auto full = sizeof(mds::TradeFileHeader) + 3 * sizeof(mds::Trade);
    const auto truncated =
        sizeof(mds::TradeFileHeader) + 2 * sizeof(mds::Trade) + sizeof(mds::Trade) / 2;
    ASSERT_LT(truncated, full);
    std::filesystem::resize_file(path, truncated);

    mds::BinaryTradeReader reader;
    ASSERT_TRUE(reader.open(dir_.string()));
    mds::Trade got{};
    ASSERT_TRUE(reader.read_next(got));
    EXPECT_EQ(got.trade_id, make_trade(0).trade_id);
    ASSERT_TRUE(reader.read_next(got));
    EXPECT_EQ(got.trade_id, make_trade(1).trade_id);
    EXPECT_FALSE(reader.read_next(got));
    EXPECT_FALSE(reader.has_error());
}

TEST_F(StorageRoundTripTest, ReaderStopsOnMiddleCrcMismatch) {
    {
        mds::BinaryTradeWriter writer;
        ASSERT_TRUE(writer.open(dir_.string(), 16));
        ASSERT_TRUE(writer.write(make_trade(0)));
        ASSERT_TRUE(writer.write(make_trade(1)));
        ASSERT_TRUE(writer.write(make_trade(2)));
        writer.close();
    }

    const auto path = only_trade_file(dir_);
    ASSERT_FALSE(path.empty());
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(f.is_open());
    const auto off = static_cast<std::streamoff>(
        sizeof(mds::TradeFileHeader) + sizeof(mds::Trade) +
        offsetof(mds::Trade, price));
    f.seekp(off);
    const char flip = 0x7F;
    f.write(&flip, 1);
    ASSERT_TRUE(f.good());
    f.close();

    mds::BinaryTradeReader reader;
    ASSERT_TRUE(reader.open(dir_.string()));
    mds::Trade got{};
    ASSERT_TRUE(reader.read_next(got));
    EXPECT_EQ(got.trade_id, make_trade(0).trade_id);
    EXPECT_FALSE(reader.read_next(got));
    EXPECT_TRUE(reader.has_error());
}

TEST_F(StorageRoundTripTest, WriterRepairsTruncatedTailThenAppends) {
    {
        mds::BinaryTradeWriter writer;
        ASSERT_TRUE(writer.open(dir_.string(), 16));
        ASSERT_TRUE(writer.write(make_trade(0)));
        ASSERT_TRUE(writer.write(make_trade(1)));
        ASSERT_TRUE(writer.write(make_trade(2)));
        writer.close();
    }

    const auto path = only_trade_file(dir_);
    ASSERT_FALSE(path.empty());
    const auto truncated =
        sizeof(mds::TradeFileHeader) + 2 * sizeof(mds::Trade) + sizeof(mds::Trade) / 2;
    std::filesystem::resize_file(path, truncated);

    {
        mds::BinaryTradeWriter writer;
        ASSERT_TRUE(writer.open(dir_.string(), 16));
        ASSERT_TRUE(writer.write(make_trade(3)));
        writer.close();
        EXPECT_GT(writer.tail_bytes_discarded(), 0u);
        EXPECT_FALSE(writer.has_error());
    }

    mds::BinaryTradeReader reader;
    ASSERT_TRUE(reader.open(dir_.string()));
    EXPECT_EQ(reader.get_record_count(), 3u);
    mds::Trade got{};
    ASSERT_TRUE(reader.read_next(got));
    EXPECT_EQ(got.trade_id, make_trade(0).trade_id);
    ASSERT_TRUE(reader.read_next(got));
    EXPECT_EQ(got.trade_id, make_trade(1).trade_id);
    ASSERT_TRUE(reader.read_next(got));
    EXPECT_EQ(got.trade_id, make_trade(3).trade_id);
    EXPECT_FALSE(reader.read_next(got));
    EXPECT_FALSE(reader.has_error());
}
