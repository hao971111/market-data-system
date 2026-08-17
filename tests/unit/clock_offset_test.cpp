#include "network/clock_offset.h"

#include <gtest/gtest.h>

using mds::ClockOffsetCalibrator;
using mds::exchange_to_recv_us;

TEST(ClockOffset, AcceptsMidpointSample) {
    ClockOffsetCalibrator cal;
    // send=1000, recv=1010, midpoint=1005；server=2005 → offset=1000，rtt=10
    const auto r = cal.on_sample(1000, 1010, 2005);
    EXPECT_EQ(r.reason, ClockOffsetCalibrator::Reason::Accepted);
    EXPECT_EQ(r.rtt_us, 10);
    EXPECT_EQ(r.sample_offset_us, 1000);
    EXPECT_TRUE(cal.has_offset());
    EXPECT_EQ(cal.offset_us(), 1000);
    EXPECT_EQ(cal.last_rtt_us(), 10);
    EXPECT_EQ(cal.accept_count(), 1u);
    EXPECT_EQ(cal.fail_count(), 0u);
}

TEST(ClockOffset, RejectsRttAbove200msKeepsPrevious) {
    ClockOffsetCalibrator cal;
    ASSERT_EQ(cal.on_sample(0, 10, 1000).reason,
              ClockOffsetCalibrator::Reason::Accepted);
    const int64_t kept = cal.offset_us();

    const auto r = cal.on_sample(0, ClockOffsetCalibrator::kMaxRttUs + 1, 1000);
    EXPECT_EQ(r.reason, ClockOffsetCalibrator::Reason::RttTooLarge);
    EXPECT_EQ(r.rtt_us, ClockOffsetCalibrator::kMaxRttUs + 1);
    EXPECT_TRUE(cal.has_offset());
    EXPECT_EQ(cal.offset_us(), kept);
    EXPECT_EQ(cal.accept_count(), 1u);
    EXPECT_EQ(cal.fail_count(), 1u);
}

TEST(ClockOffset, TransportFailureKeepsPreviousOffset) {
    ClockOffsetCalibrator cal;
    ASSERT_EQ(cal.on_sample(0, 50, 10'000).reason,
              ClockOffsetCalibrator::Reason::Accepted);
    const int64_t kept = cal.offset_us();

    const auto r = cal.on_transport_failure();
    EXPECT_EQ(r.reason, ClockOffsetCalibrator::Reason::TransportFailed);
    EXPECT_TRUE(cal.has_offset());
    EXPECT_EQ(cal.offset_us(), kept);
    EXPECT_EQ(cal.fail_count(), 1u);
}

TEST(ClockOffset, NeverSucceededOffsetIsZero) {
    ClockOffsetCalibrator cal;
    EXPECT_FALSE(cal.has_offset());
    EXPECT_EQ(cal.offset_us(), 0);
    cal.on_transport_failure();
    cal.on_sample(0, ClockOffsetCalibrator::kMaxRttUs + 1, 1);
    EXPECT_FALSE(cal.has_offset());
    EXPECT_EQ(cal.offset_us(), 0);
}

TEST(ClockOffset, RejectsBadServerTimeAndLocalRewindAndHugeOffset) {
    ClockOffsetCalibrator cal;
    EXPECT_EQ(cal.on_sample(0, 10, 0).reason,
              ClockOffsetCalibrator::Reason::BadServerTime);
    EXPECT_EQ(cal.on_sample(0, 10, -1).reason,
              ClockOffsetCalibrator::Reason::BadServerTime);
    EXPECT_EQ(cal.on_sample(20, 10, 1000).reason,
              ClockOffsetCalibrator::Reason::BadLocalTimes);

    const int64_t huge = ClockOffsetCalibrator::kMaxAbsOffsetUs + 1;
    // midpoint = 5，server = 5 + huge
    EXPECT_EQ(cal.on_sample(0, 10, 5 + huge).reason,
              ClockOffsetCalibrator::Reason::OffsetTooLarge);
    EXPECT_FALSE(cal.has_offset());
}

TEST(ClockOffset, RttEqualToMaxIsAccepted) {
    ClockOffsetCalibrator cal;
    const auto r = cal.on_sample(0, ClockOffsetCalibrator::kMaxRttUs, 1000);
    EXPECT_EQ(r.reason, ClockOffsetCalibrator::Reason::Accepted);
    EXPECT_TRUE(cal.has_offset());
}

TEST(ClockOffset, ExchangeToRecvAppliesOffsetAndDropsNegative) {
    EXPECT_FALSE(exchange_to_recv_us(0, 100, 0).has_value());
    EXPECT_FALSE(exchange_to_recv_us(200, 100, 0).has_value());
    EXPECT_EQ(exchange_to_recv_us(200, 100, 150).value(), 50u);
    EXPECT_EQ(exchange_to_recv_us(100, 150, 0).value(), 50u);
}
