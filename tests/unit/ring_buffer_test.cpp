#include "cache/ring_buffer.h"

#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

TEST(RingBuffer, RejectsZeroCapacity) {
    EXPECT_THROW(mds::RingBuffer<int>(0), std::invalid_argument);
}

TEST(RingBuffer, OverwritesOldestWhenFull) {
    mds::RingBuffer<int> rb(3);

    rb.push(1);
    rb.push(2);
    rb.push(3);
    EXPECT_EQ(rb.size(), 3u);
    EXPECT_EQ(rb.snapshot(10), (std::vector<int>{1, 2, 3}));

    rb.push(4);  // 覆盖最旧的 1
    EXPECT_EQ(rb.size(), 3u);
    EXPECT_EQ(rb.snapshot(10), (std::vector<int>{2, 3, 4}));

    rb.push(5);  // 覆盖 2
    EXPECT_EQ(rb.snapshot(10), (std::vector<int>{3, 4, 5}));
}

TEST(RingBuffer, SnapshotReturnsOldestToNewest) {
    mds::RingBuffer<int> rb(5);
    for (int i = 1; i <= 5; ++i) {
        rb.push(i);
    }

    // 全量：旧 -> 新
    EXPECT_EQ(rb.snapshot(5), (std::vector<int>{1, 2, 3, 4, 5}));
    // 只要最近 3 条，仍是旧 -> 新
    EXPECT_EQ(rb.snapshot(3), (std::vector<int>{3, 4, 5}));
    // 超过 size 时按实际条数返回
    EXPECT_EQ(rb.snapshot(100), (std::vector<int>{1, 2, 3, 4, 5}));
}

TEST(RingBuffer, SnapshotOnPartialFill) {
    mds::RingBuffer<int> rb(5);
    rb.push(10);
    rb.push(20);

    EXPECT_EQ(rb.size(), 2u);
    EXPECT_EQ(rb.snapshot(5), (std::vector<int>{10, 20}));
    EXPECT_EQ(rb.snapshot(1), (std::vector<int>{20}));
}
