#include "domain/messaging/PeerAnnounceCodec.h"
#include "domain/messaging/PeerAnnounceFeed.h"
#include "domain/messaging/AnnounceDmReply.h"
#include "domain/messaging/AnnounceOverlayReply.h"
#include "domain/messaging/AnnounceNotificationInbox.h"
#include "domain/messaging/AnnounceLiveJoin.h"
#include "domain/messaging/AnnounceLiveJoinHandoff.h"
#include "domain/messaging/PeerAnnounceKeyResolve.h"
#include "domain/messaging/PeerAnnouncePublisher.h"
#include "domain/messaging/PeerAnnounceRpcCodec.h"
#include "domain/messaging/PeerSigningKeyStore.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/identity/PeerIdUtil.h"

#include "common/directory/DirectoryJson.h"
#include "common/thread/ThreadChannel.h"

#include <gtest/gtest.h>
#include <variant>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

PeerAnnounceTip SampleTip(const std::string& topic_id) {
  PeerAnnounceTip tip;
  tip.peer_id = "12D3KooWPublisherTest";
  tip.topic_id = topic_id;
  tip.program_id = "show-1";
  tip.state = PeerAnnounceState::Live;
  tip.seq = 1;
  tip.epoch = 0;
  tip.created_at_ms = 1'700'000'000'000;
  tip.join_handle = "session-abc";
  tip.body = "live";
  return tip;
}

} // namespace

TEST(PeerAnnounceTest, TopicIdIsStableAndPeerScoped) {
  auto a = MakePeerAnnounceTopicId("peer-a", "live");
  auto b = MakePeerAnnounceTopicId("peer-a", "live");
  auto c = MakePeerAnnounceTopicId("peer-b", "live");
  auto d = MakePeerAnnounceTopicId("peer-a", "other");
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  ASSERT_TRUE(c);
  ASSERT_TRUE(d);
  EXPECT_EQ(*a, *b);
  EXPECT_NE(*a, *c);
  EXPECT_NE(*a, *d);
  EXPECT_EQ(a->size(), 64u);
}

TEST(PeerAnnounceTest, SignVerifyAndJsonRoundTrip) {
  auto keys = MlDsa::GenerateKeyPair();
  ASSERT_TRUE(keys);
  auto topic = MakePeerAnnounceTopicId("12D3KooWPublisherTest", "live");
  ASSERT_TRUE(topic);
  auto tip = SampleTip(*topic);
  auto signed_tip = SignPeerAnnounceTip(tip, keys->secret_key);
  ASSERT_TRUE(signed_tip) << signed_tip.error().message;
  ASSERT_TRUE(VerifyPeerAnnounceTip(*signed_tip, keys->public_key));

  auto json = EncodePeerAnnounceTipJson(*signed_tip);
  ASSERT_TRUE(json);
  auto decoded = DecodePeerAnnounceTipJson(*json);
  ASSERT_TRUE(decoded) << decoded.error().message;
  EXPECT_EQ(decoded->peer_id, signed_tip->peer_id);
  EXPECT_EQ(decoded->signature_b64, signed_tip->signature_b64);
  ASSERT_TRUE(VerifyPeerAnnounceTip(*decoded, keys->public_key));
}

TEST(PeerAnnounceTest, FeedRejectsStaleAndBadSignature) {
  auto keys = MlDsa::GenerateKeyPair();
  ASSERT_TRUE(keys);
  auto topic = MakePeerAnnounceTopicId("12D3KooWPublisherTest", "live");
  ASSERT_TRUE(topic);

  PeerAnnounceFeed feed(keys->public_key);
  auto tip1 = SampleTip(*topic);
  tip1.seq = 1;
  auto signed1 = SignPeerAnnounceTip(tip1, keys->secret_key);
  ASSERT_TRUE(signed1);
  ASSERT_TRUE(feed.Ingest(*signed1));

  auto tip0 = SampleTip(*topic);
  tip0.seq = 0;
  auto signed0 = SignPeerAnnounceTip(tip0, keys->secret_key);
  ASSERT_TRUE(signed0);
  EXPECT_FALSE(feed.Ingest(*signed0));

  auto other = MlDsa::GenerateKeyPair();
  ASSERT_TRUE(other);
  auto tip2 = SampleTip(*topic);
  tip2.seq = 2;
  auto forged = SignPeerAnnounceTip(tip2, other->secret_key);
  ASSERT_TRUE(forged);
  EXPECT_FALSE(feed.Ingest(*forged));

  auto signed2 = SignPeerAnnounceTip(tip2, keys->secret_key);
  ASSERT_TRUE(signed2);
  ASSERT_TRUE(feed.Ingest(*signed2));
  auto latest = feed.Latest("12D3KooWPublisherTest", *topic, "show-1");
  ASSERT_TRUE(latest);
  EXPECT_EQ(latest->seq, 2u);
}

