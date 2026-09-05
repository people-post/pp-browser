#include "domain/messaging/PeerAnnounceCodec.h"
#include "domain/messaging/PeerAnnounceFeed.h"

#include "foundation/crypto/CryptoUtil.h"

#include <sodium.h>
#include <gtest/gtest.h>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

struct Ed25519Pair {
  std::vector<uint8_t> pk;
  std::vector<uint8_t> sk;
};

Ed25519Pair MakeKeyPair() {
  EnsureSodiumInit();
  Ed25519Pair pair;
  pair.pk.resize(crypto_sign_PUBLICKEYBYTES);
  pair.sk.resize(crypto_sign_SECRETKEYBYTES);
  crypto_sign_keypair(pair.pk.data(), pair.sk.data());
  return pair;
}

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
  const auto keys = MakeKeyPair();
  auto topic = MakePeerAnnounceTopicId("12D3KooWPublisherTest", "live");
  ASSERT_TRUE(topic);
  auto tip = SampleTip(*topic);
  auto signed_tip = SignPeerAnnounceTip(tip, keys.sk);
  ASSERT_TRUE(signed_tip) << signed_tip.error().message;
  ASSERT_TRUE(VerifyPeerAnnounceTip(*signed_tip, keys.pk));

  auto json = EncodePeerAnnounceTipJson(*signed_tip);
  ASSERT_TRUE(json);
  auto decoded = DecodePeerAnnounceTipJson(*json);
  ASSERT_TRUE(decoded) << decoded.error().message;
  EXPECT_EQ(decoded->peer_id, signed_tip->peer_id);
  EXPECT_EQ(decoded->signature_b64, signed_tip->signature_b64);
  ASSERT_TRUE(VerifyPeerAnnounceTip(*decoded, keys.pk));
}

TEST(PeerAnnounceTest, FeedRejectsStaleAndBadSignature) {
  const auto keys = MakeKeyPair();
  auto topic = MakePeerAnnounceTopicId("12D3KooWPublisherTest", "live");
  ASSERT_TRUE(topic);

  PeerAnnounceFeed feed(keys.pk);
  auto tip1 = SampleTip(*topic);
  tip1.seq = 1;
  auto signed1 = SignPeerAnnounceTip(tip1, keys.sk);
  ASSERT_TRUE(signed1);
  ASSERT_TRUE(feed.Ingest(*signed1));

  auto tip0 = SampleTip(*topic);
  tip0.seq = 0;
  auto signed0 = SignPeerAnnounceTip(tip0, keys.sk);
  ASSERT_TRUE(signed0);
  EXPECT_FALSE(feed.Ingest(*signed0));

  const auto other = MakeKeyPair();
  auto tip2 = SampleTip(*topic);
  tip2.seq = 2;
  auto forged = SignPeerAnnounceTip(tip2, other.sk);
  ASSERT_TRUE(forged);
  EXPECT_FALSE(feed.Ingest(*forged));

  auto signed2 = SignPeerAnnounceTip(tip2, keys.sk);
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

} // namespace pbr
