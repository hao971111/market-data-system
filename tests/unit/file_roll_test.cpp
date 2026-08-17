#include "storage/file_roll.h"
#include "storage/trade_file_format.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include <gtest/gtest.h>

TEST(FileRoll, HourlyNameUsesUtcHour) {
    EXPECT_EQ(mds::hourly_file_name("trades", 0), "trades_19700101_00.bin");
    EXPECT_EQ(mds::hourly_file_name("trades", 3'599'999'999LL),
              "trades_19700101_00.bin");
    EXPECT_EQ(mds::hourly_file_name("trades", 3'600'000'000LL),
              "trades_19700101_01.bin");
    EXPECT_EQ(mds::hourly_file_name("orderbooks", 3'600'000'000LL),
              "orderbooks_19700101_01.bin");
}

TEST(FileRoll, IsHourlyFileName) {
    EXPECT_TRUE(mds::is_hourly_file_name("trades", "trades_20260728_14.bin"));
    EXPECT_FALSE(mds::is_hourly_file_name("trades", "trades.bin"));
    EXPECT_FALSE(mds::is_hourly_file_name("trades", "orderbooks_20260728_14.bin"));
    EXPECT_FALSE(mds::is_hourly_file_name("trades", "trades_20260728_1.bin"));
    EXPECT_FALSE(mds::is_hourly_file_name("trades", "trades_20260728_14.bin.bak"));
}

TEST(FileRoll, ListSortsLegacyThenHourly) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("mds_file_roll_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    std::ofstream(dir / "noise.txt") << "x";
    std::ofstream(dir / "trades.bin") << "legacy";
    std::ofstream(dir / "trades_20260728_15.bin") << "b";
    std::ofstream(dir / "trades_20260728_14.bin") << "a";

    const auto files = mds::list_record_files(dir, mds::TradeFileHeader::file_prefix,
                                              mds::TradeFileHeader::file_name);
    ASSERT_EQ(files.size(), 3u);
    EXPECT_EQ(files[0].filename(), "trades.bin");
    EXPECT_EQ(files[1].filename(), "trades_20260728_14.bin");
    EXPECT_EQ(files[2].filename(), "trades_20260728_15.bin");

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
