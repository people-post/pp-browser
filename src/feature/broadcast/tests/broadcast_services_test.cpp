// The live-broadcast services a node runs on the mesh: publish a tip, push it, the follower holds it.
#include "feature/broadcast/BroadcastServices.h"

#include "lib/broadcast/PeerAnnounceCodec.h"
#include "foundation/crypto/MlDsa.h"
#include "mesh/testing/mesh_test_harness.h"

#include <gtest/gtest.h>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {
namespace {

class BroadcastServicesTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto created = mesh::test::AmpMeshHarness::Create();
    ASSERT_TRUE(static_cast<bool>(created));
    harness_ = std::move(*created);
    ASSERT_TRUE(static_cast<bool>(harness_->chat_a().RegisterEndpoint("b", harness_->ma_b)));
    ASSERT_TRUE(static_cast<bool>(harness_->chat_b().RegisterEndpoint("a", harness_->ma_a)));
    auto keys = MlDsa::GenerateKeyPair();
    ASSERT_TRUE(keys);
    keys_ = *keys;
  }

  void TearDown() override {
    a_.reset();
    b_.reset();
    harness_.reset();
  }

  std::unique_ptr<BroadcastServices> Make(bool a_side, bool can_publish) {
    BroadcastServicesDeps deps;
    deps.links = a_side ? &harness_->chat_a() : &harness_->chat_b();
    deps.io.io_pump = [this] { harness_->PumpBoth(); };
    deps.io.post_io = a_side ? harness_->MakePostIoA() : harness_->MakePostIoB();
    deps.io.post_after = a_side ? harness_->MakePostAfterA() : harness_->MakePostAfterB();
    deps.publisher_key = [this](const std::string& peer_id) -> std::optional<std::vector<uint8_t>> {
      return peer_id == "publisher-a" ? std::optional<std::vector<uint8_t>>(keys_.public_key) : std::nullopt;
    };
    if (can_publish) {
      deps.local_peer_id = "publisher-a";
      deps.device_secret = [this]() -> std::optional<std::vector<uint8_t>> { return keys_.secret_key; };
    }
    return std::make_unique<BroadcastServices>(std::move(deps));
  }

  static PeerAnnouncePublisher::Draft GoLive(const std::string& topic) {
    PeerAnnouncePublisher::Draft draft;
    draft.topic_id = topic;
    draft.program_id = "show-1";
    draft.state = PeerAnnounceState::Live;
    draft.join_handle = "session-1";
    draft.body = "now live";
    return draft;
  }

  std::unique_ptr<mesh::test::AmpMeshHarness> harness_;
  MlDsaKeyPair keys_;
  std::unique_ptr<BroadcastServices> a_;
  std::unique_ptr<BroadcastServices> b_;
};

TEST_F(BroadcastServicesTest, APushedTipReachesTheFollowersFeedAndNotifications) {
  a_ = Make(true, true);
  b_ = Make(false, false);
  auto topic = MakePeerAnnounceTopicId("publisher-a", "live");
  ASSERT_TRUE(topic);

  auto ack = a_->PublishAndPush("b", GoLive(*topic), 1'000);
  ASSERT_TRUE(ack) << ack.error().message;
  EXPECT_TRUE(ack->ok) << ack->error;

  auto mine = a_->LatestTip("publisher-a", *topic, "show-1");
  ASSERT_TRUE(mine) << "the publisher's own feed holds it";
  auto theirs = b_->LatestTip("publisher-a", *topic, "show-1");
  ASSERT_TRUE(theirs);
  EXPECT_EQ(theirs->signature_b64, mine->signature_b64);
  EXPECT_EQ(b_->Notifications().Size(), 1u) << "a live program is news for the follower";
}

TEST_F(BroadcastServicesTest, WithoutADeviceIdentityItCannotPublish) {
  b_ = Make(false, false);
  auto topic = MakePeerAnnounceTopicId("publisher-a", "live");
  ASSERT_TRUE(topic);
  EXPECT_FALSE(b_->PublishTip(GoLive(*topic)));
  EXPECT_FALSE(b_->PublishAndPush("a", GoLive(*topic), 1'000));
}

} // namespace
} // namespace pbr
