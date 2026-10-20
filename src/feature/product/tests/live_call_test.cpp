#include "feature/calls/LiveCall.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// A LiveCall follows the call from admission to close; only LiveCalls changes it.
TEST(LiveCallsTest, InviteAcceptLeave) {
  LiveCalls calls;
  const LiveCall& ringing = calls.AdmitInvited("call:1", {"account:alice"});
  EXPECT_EQ(ringing.State(), LiveCallState::Ringing);
  EXPECT_EQ(ringing.Origin(), LiveCallOrigin::Invited);
  EXPECT_EQ(ringing.SolePeer(), std::optional<std::string>("account:alice"));
  EXPECT_EQ(calls.Active(), nullptr) << "a ringing call is not the active call";
  EXPECT_EQ(calls.Ringing().size(), 1u);

  calls.MarkAccepting("call:1");
  EXPECT_EQ(calls.Find("call:1")->State(), LiveCallState::Accepting);
  calls.MarkJoined("call:1");
  ASSERT_NE(calls.Active(), nullptr);
  EXPECT_EQ(calls.Active()->Id(), "call:1");

  calls.Close("call:1", LiveCallEndReason::LocalLeave);
  calls.Close("call:1", LiveCallEndReason::RemoteEnded);
  EXPECT_EQ(calls.Find("call:1")->State(), LiveCallState::Ended);
  EXPECT_EQ(calls.Find("call:1")->EndReason(), LiveCallEndReason::LocalLeave) << "the first reason sticks";
  EXPECT_EQ(calls.Active(), nullptr);
}

TEST(LiveCallsTest, AFailedAcceptRingsAgain) {
  LiveCalls calls;
  calls.AdmitInvited("call:1", {"account:alice"});
  calls.MarkAccepting("call:1");
  calls.MarkAcceptFailed("call:1");
  EXPECT_EQ(calls.Find("call:1")->State(), LiveCallState::Ringing);
}

TEST(LiveCallsTest, ARedeliveredInviteChangesNothing) {
  LiveCalls calls;
  calls.AdmitInvited("call:1", {"account:alice"});
  calls.MarkAccepting("call:1");
  calls.MarkJoined("call:1");
  const uint64_t instance = calls.Find("call:1")->Instance();
  calls.AdmitInvited("call:1", {"account:alice", "account:bob"});
  EXPECT_EQ(calls.Find("call:1")->State(), LiveCallState::Joined);
  EXPECT_EQ(calls.Find("call:1")->Instance(), instance);
  EXPECT_EQ(calls.Find("call:1")->Peers().size(), 1u);
}

TEST(LiveCallsTest, AReadmittedIdIsANewInstance) {
  LiveCalls calls;
  const uint64_t first = calls.AdmitPlaced("call:1", {"account:bob"}).Instance();
  calls.Close("call:1", LiveCallEndReason::Unanswered);
  const LiveCall& again = calls.AdmitInvited("call:1", {"account:bob"});
  EXPECT_NE(again.Instance(), first);
  EXPECT_TRUE(again.IsOpen());
  EXPECT_EQ(again.EndReason(), LiveCallEndReason::None);
}

TEST(LiveCallsTest, PlacedCallJoinsWhenAnsweredAndTracksGroupPeers) {
  LiveCalls calls;
  calls.AdmitPlaced("call:g", {"account:bob"});
  EXPECT_EQ(calls.Active()->State(), LiveCallState::Calling);
  calls.MarkJoined("call:g");
  calls.AddPeer("call:g", "account:carol");
  calls.AddPeer("call:g", "account:carol");
  EXPECT_EQ(calls.Find("call:g")->Peers().size(), 2u);
  EXPECT_EQ(calls.Find("call:g")->SolePeer(), std::nullopt);
  calls.RemovePeer("call:g", "account:bob");
  EXPECT_EQ(calls.Find("call:g")->SolePeer(), std::optional<std::string>("account:carol"));
}

TEST(LiveCallsTest, OldEndedCallsArePruned) {
  LiveCalls calls;
  for (int i = 0; i < 40; ++i) {
    const std::string id = "call:" + std::to_string(i);
    calls.AdmitPlaced(id, {"account:bob"});
    calls.Close(id, LiveCallEndReason::LocalLeave);
  }
  EXPECT_EQ(calls.Find("call:0"), nullptr);
  ASSERT_NE(calls.Find("call:39"), nullptr);
  EXPECT_EQ(calls.Find("call:39")->EndReason(), LiveCallEndReason::LocalLeave);
}

// "The peer ended it" is news only for a call this side had: not a ring the caller withdrew.
TEST(LiveCallsTest, EndedByPeerOnlyForACallThisSideHad) {
  LiveCalls calls;
  calls.AdmitInvited("call:ring", {"account:alice"});
  calls.Close("call:ring", LiveCallEndReason::RemoteEnded);
  EXPECT_FALSE(calls.Find("call:ring")->EndedByPeer()) << "withdrawn ring";
  EXPECT_EQ(calls.Find("call:ring")->StateAtClose(), LiveCallState::Ringing);

  calls.AdmitPlaced("call:placed", {"account:bob"});
  calls.Close("call:placed", LiveCallEndReason::DeclinedByPeer);
  EXPECT_TRUE(calls.Find("call:placed")->EndedByPeer());
  EXPECT_EQ(calls.LastEnded()->Id(), "call:placed");

  calls.AdmitPlaced("call:left", {"account:bob"});
  calls.MarkJoined("call:left");
  calls.Close("call:left", LiveCallEndReason::LocalLeave);
  EXPECT_FALSE(calls.Find("call:left")->EndedByPeer());
  // PR #240 review: a later local close must not hide the peer's end before the UI reads it.
  ASSERT_NE(calls.LastEndedByPeer(), nullptr);
  EXPECT_EQ(calls.LastEndedByPeer()->Id(), "call:placed");
  calls.AdmitPlaced("call:placed", {"account:bob"});  // the same id again: that news is gone
  EXPECT_EQ(calls.LastEndedByPeer(), nullptr);
}

// The ring the device shows is the newest call still Ringing or being accepted.
TEST(LiveCallsTest, TheRingIsTheNewestRingingOrAcceptingCall) {
  LiveCalls calls;
  EXPECT_EQ(calls.TheRing(), nullptr);
  calls.AdmitInvited("call:a", {"account:alice"});
  calls.AdmitInvited("call:b", {"account:bob"});
  ASSERT_NE(calls.TheRing(), nullptr);
  EXPECT_EQ(calls.TheRing()->Id(), "call:b");
  calls.MarkAccepting("call:b");
  EXPECT_EQ(calls.TheRing()->Id(), "call:b") << "still the ring while its accept is in flight";
  calls.MarkJoined("call:b");
  EXPECT_EQ(calls.TheRing()->Id(), "call:a") << "a joined call is not a ring";
  calls.Close("call:a", LiveCallEndReason::Expired);
  EXPECT_EQ(calls.TheRing(), nullptr);
}

} // namespace
} // namespace pbr
