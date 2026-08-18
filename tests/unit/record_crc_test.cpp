#include "storage/record_integrity.h"

#include <mds/types.h>

#include <cstring>
#include <string>

#include <gtest/gtest.h>

TEST(RecordCrc, IeeeKnownVector) {
    const char* s = "123456789";
    EXPECT_EQ(mds::crc32_ieee(s, std::strlen(s)), 0xCBF43926u);
}

TEST(RecordCrc, TradeStampRoundTripAndByteFlip) {
    mds::Trade t{};
    t.trade_id = 42;
    t.price = 1.25;
    t.set_symbol("BTCUSDT");
    mds::stamp_record_crc(t);
    EXPECT_TRUE(mds::record_crc_ok(t));
    EXPECT_EQ(t.crc32, mds::compute_record_crc(t));

    t.trade_id = 43;
    EXPECT_FALSE(mds::record_crc_ok(t));
}

TEST(RecordCrc, ZeroCrcTreatedAsLegacyOk) {
    mds::Trade t{};
    t.trade_id = 7;
    EXPECT_EQ(t.crc32, 0u);
    EXPECT_TRUE(mds::record_crc_ok(t));
}
