#include "domain/messaging/CallThreadPresenceLogic.h"
#include "domain/messaging/CallControlCodec.h"

#include <gtest/gtest.h>

using namespace pbr;

TEST(CallThreadPresenceLogicTest, ShadowRequiresPrivateSiblingAndEmptyChrome) {
  Thread pub;
  pub.id = "t-pub";
  pub.kind = ThreadKind::Direct;
  pub.channel = ThreadChannel::E2ePublic;
  pub.peer_identity_value = "account:bob";
  pub.preview = "";
  pub.unread_count = 0;

  Thread priv;
  priv.id = "t-priv";
  priv.kind = ThreadKind::Direct;
  priv.channel = ThreadChannel::E2e;
  priv.peer_identity_value = "account:bob";
  priv.preview = "hello";

  EXPECT_TRUE(HasPrivateE2eSibling(pub, {pub, priv}));
  EXPECT_TRUE(IsCallControlShadowThread(pub, {pub, priv}));

  pub.preview = "hi";
  EXPECT_TRUE(HasPrivateE2eSibling(pub, {pub, priv}));
  EXPECT_FALSE(IsCallControlShadowThread(pub, {pub, priv}));
  pub.preview = "";
  pub.unread_count = 1;
  EXPECT_FALSE(IsCallControlShadowThread(pub, {pub, priv}));
}

TEST(CallThreadPresenceLogicTest, ShadowFalseWithoutPrivateSibling) {
  Thread pub;
  pub.id = "t-pub";
  pub.kind = ThreadKind::Direct;
  pub.channel = ThreadChannel::E2ePublic;
  pub.peer_identity_value = "account:bob";

  Thread other;
  other.id = "t-other";
  other.kind = ThreadKind::Direct;
  other.channel = ThreadChannel::E2ePublic;
  other.peer_identity_value = "account:bob";

  EXPECT_FALSE(IsCallControlShadowThread(pub, {pub, other}));
}

TEST(CallThreadPresenceLogicTest, MatchesOriginThreadAndJoinedPeer) {
  CallSession session;
  session.call_id = "call:1";
  session.origin_thread_id = "t-origin";

  Thread origin;
  origin.id = "t-origin";
  origin.kind = ThreadKind::Direct;
  EXPECT_TRUE(ThreadMatchesActiveCall(origin, session, {}));

  Thread peer_dm;
  peer_dm.id = "t-peer";
  peer_dm.kind = ThreadKind::Direct;
  peer_dm.peer_identity_value = "account:bob";
  CallParticipant bob;
  bob.identity = "account:bob";
  bob.state = CallParticipantState::Joined;
  EXPECT_TRUE(ThreadMatchesActiveCall(peer_dm, session, {bob}));

  bob.state = CallParticipantState::Ringing;
  EXPECT_FALSE(ThreadMatchesActiveCall(peer_dm, session, {bob}));

  Thread stranger;
  stranger.id = "t-x";
  stranger.kind = ThreadKind::Direct;
  stranger.peer_identity_value = "account:carol";
  bob.state = CallParticipantState::Joined;
  EXPECT_FALSE(ThreadMatchesActiveCall(stranger, session, {bob}));
}

TEST(CallThreadPresenceLogicTest, MatchesGroupOrigin) {
  CallSession session;
  session.call_id = "call:1";
  session.origin_group_id = "group:1";

  Thread group;
  group.id = "t-g";
  group.kind = ThreadKind::Group;
  group.group_id = "group:1";
  EXPECT_TRUE(ThreadMatchesActiveCall(group, session, {}));
}

TEST(CallThreadPresenceLogicTest, TranscriptOnlyCallControlRespectsScanLimit) {
  EXPECT_TRUE(TranscriptIsOnlyCallControl({}, kOrphanCallControlShadowScanLimit));

  auto invite = CallControlCodec::BuildSystemMessage("t", CallControlType::CallInvite, "ring", "{}", "me");
  ASSERT_TRUE(static_cast<bool>(invite));
  EXPECT_TRUE(TranscriptIsOnlyCallControl({*invite}, kOrphanCallControlShadowScanLimit));

  ThreadMessage chat;
  chat.content_type = ChatContentType::Text;
  chat.text = "hi";
  EXPECT_FALSE(TranscriptIsOnlyCallControl({*invite, chat}, kOrphanCallControlShadowScanLimit));

  std::vector<ThreadMessage> full(kOrphanCallControlShadowScanLimit, *invite);
  EXPECT_FALSE(TranscriptIsOnlyCallControl(full, kOrphanCallControlShadowScanLimit));

  std::vector<ThreadMessage> under(kOrphanCallControlShadowScanLimit - 1, *invite);
  EXPECT_TRUE(TranscriptIsOnlyCallControl(under, kOrphanCallControlShadowScanLimit));
}
