#include "storage/binary_order_book_reader.h"
#include "storage/binary_order_book_writer.h"
#include "storage/binary_trade_reader.h"
#include "storage/binary_trade_writer.h"

#include <cstring>
#include <filesystem>
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

mds::Trade make_trade(int i) {
    mds::Trade t{};
    t.timestamp_us = 1'700'000'000'000'000LL + i;
    t.trade_id = 10'000 + i;
    t.price = 100.0 + i * 0.01;
    t.quantity = 0.001 + i * 0.0001;
    t.is_buyer_maker = (i % 2) == 0;
    t.set_symbol(i % 2 == 0 ? "BTCUSDT" : "ETHUSDT");
    return t;
}

mds::OrderBookSnapshot make_book(int i) {
    mds::OrderBookSnapshot book{};
    book.timestamp_us = 1'700'000'000'000'000LL + i;
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
        EXPECT_EQ(std::memcmp(&got, &original[i], sizeof(mds::Trade)), 0)
            << "mismatch at record " << i;
        EXPECT_EQ(got.timestamp_us, original[i].timestamp_us);
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
        EXPECT_EQ(std::memcmp(&got, &original[i], sizeof(mds::OrderBookSnapshot)), 0)
            << "mismatch at record " << i;
        EXPECT_EQ(got.timestamp_us, original[i].timestamp_us);
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
