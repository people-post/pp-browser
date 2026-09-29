#include "feature/calls/CallMediaCoordinator.h"

#include <gtest/gtest.h>

#include <vector>

namespace pbr {
namespace {

class CallMediaCoordinatorTest : public ::testing::Test {
protected:
  void SetUp() override {
    engine_.SetSkipDeviceOpenForTest(true);
    with_seat_.engine = &engine_;
    with_seat_.seat = &seat_;
    with_seat_.direct = &direct_;
    no_seat_.engine = &engine_;
  }
  void TearDown() override { engine_.Stop(); }

  struct RecordingDirectDriver final : CallDirectDriver {
    std::vector<std::string> started;
    std::vector<CallMediaSeat::Token> released;
    void ScheduleDirectStart(const std::string& call_id, const std::string&, bool) override {
      started.push_back(call_id);
    }
    void ReleaseDirectTransport(const CallMediaSeat::Token& token) override { released.push_back(token); }
    void ReleaseDirectTransport() override { released.push_back({}); }
  };

  CallMediaEngine engine_;
  CallMediaSeat seat_;
  RecordingDirectDriver direct_;
  CallMediaResources with_seat_;
  CallMediaResources no_seat_;
};

// A call's coordinator runs the one engine for that call: it takes the seat, starts the engine and
// marks the seat started on its path.
TEST_F(CallMediaCoordinatorTest, StartTakesTheSeatAndRunsTheEngineOnThePath) {
  CallMediaCoordinator call("call:1", with_seat_);
  ASSERT_TRUE(call.StartEngine(CallMediaSeat::PathKind::Direct, [](const CallMediaEngine::SfuPacket&) {}));
  EXPECT_TRUE(seat_.IsBound("call:1"));
  EXPECT_EQ(seat_.Path(), CallMediaSeat::PathKind::Direct);
  EXPECT_EQ(engine_.ActiveCallId(), "call:1");
  EXPECT_EQ(call.Path(), CallMediaSeat::PathKind::Direct);

  // Direct → Hop on the same call re-points the running engine; the seat stays this call's.
  ASSERT_TRUE(call.StartEngine(CallMediaSeat::PathKind::Hop, [](const CallMediaEngine::SfuPacket&) {}));
  EXPECT_TRUE(seat_.IsBound("call:1"));
  EXPECT_EQ(seat_.Path(), CallMediaSeat::PathKind::Hop);
}

// Another call starting media takes the seat from the first (one call owns media at a time).
TEST_F(CallMediaCoordinatorTest, ASecondCallTakesTheSeat) {
  CallMediaCoordinator first("call:1", with_seat_);
  CallMediaCoordinator second("call:2", with_seat_);
  ASSERT_TRUE(first.StartEngine(CallMediaSeat::PathKind::Direct, [](const CallMediaEngine::SfuPacket&) {}));
  ASSERT_TRUE(second.StartEngine(CallMediaSeat::PathKind::Direct, [](const CallMediaEngine::SfuPacket&) {}));
  EXPECT_TRUE(seat_.IsBound("call:2"));
  EXPECT_FALSE(seat_.IsBound("call:1"));
  EXPECT_EQ(engine_.ActiveCallId(), "call:2");
}

TEST_F(CallMediaCoordinatorTest, WithoutASeatOnlyTheEngineIsDriven) {
  CallMediaCoordinator call("call:1", no_seat_);
  ASSERT_TRUE(call.StartEngine(CallMediaSeat::PathKind::Direct, [](const CallMediaEngine::SfuPacket&) {}));
  EXPECT_EQ(engine_.ActiveCallId(), "call:1");
  call.StopEngine("test");
  EXPECT_FALSE(engine_.IsActive());
}

// The call starts on the direct path through its coordinator: seat first, then the 1:1 driver.
TEST_F(CallMediaCoordinatorTest, BeginDirectTakesTheSeatThenStartsTheDriver) {
  CallMediaCoordinator call("call:1", with_seat_);
  ASSERT_TRUE(call.BeginDirect("account:peer", true));
  EXPECT_TRUE(seat_.IsBound("call:1"));
  EXPECT_EQ(direct_.started, std::vector<std::string>{"call:1"});
}

// Direct → Hop hand-off: once the hop runs the call, the coordinator drops the 1:1 transport under
// the seat's token and marks the seat on Hop — only while this call holds the seat.
TEST_F(CallMediaCoordinatorTest, ReleaseDirectOnlyWhileTheCallHoldsTheSeat) {
  CallMediaCoordinator call("call:1", with_seat_);
  call.HoldSeatForHop();
  call.ReleaseDirect();
  ASSERT_EQ(direct_.released.size(), 1u);
  EXPECT_EQ(direct_.released.front().call_id, "call:1");
  EXPECT_EQ(seat_.Path(), CallMediaSeat::PathKind::Hop);

  CallMediaCoordinator other("call:2", with_seat_);
  other.HoldSeatForHop();  // another call took the seat
  call.ReleaseDirect();
  EXPECT_EQ(direct_.released.size(), 1u) << "not this call's transport to drop any more";
}

} // namespace
} // namespace pbr