TEST(PeerAnnounceTest, HeartbeatMinInterval) {
  EXPECT_TRUE(PeerAnnounceHeartbeatDue(0, 100));
  EXPECT_FALSE(PeerAnnounceHeartbeatDue(1'000, 1'000 + kPeerAnnounceLiveHeartbeatMinIntervalMs - 1));
  EXPECT_TRUE(PeerAnnounceHeartbeatDue(1'000, 1'000 + kPeerAnnounceLiveHeartbeatMinIntervalMs));
  EXPECT_EQ(PeerAnnounceNextHeartbeatAtMs(1'000, 0.0), 1'000 + kPeerAnnounceLiveHeartbeatMinIntervalMs);
  EXPECT_EQ(PeerAnnounceNextHeartbeatAtMs(1'000, 1.0), 1'000 + kPeerAnnounceLiveHeartbeatMaxIntervalMs);
}

TEST(PeerAnnounceTest, PublisherLocalFeedAndHeartbeat) {
  auto keys = MlDsa::GenerateKeyPair();
  ASSERT_TRUE(keys);
  auto topic = MakePeerAnnounceTopicId("12D3KooWPublisherTest", "live");
  ASSERT_TRUE(topic);

  PeerAnnounceFeed feed(keys->public_key);
  PeerAnnouncePublisher publisher("12D3KooWPublisherTest", keys->secret_key, &feed);

  PeerAnnouncePublisher::Draft go_live;
  go_live.topic_id = *topic;
  go_live.program_id = "show-1";
  go_live.state = PeerAnnounceState::Live;
  go_live.join_handle = "session-abc";
  go_live.body = "Now live";
  auto tip = publisher.Publish(go_live, 1'000);
  ASSERT_TRUE(tip) << tip.error().message;
  EXPECT_EQ(tip->seq, 1u);
  EXPECT_EQ(tip->body, "Now live");
  ASSERT_TRUE(feed.Latest("12D3KooWPublisherTest", *topic, "show-1"));

  auto too_soon = publisher.MaybeEmitHeartbeat(1'000 + 1'000);
  ASSERT_TRUE(too_soon);
  EXPECT_FALSE(too_soon->has_value());

  auto hb = publisher.MaybeEmitHeartbeat(1'000 + kPeerAnnounceLiveHeartbeatMinIntervalMs);
  ASSERT_TRUE(hb) << hb.error().message;
  ASSERT_TRUE(hb->has_value());
  EXPECT_EQ((*hb)->seq, 2u);
  EXPECT_TRUE((*hb)->body.empty());
  EXPECT_EQ((*hb)->state, PeerAnnounceState::Live);

  PeerAnnouncePublisher::Draft end;
  end.topic_id = *topic;
  end.program_id = "show-1";
  end.state = PeerAnnounceState::Ended;
  end.content_id_hex = "abcd";
  auto ended = publisher.Publish(end, 1'000 + 2 * kPeerAnnounceLiveHeartbeatMinIntervalMs);
  ASSERT_TRUE(ended);
  EXPECT_EQ(ended->state, PeerAnnounceState::Ended);

  auto no_hb = publisher.MaybeEmitHeartbeat(1'000 + 10 * kPeerAnnounceLiveHeartbeatMinIntervalMs);
  ASSERT_TRUE(no_hb);
  EXPECT_FALSE(no_hb->has_value());
}

TEST(PeerAnnounceTest, RpcTipPushAckRoundTrip) {
  auto keys = MlDsa::GenerateKeyPair();
  ASSERT_TRUE(keys);
  auto topic = MakePeerAnnounceTopicId("12D3KooWPublisherTest", "live");
  ASSERT_TRUE(topic);
  auto tip = SampleTip(*topic);
  auto signed_tip = SignPeerAnnounceTip(tip, keys->secret_key);
  ASSERT_TRUE(signed_tip);

  auto push_json = EncodePeerAnnounceTipPush(*signed_tip);
  ASSERT_TRUE(push_json);
  auto decoded = DecodePeerAnnounceRpcJson(*push_json);
  ASSERT_TRUE(decoded) << decoded.error().message;
  ASSERT_TRUE(std::holds_alternative<PeerAnnounceTipPush>(*decoded));
  EXPECT_EQ(std::get<PeerAnnounceTipPush>(*decoded).tip.signature_b64, signed_tip->signature_b64);

  PeerAnnounceTipAck ack;
  ack.ok = true;
  ack.seq = signed_tip->seq;
  ack.epoch = signed_tip->epoch;
  auto ack_json = EncodePeerAnnounceTipAck(ack);
  ASSERT_TRUE(ack_json);
  auto decoded_ack = DecodePeerAnnounceRpcJson(*ack_json);
  ASSERT_TRUE(decoded_ack);
  ASSERT_TRUE(std::holds_alternative<PeerAnnounceTipAck>(*decoded_ack));
  EXPECT_TRUE(std::get<PeerAnnounceTipAck>(*decoded_ack).ok);
  EXPECT_STREQ(kRpcPeerAnnounceProtocolId, "/pp-browser/rpc/peer-announce/1.0.0");
}

TEST(PeerAnnounceKeyResolveTest, PrefersLocalDeviceKey) {
  auto keys = MlDsa::GenerateKeyPair();
  ASSERT_TRUE(keys);
  auto peer_id = PeerIdFromMlDsaPublicKey(keys->public_key);
  ASSERT_TRUE(peer_id);

  PeerSigningKeyStore store;
  auto resolved =
      ResolvePeerAnnouncePublisherKey(*peer_id, *peer_id, keys->public_key, store);
  ASSERT_TRUE(resolved);
  EXPECT_EQ(*resolved, keys->public_key);
}

TEST(PeerAnnounceKeyResolveTest, ResolvesPeerIdStoreAndRejectsAccountKind) {
  auto keys = MlDsa::GenerateKeyPair();
  ASSERT_TRUE(keys);
  auto peer_id = PeerIdFromMlDsaPublicKey(keys->public_key);
  ASSERT_TRUE(peer_id);

  PeerSigningKeyStore store;
  PeerSigningKeyRecord record;
  record.signing_public_key_b64 = Base64Encode(keys->public_key);
  record.source = "test";
  store.Put(ContactIdKindToString(ContactIdKind::PeerId), *peer_id, record);

  auto resolved = ResolvePeerAnnouncePublisherKey(*peer_id, "other-local", {}, store);
  ASSERT_TRUE(resolved);
  EXPECT_EQ(*resolved, keys->public_key);

  PeerSigningKeyStore account_only;
  account_only.Put(ContactIdKindToString(ContactIdKind::Account), *peer_id, record);
  EXPECT_FALSE(ResolvePeerAnnouncePublisherKey(*peer_id, "other-local", {}, account_only));
}

TEST(PeerAnnounceKeyResolveTest, RejectsPeerIdBindMismatch) {
  auto keys = MlDsa::GenerateKeyPair();
  ASSERT_TRUE(keys);
  auto peer_id = PeerIdFromMlDsaPublicKey(keys->public_key);
  ASSERT_TRUE(peer_id);

  PeerSigningKeyStore store;
  PeerSigningKeyRecord record;
  record.signing_public_key_b64 = Base64Encode(keys->public_key);
  record.source = "test";
  store.Put(ContactIdKindToString(ContactIdKind::PeerId), "not-the-derived-peer-id", record);

  EXPECT_FALSE(
      ResolvePeerAnnouncePublisherKey("not-the-derived-peer-id", "other-local", {}, store));
  EXPECT_FALSE(ResolvePeerAnnouncePublisherKey(*peer_id, "other-local", {}, store));
}


TEST(AnnounceDmReplyTest, PrefersAccountThreadWhenContactKnown) {
  auto plan = PlanAnnounceDmReply("12D3KooWPublisher", "c1", "account:alice", "Alice");
  ASSERT_TRUE(plan) << plan.error().message;
  EXPECT_EQ(plan->target.peer_identity_kind, ContactIdKindToString(ContactIdKind::Account));
  EXPECT_EQ(plan->target.peer_identity_value, "account:alice");
  EXPECT_EQ(plan->target.channel, ThreadChannel::E2ePublic);
  EXPECT_EQ(plan->contact_id, "c1");
  EXPECT_EQ(plan->thread_title, "Alice");
}

TEST(AnnounceDmReplyTest, FallsBackToPeerIdThread) {
  auto plan = PlanAnnounceDmReply("12D3KooWPublisher", "", "", "");
  ASSERT_TRUE(plan) << plan.error().message;
  EXPECT_EQ(plan->target.peer_identity_kind, ContactIdKindToString(ContactIdKind::PeerId));
  EXPECT_EQ(plan->target.peer_identity_value, "12D3KooWPublisher");
  EXPECT_EQ(plan->thread_title, "12D3KooWPublisher");
}

TEST(AnnounceDmReplyTest, RejectsEmptyPeerId) {
  auto plan = PlanAnnounceDmReply("", "c1", "account:alice", "Alice");
  EXPECT_FALSE(plan);
}


TEST(AnnounceLiveJoinTest, PlansCallIdFromLiveJoinHandle) {
  auto tip = SampleTip("topic-live");
  tip.state = PeerAnnounceState::Live;
  tip.join_handle = "session-abc";
  tip.seq = 3;
  tip.epoch = 2;

  ASSERT_TRUE(TipIsLiveJoinable(tip));
  auto plan = PlanAnnounceLiveJoin(tip);
  ASSERT_TRUE(plan) << plan.error().message;
  EXPECT_EQ(plan->call_id, "session-abc");
  EXPECT_EQ(plan->publisher_peer_id, tip.peer_id);
  EXPECT_EQ(plan->topic_id, tip.topic_id);
  EXPECT_EQ(plan->program_id, tip.program_id);
  EXPECT_EQ(plan->seq, 3u);
  EXPECT_EQ(plan->epoch, 2u);
}

TEST(AnnounceLiveJoinTest, RejectsScheduledEndedOrMissingHandle) {
  auto tip = SampleTip("topic-live");
  tip.state = PeerAnnounceState::Scheduled;
  EXPECT_FALSE(TipIsLiveJoinable(tip));
  EXPECT_FALSE(PlanAnnounceLiveJoin(tip));

  tip.state = PeerAnnounceState::Ended;
  tip.join_handle = "session-abc";
  EXPECT_FALSE(TipIsLiveJoinable(tip));
  EXPECT_FALSE(PlanAnnounceLiveJoin(tip));

  tip.state = PeerAnnounceState::Live;
  tip.join_handle.clear();
  EXPECT_FALSE(TipIsLiveJoinable(tip));
  EXPECT_FALSE(PlanAnnounceLiveJoin(tip));
}

TEST(AnnounceLiveJoinTest, RejectsEmptyPublisherPeerId) {
  PeerAnnounceTip tip;
  tip.state = PeerAnnounceState::Live;
  tip.join_handle = "session-abc";
  EXPECT_FALSE(TipIsLiveJoinable(tip));
  EXPECT_FALSE(PlanAnnounceLiveJoin(tip));
}


TEST(AnnounceLiveJoinHandoffTest, BuildsPendingInviteAndRingingSession) {
  AnnounceLiveJoinPlan plan;
  plan.call_id = "session-abc";
  plan.publisher_peer_id = "12D3KooWPublisher";
  plan.topic_id = "topic-1";
  plan.program_id = "show-1";
  plan.seq = 4;
  plan.epoch = 1;

  auto handoff = BuildAnnounceLiveJoinHandoff(plan, "account:bob", "account:alice", 1'700'000'000'000, true);
  ASSERT_TRUE(handoff) << handoff.error().message;
  EXPECT_EQ(handoff->pending.call_id, "session-abc");
  EXPECT_EQ(handoff->pending.inviter_identity, "account:alice");
  EXPECT_EQ(handoff->pending.invitee_identity, "account:bob");
  EXPECT_EQ(handoff->pending.media_mode, CallMediaMode::Video);
  EXPECT_TRUE(handoff->pending.video_allowed);
  EXPECT_EQ(handoff->pending.status, "pending");
  ASSERT_TRUE(handoff->pending.expires_at.has_value());
  EXPECT_EQ(*handoff->pending.expires_at, 1'700'000'000'000 + kDefaultCallInviteTtlMs);
  EXPECT_EQ(handoff->session.call_id, "session-abc");
  EXPECT_EQ(handoff->session.state, CallSessionState::Ringing);
  EXPECT_EQ(handoff->session.media_mode, CallMediaMode::Video);
}

TEST(AnnounceLiveJoinHandoffTest, RejectsMissingIdentities) {
  AnnounceLiveJoinPlan plan;
  plan.call_id = "session-abc";
  plan.publisher_peer_id = "12D3KooWPublisher";
  EXPECT_FALSE(BuildAnnounceLiveJoinHandoff(plan, "", "account:alice", 1, true));
  EXPECT_FALSE(BuildAnnounceLiveJoinHandoff(plan, "account:bob", "", 1, true));
  plan.call_id.clear();
  EXPECT_FALSE(BuildAnnounceLiveJoinHandoff(plan, "account:bob", "account:alice", 1, true));
}


TEST(PeerAnnounceHopPeerIdTest, JsonAndSignRoundTripWithHop) {
  auto keys = MlDsa::GenerateKeyPair();
  ASSERT_TRUE(keys);
  auto tip = SampleTip("topic-hop");
  tip.hop_peer_id = "12D3KooWMediaHop";
  auto signed_tip = SignPeerAnnounceTip(tip, keys->secret_key);
  ASSERT_TRUE(signed_tip) << signed_tip.error().message;
  ASSERT_TRUE(VerifyPeerAnnounceTip(*signed_tip, keys->public_key));

  auto json = EncodePeerAnnounceTipJson(*signed_tip);
  ASSERT_TRUE(json);
  auto decoded = DecodePeerAnnounceTipJson(*json);
  ASSERT_TRUE(decoded) << decoded.error().message;
  EXPECT_EQ(decoded->hop_peer_id, "12D3KooWMediaHop");
  ASSERT_TRUE(VerifyPeerAnnounceTip(*decoded, keys->public_key));
}

TEST(PeerAnnounceHopPeerIdTest, EmptyHopKeepsLegacyCanonical) {
  auto tip = SampleTip("topic-hop");
  tip.hop_peer_id.clear();
  const std::string with_empty = PeerAnnounceCanonicalSignBytes(tip);
  EXPECT_EQ(with_empty.find("hop_peer_id="), std::string::npos);
}

TEST(AnnounceLiveJoinTest, CopiesHopPeerIdIntoPlanAndHandoffSfuHint) {
  auto tip = SampleTip("topic-hop");
  tip.state = PeerAnnounceState::Live;
  tip.join_handle = "session-live-1";
  tip.hop_peer_id = "12D3KooWMediaHop";
  auto plan = PlanAnnounceLiveJoin(tip);
  ASSERT_TRUE(plan) << plan.error().message;
  EXPECT_EQ(plan->hop_peer_id, "12D3KooWMediaHop");

  auto handoff = BuildAnnounceLiveJoinHandoff(*plan, "account:bob", "account:alice", 1'700'000'000'000, true);
  ASSERT_TRUE(handoff) << handoff.error().message;
  ASSERT_TRUE(handoff->pending.sfu_hint.has_value());
  EXPECT_EQ(*handoff->pending.sfu_hint, "12D3KooWMediaHop");
  ASSERT_TRUE(handoff->session.sfu_hint.has_value());
  EXPECT_EQ(*handoff->session.sfu_hint, "12D3KooWMediaHop");
  EXPECT_EQ(handoff->session.session_kind, CallSessionKind::Broadcast);
  EXPECT_EQ(handoff->pending.session_kind, CallSessionKind::Broadcast);
}

TEST(PeerAnnounceKindTest, LiveChatJsonAndSignRoundTripAdditive) {
  auto keys = MlDsa::GenerateKeyPair();
  ASSERT_TRUE(keys);
  auto topic = MakePeerAnnounceTopicId("12D3KooWPublisherTest", "live");
  ASSERT_TRUE(topic);
  auto tip = SampleTip(*topic);
  tip.kind = kPeerAnnounceKindLiveChat;
  tip.viewer_peer_id = "12D3KooWViewer";
  tip.viewer_msg_id = "msg-1";
  tip.body = "hello overlay";
  auto signed_tip = SignPeerAnnounceTip(tip, keys->secret_key);
  ASSERT_TRUE(signed_tip) << signed_tip.error().message;
  ASSERT_TRUE(VerifyPeerAnnounceTip(*signed_tip, keys->public_key));
  auto json = EncodePeerAnnounceTipJson(*signed_tip);
  ASSERT_TRUE(json);
  auto decoded = DecodePeerAnnounceTipJson(*json);
  ASSERT_TRUE(decoded) << decoded.error().message;
  EXPECT_EQ(decoded->kind, kPeerAnnounceKindLiveChat);
  EXPECT_EQ(decoded->viewer_peer_id, "12D3KooWViewer");
  EXPECT_EQ(decoded->viewer_msg_id, "msg-1");
  ASSERT_TRUE(VerifyPeerAnnounceTip(*decoded, keys->public_key));
}

TEST(PeerAnnounceKindTest, EmptyKindKeepsLegacyCanonical) {
  auto tip = SampleTip("topic");
  tip.kind.clear();
  tip.viewer_peer_id.clear();
  tip.viewer_msg_id.clear();
  const std::string canonical = PeerAnnounceCanonicalSignBytes(tip);
  EXPECT_EQ(canonical.find("kind="), std::string::npos);
  EXPECT_EQ(canonical.find("viewer_peer_id="), std::string::npos);
}

TEST(PeerAnnounceFeedKindTest, LiveChatDoesNotClobberProgramLatest) {
  auto keys = MlDsa::GenerateKeyPair();
  ASSERT_TRUE(keys);
  auto topic = MakePeerAnnounceTopicId("12D3KooWPublisherTest", "live");
  ASSERT_TRUE(topic);
  PeerAnnounceFeed feed(keys->public_key);

  auto program = SampleTip(*topic);
  program.seq = 1;
  program.state = PeerAnnounceState::Live;
  program.join_handle = "session-1";
  auto signed_program = SignPeerAnnounceTip(program, keys->secret_key);
  ASSERT_TRUE(signed_program);
  ASSERT_TRUE(feed.Ingest(*signed_program));

  auto chat = SampleTip(*topic);
  chat.seq = 2;
  chat.kind = kPeerAnnounceKindLiveChat;
  chat.viewer_peer_id = "viewer";
  chat.viewer_msg_id = "m1";
  chat.body = "hi";
  auto signed_chat = SignPeerAnnounceTip(chat, keys->secret_key);
  ASSERT_TRUE(signed_chat);
  ASSERT_TRUE(feed.Ingest(*signed_chat));

  auto latest = feed.Latest(program.peer_id, *topic, program.program_id);
  ASSERT_TRUE(latest);
  EXPECT_TRUE(TipIsProgramKind(*latest));
  EXPECT_EQ(latest->join_handle, "session-1");
  EXPECT_EQ(feed.ListLiveChat(program.peer_id, *topic, program.program_id).size(), 1u);
}

TEST(AnnounceLiveJoinTest, RejectsLiveChatKind) {
  auto tip = SampleTip("topic");
  tip.state = PeerAnnounceState::Live;
  tip.join_handle = "session";
  tip.kind = kPeerAnnounceKindLiveChat;
  tip.viewer_msg_id = "m1";
  EXPECT_FALSE(TipIsLiveJoinable(tip));
  EXPECT_FALSE(PlanAnnounceLiveJoin(tip));
}

TEST(AnnounceOverlayReplyTest, PlansEncodesAndBuildsLiveChatDraft) {
  auto plan = PlanAnnounceOverlayReply("12D3KooWPublisher", "c1", "account:alice", "Alice", "session-1",
                                       "hello", "vm-1");
  ASSERT_TRUE(plan) << plan.error().message;
  EXPECT_EQ(plan->join_handle, "session-1");
  auto decoded = DecodeAnnounceOverlayReplyBody(plan->message_body);
  ASSERT_TRUE(decoded) << decoded.error().message;
  EXPECT_EQ(decoded->text, "hello");

  EXPECT_TRUE(AnnounceOverlayViewerAllowed(0, 1000));
  EXPECT_FALSE(AnnounceOverlayViewerAllowed(1000, 1000 + kAnnounceOverlayViewerMinIntervalMs - 1));
  EXPECT_TRUE(AnnounceOverlayPublisherAllowed(0, 0, 1000));
  EXPECT_FALSE(AnnounceOverlayPublisherAllowed(static_cast<int>(kAnnounceOverlayPublisherMaxPerMinute), 1000,
                                               1000 + 10));

  auto draft = MakeLiveChatAnnounceDraft("topic", "show", "session-1", "viewer-peer", *decoded);
  ASSERT_TRUE(draft) << draft.error().message;
  EXPECT_EQ(draft->kind, kPeerAnnounceKindLiveChat);
  EXPECT_EQ(draft->viewer_msg_id, "vm-1");
  EXPECT_EQ(draft->body, "hello");
}

TEST(AnnounceNotificationInboxTest, UpsertsProgramTipsAndLiveBanner) {
  AnnounceNotificationInbox inbox;
  PeerAnnounceTip tip;
  tip.peer_id = "pub";
  tip.topic_id = "topic";
  tip.program_id = "show";
  tip.state = PeerAnnounceState::Live;
  tip.seq = 1;
  tip.join_handle = "s1";
  EXPECT_TRUE(inbox.UpsertFromTip(tip, 10));
  EXPECT_EQ(inbox.ListActive().size(), 1u);
  EXPECT_EQ(inbox.ListLiveBanners().size(), 1u);

  tip.kind = kPeerAnnounceKindLiveChat;
  tip.viewer_msg_id = "m";
  tip.seq = 2;
  EXPECT_FALSE(inbox.UpsertFromTip(tip, 20));

  tip.kind.clear();
  tip.viewer_msg_id.clear();
  tip.seq = 3;
  tip.body = "still live";
  EXPECT_TRUE(inbox.UpsertFromTip(tip, 30));
  const auto key = AnnounceNotificationInbox::MakeKey(tip);
  EXPECT_TRUE(inbox.DismissBanner(key));
  EXPECT_TRUE(inbox.ListLiveBanners().empty());
  EXPECT_FALSE(inbox.ListActive().empty());
}


TEST(PeerAnnounceL1HopPeerIdsTest, JsonAndSignRoundTripAdditive) {
  auto keys = MlDsa::GenerateKeyPair();
  ASSERT_TRUE(keys);
  auto tip = SampleTip("topic-l1");
  tip.hop_peer_id = "12D3KooWL1a";
  tip.l1_hop_peer_ids = {"12D3KooWL1a", "12D3KooWL1b"};
  auto signed_tip = SignPeerAnnounceTip(tip, keys->secret_key);
  ASSERT_TRUE(signed_tip) << signed_tip.error().message;
  ASSERT_TRUE(VerifyPeerAnnounceTip(*signed_tip, keys->public_key));

  auto json = EncodePeerAnnounceTipJson(*signed_tip);
  ASSERT_TRUE(json);
  auto decoded = DecodePeerAnnounceTipJson(*json);
  ASSERT_TRUE(decoded) << decoded.error().message;
  EXPECT_EQ(decoded->hop_peer_id, "12D3KooWL1a");
  ASSERT_EQ(decoded->l1_hop_peer_ids.size(), 2u);
  EXPECT_EQ(decoded->l1_hop_peer_ids[0], "12D3KooWL1a");
  EXPECT_EQ(decoded->l1_hop_peer_ids[1], "12D3KooWL1b");
  ASSERT_TRUE(VerifyPeerAnnounceTip(*decoded, keys->public_key));

  tip.l1_hop_peer_ids.clear();
  tip.hop_peer_id.clear();
  const std::string canonical = PeerAnnounceCanonicalSignBytes(tip);
  EXPECT_EQ(canonical.find("l1_hop_peer_ids="), std::string::npos);
}

TEST(AnnounceLiveJoinTest, FallsBackHopPeerIdFromL1Hints) {
  auto tip = SampleTip("topic-l1");
  tip.state = PeerAnnounceState::Live;
  tip.join_handle = "session-live-l1";
  tip.hop_peer_id.clear();
  tip.l1_hop_peer_ids = {"12D3KooWL1a", "12D3KooWL1b"};
  auto plan = PlanAnnounceLiveJoin(tip);
  ASSERT_TRUE(plan) << plan.error().message;
  EXPECT_EQ(plan->hop_peer_id, "12D3KooWL1a");
  ASSERT_EQ(plan->l1_hop_peer_ids.size(), 2u);
}

} // namespace pbr
