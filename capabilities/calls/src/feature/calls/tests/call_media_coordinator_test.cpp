#include "feature/calls/CallMediaCoordinator.h"
#include "feature/calls/tests/call_direct_driver_fake.h"

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
    with_seat_.make_direct = test::ForwardTo(direct_);
    with_seat_.make_hop = test::ForwardHopTo(hop_);
    no_seat_.engine = &engine_;
  }
  void TearDown() override { engine_.Stop(); }

  struct RecordingDirectDriver final : test::NullDirectDriver {
    std::vector<std::string> started;
    std::vector<CallMediaSeat::Token> released;
    void ScheduleDirectStart(const std::string& call_id, const std::string&, bool) override {
      started.push_back(call_id);
    }
    void ReleaseDirectTransport(const CallMediaSeat::Token& token) override { released.push_back(token); }
    void ReleaseDirectTransport() override { released.push_back({}); }
  };

  /** The group path: takes a call once `group_from` are joined; answers the hop questions. */
  struct FakeHopDriver final : test::NullHopDriver {
    size_t group_from = 3;
    bool attached = false;
    bool in_flight = false;
    bool expects = false;
    std::vector<std::string> attach_waits;
    bool OnLocalAcceptJoined(const std::string&, size_t n, const std::optional<std::string>&) override {
      return n >= group_from;
    }
    bool OnRemoteAcceptJoined(const std::string&, size_t n, const std::string&) override { return n >= group_from; }
    bool IsSfuAttached() const override { return attached; }
    bool IsAwaitingSfuRecovery() const override { return in_flight; }
    bool ExpectsGroupMedia(const std::string&) const override { return expects; }
    void BeginSfuAttachWait(const std::string& call_id) override { attach_waits.push_back(call_id); }
    void OnMediaStopped(const std::string&) override {}
    void RefreshAdaptation(const std::string&, bool) override {}
  };

  MediaEngine engine_;
  CallMediaSeat seat_;
  RecordingDirectDriver direct_;
  FakeHopDriver hop_;
  CallMediaResources with_seat_;
  CallMediaResources no_seat_;
};

// A call's coordinator runs the one engine for that call: it takes the seat and starts the engine.
TEST_F(CallMediaCoordinatorTest, StartTakesTheSeatAndRunsTheEngineOnThePath) {
  CallMediaCoordinator call("call:1", with_seat_);
  ASSERT_TRUE(call.StartEngine(CallMediaPath::Direct, [](const MediaEngine::SfuPacket&) {}));
  EXPECT_TRUE(seat_.IsBound("call:1"));
  EXPECT_EQ(engine_.ActiveCallId(), "call:1");
  const uint64_t direct_epoch = seat_.Epoch();

  // Direct → Hop on the same call re-points the running engine; the seat stays this call's.
  ASSERT_TRUE(call.StartEngine(CallMediaPath::Hop, [](const MediaEngine::SfuPacket&) {}));
  EXPECT_TRUE(seat_.IsBound("call:1"));
  EXPECT_GT(seat_.Epoch(), direct_epoch) << "a release posted before the re-point cannot stop it";
  EXPECT_EQ(engine_.ActiveCallId(), "call:1");
}

// Another call starting media takes the seat from the first (one call owns media at a time).
TEST_F(CallMediaCoordinatorTest, ASecondCallTakesTheSeat) {
  CallMediaCoordinator first("call:1", with_seat_);
  CallMediaCoordinator second("call:2", with_seat_);
  ASSERT_TRUE(first.StartEngine(CallMediaPath::Direct, [](const MediaEngine::SfuPacket&) {}));
  ASSERT_TRUE(second.StartEngine(CallMediaPath::Direct, [](const MediaEngine::SfuPacket&) {}));
  EXPECT_TRUE(seat_.IsBound("call:2"));
  EXPECT_FALSE(seat_.IsBound("call:1"));
  EXPECT_EQ(engine_.ActiveCallId(), "call:2");
}

TEST_F(CallMediaCoordinatorTest, WithoutASeatOnlyTheEngineIsDriven) {
  CallMediaCoordinator call("call:1", no_seat_);
  ASSERT_TRUE(call.StartEngine(CallMediaPath::Direct, [](const MediaEngine::SfuPacket&) {}));
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
// the seat's token — only while this call holds the seat.
TEST_F(CallMediaCoordinatorTest, ReleaseDirectOnlyWhileTheCallHoldsTheSeat) {
  CallMediaCoordinator call("call:1", with_seat_);
  ASSERT_NE(call.Direct(), nullptr); // the call ran 1:1 first
  call.HoldSeatForHop();
  call.ReleaseDirect();
  ASSERT_EQ(direct_.released.size(), 1u);
  EXPECT_EQ(direct_.released.front().call_id, "call:1");

  CallMediaCoordinator other("call:2", with_seat_);
  other.HoldSeatForHop();  // another call took the seat
  call.ReleaseDirect();
  EXPECT_EQ(direct_.released.size(), 1u) << "not this call's transport to drop any more";
}

// The call's coordinator decides the path at a join: the hop takes a group call, else 1:1 — and a
// call already on the hop (the call's path, which LiveCall keeps) never goes back to Direct.
TEST_F(CallMediaCoordinatorTest, JoinDecidesThePathAndGroupIsOneWay) {
  CallMediaCoordinator call("call:1", with_seat_);
  EXPECT_EQ(call.DecideOnRemoteAccept(2, "account:bob", CallMediaPath::Undecided), CallMediaPath::Direct);
  EXPECT_EQ(call.DecideOnRemoteAccept(3, "account:carol", CallMediaPath::Direct), CallMediaPath::Hop);
  hop_.group_from = 99;  // even if the hop would not take a later join
  EXPECT_EQ(call.DecideOnRemoteAccept(2, "account:bob", CallMediaPath::Hop), CallMediaPath::Hop) << "group is one-way";

  CallMediaCoordinator accepted("call:2", with_seat_);
  EXPECT_EQ(accepted.DecideOnLocalAccept(2, std::nullopt), CallMediaPath::Direct);
  CallMediaCoordinator no_hop("call:3", no_seat_);
  EXPECT_EQ(no_hop.DecideOnLocalAccept(5, std::nullopt), CallMediaPath::Direct) << "no group path wired";
}

// What the 1:1 path asks about the group path goes through the call's coordinator.
TEST_F(CallMediaCoordinatorTest, HopQuestionsAskTheHopDriver) {
  CallMediaCoordinator call("call:1", with_seat_);
  EXPECT_FALSE(call.HopAttached());
  EXPECT_FALSE(call.HopInFlight());
  EXPECT_FALSE(call.ExpectsHop());
  hop_.attached = hop_.in_flight = hop_.expects = true;
  EXPECT_TRUE(call.HopAttached());
  EXPECT_TRUE(call.HopInFlight());
  EXPECT_TRUE(call.ExpectsHop());
  call.ExpectHopAttach();
  EXPECT_EQ(hop_.attach_waits, std::vector<std::string>{"call:1"});
  CallMediaCoordinator no_hop("call:2", no_seat_);
  EXPECT_FALSE(no_hop.ExpectsHop());
}

} // namespace
} // namespace pbr
