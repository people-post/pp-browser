#include "feature/conversations/AmpPeerAnnounceTransport.h"

#include "domain/messaging/PeerAnnounceCodec.h"
#include "domain/messaging/PeerAnnounceKeyResolve.h"
#include "domain/messaging/PeerAnnouncePublisher.h"
#include "domain/messaging/PeerSigningKeyStore.h"
#include "domain/mesh/tests/support/mesh_test_harness.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/identity/PeerIdUtil.h"

#include "common/directory/DirectoryJson.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

class AmpPeerAnnounceTransportTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto created = pbr::test::AmpMeshHarness::Create();
    ASSERT_TRUE(static_cast<bool>(created));
    harness_ = std::move(*created);

    ASSERT_TRUE(static_cast<bool>(harness_->chat_a().RegisterEndpoint("b", harness_->ma_b)));
    ASSERT_TRUE(static_cast<bool>(harness_->chat_b().RegisterEndpoint("a", harness_->ma_a)));

    auto keys_a = MlDsa::GenerateKeyPair();
    auto keys_b = MlDsa::GenerateKeyPair();
    ASSERT_TRUE(keys_a);
    ASSERT_TRUE(keys_b);
    pk_a_ = keys_a->public_key;
    pk_b_ = keys_b->public_key;
    sk_a_ = keys_a->secret_key;

    auto resolve = [this](const std::string& peer_id) -> std::optional<std::vector<uint8_t>> {
      if (peer_id == "publisher-a") {
        return pk_a_;
      }
      if (peer_id == "publisher-b") {
        return pk_b_;
      }
      return std::nullopt;
    };

    AmpPeerAnnounceTransport::WorkerPost no_worker;
    a_svc_ = std::make_unique<AmpPeerAnnounceTransport>(
        harness_->chat_a(), feed_a_, [this] { harness_->PumpBoth(); }, no_worker, resolve);
    b_svc_ = std::make_unique<AmpPeerAnnounceTransport>(
        harness_->chat_b(), feed_b_, [this] { harness_->PumpBoth(); }, no_worker, resolve);
    a_svc_->Start();
    b_svc_->Start();
  }

  void TearDown() override {
    if (a_svc_) {
      a_svc_->Stop();
    }
    if (b_svc_) {
      b_svc_->Stop();
    }
    a_svc_.reset();
    b_svc_.reset();
    harness_.reset();
  }

  std::unique_ptr<pbr::test::AmpMeshHarness> harness_;
  PeerAnnounceFeed feed_a_;
  PeerAnnounceFeed feed_b_;
  std::unique_ptr<AmpPeerAnnounceTransport> a_svc_;
  std::unique_ptr<AmpPeerAnnounceTransport> b_svc_;
  std::vector<uint8_t> pk_a_;
  std::vector<uint8_t> pk_b_;
  std::vector<uint8_t> sk_a_;
};

TEST_F(AmpPeerAnnounceTransportTest, PushTipRoundTripIngestsOnPeer) {
  auto topic = MakePeerAnnounceTopicId("publisher-a", "live");
  ASSERT_TRUE(topic);

  PeerAnnouncePublisher publisher("publisher-a", sk_a_);
  PeerAnnouncePublisher::Draft go_live;
  go_live.topic_id = *topic;
  go_live.program_id = "show-1";
  go_live.state = PeerAnnounceState::Live;
  go_live.join_handle = "session-1";
  go_live.body = "now live";
  auto tip = publisher.Publish(go_live, 1'000);
  ASSERT_TRUE(tip) << tip.error().message;

  auto ack = a_svc_->PushTip("b", *tip);
  ASSERT_TRUE(ack) << ack.error().message;
  EXPECT_TRUE(ack->ok) << ack->error;
  EXPECT_EQ(ack->seq, tip->seq);
  EXPECT_EQ(ack->epoch, tip->epoch);

  auto latest = feed_b_.Latest("publisher-a", *topic, "show-1");
  ASSERT_TRUE(latest);
  EXPECT_EQ(latest->body, "now live");
  EXPECT_EQ(latest->signature_b64, tip->signature_b64);
}

TEST_F(AmpPeerAnnounceTransportTest, PushTipFailsAckWhenPublisherKeyUnknown) {
  auto topic = MakePeerAnnounceTopicId("unknown-publisher", "live");
  ASSERT_TRUE(topic);

  PeerAnnouncePublisher publisher("unknown-publisher", sk_a_);
  PeerAnnouncePublisher::Draft go_live;
  go_live.topic_id = *topic;
  go_live.program_id = "show-1";
  go_live.state = PeerAnnounceState::Live;
  go_live.join_handle = "session-1";
  auto tip = publisher.Publish(go_live, 1'000);
  ASSERT_TRUE(tip) << tip.error().message;

  auto ack = a_svc_->PushTip("b", *tip);
  ASSERT_TRUE(ack) << ack.error().message;
  EXPECT_FALSE(ack->ok);
  EXPECT_EQ(ack->error, "unknown publisher key");
  EXPECT_FALSE(feed_b_.Latest("unknown-publisher", *topic, "show-1"));
}

TEST_F(AmpPeerAnnounceTransportTest, PushTipRoundTripWithStoreBackedResolver) {
  auto keys = MlDsa::GenerateKeyPair();
  ASSERT_TRUE(keys);
  auto peer_id = PeerIdFromMlDsaPublicKey(keys->public_key);
  ASSERT_TRUE(peer_id);

  PeerSigningKeyStore store;
  PeerSigningKeyRecord record;
  record.signing_public_key_b64 = Base64Encode(keys->public_key);
  record.source = "test";
  store.Put(ContactIdKindToString(ContactIdKind::PeerId), *peer_id, record);

  b_svc_->SetPublisherKeyResolver(
      [&store](const std::string& tip_peer_id) -> std::optional<std::vector<uint8_t>> {
        return ResolvePeerAnnouncePublisherKey(tip_peer_id, "", {}, store);
      });

  auto topic = MakePeerAnnounceTopicId(*peer_id, "live");
  ASSERT_TRUE(topic);
  PeerAnnouncePublisher publisher(*peer_id, keys->secret_key);
  PeerAnnouncePublisher::Draft go_live;
  go_live.topic_id = *topic;
  go_live.program_id = "show-2";
  go_live.state = PeerAnnounceState::Live;
  go_live.join_handle = "session-2";
  go_live.body = "store resolve";
  auto tip = publisher.Publish(go_live, 2'000);
  ASSERT_TRUE(tip) << tip.error().message;

  auto ack = a_svc_->PushTip("b", *tip);
  ASSERT_TRUE(ack) << ack.error().message;
  EXPECT_TRUE(ack->ok) << ack->error;

  auto latest = feed_b_.Latest(*peer_id, *topic, "show-2");
  ASSERT_TRUE(latest);
  EXPECT_EQ(latest->body, "store resolve");
}

} // namespace
} // namespace pbr
