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

TEST(FileRoll, ListSortsHourlyAndIgnoresOtherNames) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("mds_file_roll_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    std::ofstream(dir / "noise.txt") << "x";
    std::ofstream(dir / "trades.bin") << "old-single-file";
    std::ofstream(dir / "trades_20260728_15.bin") << "b";
    std::ofstream(dir / "trades_20260728_14.bin") << "a";

    const auto files = mds::list_record_files(dir, mds::TradeFileHeader::file_prefix);
    ASSERT_EQ(files.size(), 2u);
    EXPECT_EQ(files[0].filename(), "trades_20260728_14.bin");
    EXPECT_EQ(files[1].filename(), "trades_20260728_15.bin");

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(FileRoll, HourlyFileInWindowIsClosedIntervalOnFileName) {
    const char* p = "trades";
    mds::TimeWindow both{0, 3'599'999'999LL};
    EXPECT_TRUE(mds::hourly_file_in_window(p, "trades_19700101_00.bin", both));
    EXPECT_FALSE(mds::hourly_file_in_window(p, "trades_19700101_01.bin", both));
    EXPECT_FALSE(mds::hourly_file_in_window(p, "trades.bin", both));

    mds::TimeWindow from_only{3'600'000'000LL, std::nullopt};
    EXPECT_FALSE(mds::hourly_file_in_window(p, "trades_19700101_00.bin", from_only));
    EXPECT_TRUE(mds::hourly_file_in_window(p, "trades_19700101_01.bin", from_only));
    EXPECT_TRUE(mds::hourly_file_in_window(p, "trades_19700101_02.bin", from_only));

    mds::TimeWindow to_only{std::nullopt, 3'600'000'000LL};
    EXPECT_TRUE(mds::hourly_file_in_window(p, "trades_19700101_00.bin", to_only));
    EXPECT_TRUE(mds::hourly_file_in_window(p, "trades_19700101_01.bin", to_only));
    EXPECT_FALSE(mds::hourly_file_in_window(p, "trades_19700101_02.bin", to_only));
}

TEST(FileRoll, ListWindowSelectsIntersectingHours) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("mds_file_roll_win_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    std::ofstream(dir / "trades.bin") << "old-single-file";
    std::ofstream(dir / "trades_19700101_00.bin") << "h0";
    std::ofstream(dir / "trades_19700101_01.bin") << "h1";
    std::ofstream(dir / "trades_19700101_02.bin") << "h2";

    const mds::TimeWindow window{1'000'000LL, 3'600'000'000LL + 1'000'000LL};
    const auto files = mds::list_record_files(
        dir, mds::TradeFileHeader::file_prefix, window);
    ASSERT_EQ(files.size(), 2u);
    EXPECT_EQ(files[0].filename(), "trades_19700101_00.bin");
    EXPECT_EQ(files[1].filename(), "trades_19700101_01.bin");

    const auto all = mds::list_record_files(dir, mds::TradeFileHeader::file_prefix);
    ASSERT_EQ(all.size(), 3u);
    EXPECT_EQ(all[0].filename(), "trades_19700101_00.bin");
    EXPECT_EQ(all[2].filename(), "trades_19700101_02.bin");

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
