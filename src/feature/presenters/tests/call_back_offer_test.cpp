// V064: End & accept ended a 1:1 call; once the call taken in its place is over and nothing else is up, the user is
// offered the ended call again ("Call {name} back?"), once.
#include "feature/presenters/CallBackOffer.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

/** `call_id` (from carol) rings over the 1:1 call `ending` with bob. */
CallScreen RingOver(const std::string& call_id, const std::string& ending) {
  CallScreen s;
  s.kind = CallScreen::Kind::Ringing;
  s.ring_call_id = call_id;
  s.caller_identity = "carol";
  s.conflict = true;
  s.ending.call_id = ending;
  s.ending.peer_identity = "bob";
  s.ending.thread_id = "dm-bob";
  s.ending.video = true;
  return s;
}

CallScreen Answering(const std::string& call_id) {
  CallScreen s;
  s.kind = CallScreen::Kind::Answering;
  s.ring_call_id = call_id;
  return s;
}

CallScreen InCall(const std::string& call_id) {
  CallScreen s;
  s.kind = CallScreen::Kind::InCall;
  s.call_id = call_id;
  return s;
}

TEST(CallBackOfferTest, OfferedOnceTheCallTakenInsteadIsOver) {
  CallBackOffer offer;
  offer.OnAccept(RingOver("b", "a"), "b");
  EXPECT_FALSE(offer.OnFrame(Answering("b"), "").has_value());
  EXPECT_FALSE(offer.OnFrame(InCall("b"), "b").has_value()) << "not while the user is in the call taken";
  const auto ended = offer.OnFrame(CallScreen{}, "");
  ASSERT_TRUE(ended.has_value());
  EXPECT_EQ(ended->peer_identity, "bob");
  EXPECT_EQ(ended->thread_id, "dm-bob");
  EXPECT_TRUE(ended->video) << "called back the way it was";
  EXPECT_FALSE(offer.OnFrame(CallScreen{}, "").has_value()) << "once";
}

TEST(CallBackOfferTest, NotOfferedWithoutAConflict) {
  CallBackOffer offer;
  CallScreen ring = RingOver("b", "a");
  ring.conflict = false;
  offer.OnAccept(ring, "b");
  EXPECT_FALSE(offer.Pending());
}

TEST(CallBackOfferTest, NotOfferedForAGroupsCallOrTheSamePeer) {
  CallBackOffer offer;
  CallScreen group = RingOver("b", "a");
  group.ending.group = true; // it goes on without us; its chat's call bar offers Join
  offer.OnAccept(group, "b");
  EXPECT_FALSE(offer.Pending());
  CallScreen same = RingOver("b", "a");
  same.ending.same_peer = true; // the user is now talking to them
  offer.OnAccept(same, "b");
  EXPECT_FALSE(offer.Pending());
}

TEST(CallBackOfferTest, NotOfferedWithoutTheCallsChat) {
  CallBackOffer offer;
  CallScreen ring = RingOver("b", "a");
  ring.ending.thread_id.clear();
  offer.OnAccept(ring, "b");
  EXPECT_FALSE(offer.Pending());
}

TEST(CallBackOfferTest, AnotherCallUpDropsIt) {
  CallBackOffer offer;
  offer.OnAccept(RingOver("b", "a"), "b");
  EXPECT_FALSE(offer.OnFrame(InCall("c"), "c").has_value());
  EXPECT_FALSE(offer.Pending());
  EXPECT_FALSE(offer.OnFrame(CallScreen{}, "").has_value());
}

TEST(CallBackOfferTest, TheEndedCallStillUpDropsIt) {
  // The accept did not end it (it failed before leaving the call): nothing to call back.
  CallBackOffer offer;
  offer.OnAccept(RingOver("b", "a"), "b");
  EXPECT_FALSE(offer.OnFrame(InCall("a"), "a").has_value());
  EXPECT_FALSE(offer.Pending());
}

TEST(CallBackOfferTest, AnAcceptThatLeavesItRingingKeepsIt) {
  // The call taken rings again (the accept did not go through, after the first call ended); declined, it is over.
  CallBackOffer offer;
  offer.OnAccept(RingOver("b", "a"), "b");
  CallScreen ringing;
  ringing.kind = CallScreen::Kind::Ringing;
  ringing.ring_call_id = "b";
  EXPECT_FALSE(offer.OnFrame(ringing, "").has_value());
  EXPECT_TRUE(offer.OnFrame(CallScreen{}, "").has_value());
}

TEST(CallBackOfferTest, AnswerWithoutAConflictClearsAnEarlierOffer) {
  CallBackOffer offer;
  offer.OnAccept(RingOver("b", "a"), "b");
  CallScreen plain;
  plain.kind = CallScreen::Kind::Ringing;
  plain.ring_call_id = "c";
  offer.OnAccept(plain, "c");
  EXPECT_FALSE(offer.Pending());
}

} // namespace
} // namespace pbr
