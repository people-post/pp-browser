#include "lib/calls/CallControlGate.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// V057 4: the inbound edge's gate. An invite may come from anyone (the audience decides); our own
// account's devices say they answered; a group call's offered members may ask to join and hear it end;
// everything else steers a call only from someone in it.
TEST(CallControlGateTest, EachTypeHasItsSenders) {
  EXPECT_EQ(WhoMaySend(CallControlType::CallInvite), CallControlSender::Anyone);
  EXPECT_EQ(WhoMaySend(CallControlType::CallAnsweredElsewhere), CallControlSender::OwnAccount);
  EXPECT_EQ(WhoMaySend(CallControlType::CallJoinRequest), CallControlSender::OfferedOrInCall);
  EXPECT_EQ(WhoMaySend(CallControlType::CallEnded), CallControlSender::OfferedOrInCall);
  for (const auto type :
       {CallControlType::CallAccept, CallControlType::CallDecline, CallControlType::CallLeave,
        CallControlType::CallRoster, CallControlType::CallMediaKey, CallControlType::CallSfuAttach,
        CallControlType::CallSfuAttachFailed, CallControlType::CallHopRefuse, CallControlType::CallVideoRefresh,
        CallControlType::CallCircuitR1, CallControlType::CallPunchOffer, CallControlType::CallPunchAnswer,
        CallControlType::CallCapsUpdate, CallControlType::CallConnectGiveUp}) {
    EXPECT_EQ(WhoMaySend(type), CallControlSender::InCall) << static_cast<int>(type) << " steers a call: in it only";
  }
  for (const auto type : {CallControlType::CallSummary, CallControlType::CallSdp, CallControlType::CallIce}) {
    EXPECT_EQ(WhoMaySend(type), CallControlSender::Nobody) << static_cast<int>(type) << " is not signaling here";
  }
}

} // namespace
} // namespace pbr
