// CallReads::ActiveLocalCall (DEV/PLAN-call-ui-never-detached.md 1): a read that fails is an error, never "no call" —
// the call screen took that for the call's end while this device was still in it, mic open.
#include "feature/calls/CallReads.h"

#include "lib/calls/testing/FailingCallSessionStore.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"

#include <gtest/gtest.h>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

class FakeAccount final : public ILocalAccount {
public:
  Roe<std::string> GetAccountId() const override { return std::string("account:me"); }
  Roe<int64_t> InitiationFloor() const override { return int64_t{0}; }
};

class CallReadsTest : public ::testing::Test {
protected:
  void SetUp() override {
    CallSession session;
    session.call_id = "call:on";
    session.state = CallSessionState::Active;
    session.created_at = 1000;
    session.media_key_id = "mk:1";
    ASSERT_TRUE(sessions_.UpsertSession(session));
    CallParticipant me;
    me.call_id = session.call_id;
    me.identity = "account:me";
    me.state = CallParticipantState::Joined;
    ASSERT_TRUE(sessions_.UpsertParticipant(me));
  }

  FailingCallSessionStore sessions_;
  FakeAccount account_;
  InMemoryDeliveryStore threads_;
  CallReads reads_{sessions_, account_, threads_, {}, nullptr};
};

TEST_F(CallReadsTest, TheCallThisDeviceIsJoinedIn) {
  auto active = reads_.ActiveLocalCall();
  ASSERT_TRUE(active && active->has_value());
  EXPECT_EQ((*active)->call_id, "call:on");
}

TEST_F(CallReadsTest, AParticipantsReadThatFailsIsAnErrorNotNoCall) {
  sessions_.fail_participants = true;
  auto active = reads_.ActiveLocalCall();
  ASSERT_FALSE(active) << "read as " << (active->has_value() ? "a call" : "no call");
  EXPECT_NE(active.error().message.find("ListParticipants"), std::string::npos) << active.error().message;
}

TEST_F(CallReadsTest, AnActiveCallsReadThatFailsIsAnError) {
  sessions_.fail_active = true;
  EXPECT_FALSE(reads_.ActiveLocalCall());
}

TEST_F(CallReadsTest, TheDeveloperSwitchFailsTheRead) {
  bool fail = true;
  reads_.SetDevFailActiveRead([&fail]() { return fail; });
  auto active = reads_.ActiveLocalCall();
  ASSERT_FALSE(active);
  EXPECT_NE(active.error().message.find("debug.pp.fail_call_read"), std::string::npos) << active.error().message;
  fail = false;
  EXPECT_TRUE(reads_.ActiveLocalCall() && reads_.ActiveLocalCall()->has_value());
}

} // namespace
} // namespace pbr
