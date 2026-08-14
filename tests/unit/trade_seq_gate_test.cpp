#include "network/trade_seq_gate.h"

#include <gtest/gtest.h>

using mds::TradeSeqGate;

TEST(TradeSeqGate, ContinuousSequenceAllAccepted) {
    TradeSeqGate gate;

    for (int64_t id = 100; id <= 110; ++id) {
        const auto r = gate.on_live_trade(id);
        EXPECT_EQ(r.action, TradeSeqGate::Action::Accept) << "id=" << id;
        EXPECT_TRUE(gate.can_trade());
        EXPECT_EQ(gate.last_id(), id);
    }
    EXPECT_EQ(gate.state(), TradeSeqGate::State::Live);
}

TEST(TradeSeqGate, DuplicateOrOutOfOrderRejected) {
    TradeSeqGate gate;
    ASSERT_EQ(gate.on_live_trade(10).action, TradeSeqGate::Action::Accept);

    // 重复
    auto r = gate.on_live_trade(10);
    EXPECT_EQ(r.action, TradeSeqGate::Action::Duplicate);
    EXPECT_EQ(gate.last_id(), 10);
    EXPECT_TRUE(gate.can_trade());

    // 乱序/回退
    r = gate.on_live_trade(9);
    EXPECT_EQ(r.action, TradeSeqGate::Action::Duplicate);
    EXPECT_EQ(gate.last_id(), 10);
    EXPECT_TRUE(gate.can_trade());

    // 仍可继续正常步进
    r = gate.on_live_trade(11);
    EXPECT_EQ(r.action, TradeSeqGate::Action::Accept);
    EXPECT_EQ(gate.last_id(), 11);
}

TEST(TradeSeqGate, GapEntersRecoveringAndHoldsLive) {
    TradeSeqGate gate;
    ASSERT_EQ(gate.on_live_trade(1).action, TradeSeqGate::Action::Accept);
    ASSERT_EQ(gate.on_live_trade(2).action, TradeSeqGate::Action::Accept);

    // 跳到 5：缺失 [3,4]
    const auto gap = gate.on_live_trade(5);
    EXPECT_EQ(gap.action, TradeSeqGate::Action::Gap);
    EXPECT_EQ(gap.missing_begin, 3);
    EXPECT_EQ(gap.missing_end, 4);
    EXPECT_EQ(gap.missing_count(), 2u);
    EXPECT_EQ(gate.state(), TradeSeqGate::State::Recovering);
    EXPECT_FALSE(gate.can_trade());
    EXPECT_EQ(gate.last_id(), 2);  // 锚点仍在 gap 前

    // 恢复完成前，实时包一律 Hold
    const auto hold = gate.on_live_trade(6);
    EXPECT_EQ(hold.action, TradeSeqGate::Action::Hold);
    EXPECT_FALSE(gate.can_trade());
    EXPECT_EQ(gate.last_id(), 2);
}

TEST(TradeSeqGate, ReplayFillsGapThenReturnsLive) {
    TradeSeqGate gate;
    ASSERT_EQ(gate.on_live_trade(1).action, TradeSeqGate::Action::Accept);
    ASSERT_EQ(gate.on_live_trade(5).action, TradeSeqGate::Action::Gap);  // missing 2..4
    ASSERT_FALSE(gate.can_trade());

    EXPECT_TRUE(gate.on_replay_trade(2));
    EXPECT_EQ(gate.state(), TradeSeqGate::State::Recovering);
    EXPECT_TRUE(gate.on_replay_trade(3));
    EXPECT_TRUE(gate.on_replay_trade(4));
    EXPECT_EQ(gate.state(), TradeSeqGate::State::Live);
    EXPECT_TRUE(gate.can_trade());
    EXPECT_EQ(gate.last_id(), 4);

    // 恢复后实时从 5 继续
    const auto r = gate.on_live_trade(5);
    EXPECT_EQ(r.action, TradeSeqGate::Action::Accept);
    EXPECT_EQ(gate.last_id(), 5);
}
