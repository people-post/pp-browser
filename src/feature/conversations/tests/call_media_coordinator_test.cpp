#include "feature/calls/CallMediaCoordinator.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

class CallMediaCoordinatorTest : public ::testing::Test {
protected:
  void SetUp() override { engine_.SetSkipDeviceOpenForTest(true); }
  void TearDown() override { engine_.Stop(); }

  CallMediaEngine engine_;
  CallMediaSeat seat_;
};

// A call's coordinator runs the one engine for that call: it takes the seat, starts the engine and
// marks the seat started on its path.
TEST_F(CallMediaCoordinatorTest, StartTakesTheSeatAndRunsTheEngineOnThePath) {
  CallMediaCoordinator call("call:1", engine_, &seat_);
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
  CallMediaCoordinator first("call:1", engine_, &seat_);
  CallMediaCoordinator second("call:2", engine_, &seat_);
  ASSERT_TRUE(first.StartEngine(CallMediaSeat::PathKind::Direct, [](const CallMediaEngine::SfuPacket&) {}));
  ASSERT_TRUE(second.StartEngine(CallMediaSeat::PathKind::Direct, [](const CallMediaEngine::SfuPacket&) {}));
  EXPECT_TRUE(seat_.IsBound("call:2"));
  EXPECT_FALSE(seat_.IsBound("call:1"));
  EXPECT_EQ(engine_.ActiveCallId(), "call:2");
}

TEST_F(CallMediaCoordinatorTest, WithoutASeatOnlyTheEngineIsDriven) {
  CallMediaCoordinator call("call:1", engine_, nullptr);
  ASSERT_TRUE(call.StartEngine(CallMediaSeat::PathKind::Direct, [](const CallMediaEngine::SfuPacket&) {}));
  EXPECT_EQ(engine_.ActiveCallId(), "call:1");
  call.StopEngine("test");
  EXPECT_FALSE(engine_.IsActive());
}

} // namespace
} // namespace pbr
