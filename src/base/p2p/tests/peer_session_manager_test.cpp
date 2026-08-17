#include "base/p2p/Libp2pHost.h"
#include "base/p2p/PeerSessionManager.h"
#include "base/p2p/tests/libp2p_ephemeral_listen.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

namespace pbr {
namespace {

class PeerSessionManagerTest : public ::testing::Test {
protected:
  void SetUp() override {
    PeerSessionConfig config;
    config.max_connections = 4;
    config.max_concurrent_dials = 2;
    config.idle_ttl = std::chrono::milliseconds(50);
    config.dial_failure_backoff = std::chrono::milliseconds(100);
    config.dial_timeout = std::chrono::milliseconds(500);

    int listen_port = 0;
    auto started = test::StartEphemeralLoopbackHost(host_, listen_port);
    ASSERT_TRUE(started) << started.error().message;
    sessions_ = std::make_unique<PeerSessionManager>(host_, config);
  }

  void TearDown() override {
    sessions_.reset();
    host_.Stop();
  }

  Libp2pHost host_;
  std::unique_ptr<PeerSessionManager> sessions_;
};

TEST_F(PeerSessionManagerTest, RegisterEndpointRequiresP2pComponent) {
  EXPECT_FALSE(sessions_->RegisterEndpoint("relay:alice", "/ip4/127.0.0.1/tcp/4001"));
  EXPECT_FALSE(sessions_->IsDialable("relay:alice"));
}

TEST_F(PeerSessionManagerTest, RegisterEndpointMakesDialable) {
  auto peer_id = host_.LocalPeerIdBase58();
  ASSERT_TRUE(peer_id);
  const std::string ma = "/ip4/127.0.0.1/tcp/40199/p2p/" + *peer_id;
  ASSERT_TRUE(sessions_->RegisterEndpoint("relay:bob", ma));
  EXPECT_TRUE(sessions_->IsDialable("relay:bob"));
  EXPECT_EQ(sessions_->RegisteredEndpointCount(), 1u);
  EXPECT_FALSE(sessions_->IsConnected("relay:bob"));
}

TEST_F(PeerSessionManagerTest, WarmAndClear) {
  auto peer_id = host_.LocalPeerIdBase58();
  ASSERT_TRUE(peer_id);
  const std::string ma = "/ip4/127.0.0.1/tcp/40198/p2p/" + *peer_id;
  ASSERT_TRUE(sessions_->RegisterEndpoint("relay:carol", ma));
  EXPECT_EQ(sessions_->WarmPeerCount(), 0u);
  sessions_->MarkWarm("relay:carol");
  EXPECT_EQ(sessions_->WarmPeerCount(), 1u);
  sessions_->ClearWarm("relay:carol");
  EXPECT_EQ(sessions_->WarmPeerCount(), 0u);
  sessions_->MarkWarm("relay:carol");
  sessions_->ClearAllWarm();
  EXPECT_EQ(sessions_->WarmPeerCount(), 0u);
}

TEST_F(PeerSessionManagerTest, LinkSnapshotAndBackoff) {
  // Lengthen backoff for this case so GetLinkSnapshot cannot race past expiry on
  // slow CI hosts (SetUp default is 100ms, which flakes when dial takes longer).
  PeerSessionConfig config = sessions_->GetConfig();
  config.dial_failure_backoff = std::chrono::milliseconds(5000);
  sessions_->SetConfig(config);

  auto peer_id = host_.LocalPeerIdBase58();
  ASSERT_TRUE(peer_id);
  // Unreachable port — connection refused should fail quickly into backoff.
  const std::string ma = "/ip4/127.0.0.1/tcp/1/p2p/" + *peer_id;
  ASSERT_TRUE(sessions_->RegisterEndpoint("relay:dave", ma));

  PeerLinkSnapshot snap = sessions_->GetLinkSnapshot("relay:dave");
  EXPECT_TRUE(snap.host_running);
  EXPECT_TRUE(snap.has_endpoint);
  EXPECT_EQ(snap.phase, PeerLinkPhase::Idle);
  EXPECT_FALSE(sessions_->IsDialing("relay:dave"));

  EXPECT_EQ(sessions_->GetLinkSnapshot("relay:missing").phase, PeerLinkPhase::Unavailable);
  EXPECT_EQ(PeerDialErrorUserCopy("libp2p dial failed"),
            "Peer didn't answer — they may be offline or the address may be wrong.");

  std::atomic<bool> done{false};
  sessions_->EnsureConnection("relay:dave", [&](Roe<void> result) {
    EXPECT_FALSE(result);
    done = true;
  });
  for (int i = 0; i < 250 && !done.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  ASSERT_TRUE(done.load());
  snap = sessions_->GetLinkSnapshot("relay:dave");
  EXPECT_EQ(snap.phase, PeerLinkPhase::Backoff);
  EXPECT_FALSE(sessions_->IsDialable("relay:dave"));
  EXPECT_GE(snap.backoff_remaining.count(), 1000);
  EXPECT_FALSE(PeerDialErrorUserCopy(snap.detail).empty());

  sessions_->ClearDialBackoff("relay:dave");
  EXPECT_TRUE(sessions_->IsDialable("relay:dave"));
  EXPECT_EQ(sessions_->GetLinkSnapshot("relay:dave").phase, PeerLinkPhase::Idle);
}

TEST_F(PeerSessionManagerTest, HostExposesLocalPeerId) {
  auto peer_id = host_.LocalPeerIdBase58();
  ASSERT_TRUE(peer_id);
  EXPECT_FALSE(peer_id->empty());
  EXPECT_TRUE(host_.IsRunning());
}

TEST_F(PeerSessionManagerTest, RawPeerIdHydratesFromAddressBook) {
  auto remote = host_.LocalPeerIdBase58();
  ASSERT_TRUE(remote);
  const std::string ma = "/ip4/203.0.113.20/tcp/4001/p2p/" + *remote;

  ASSERT_TRUE(sessions_->RegisterEndpoint("relay:remote", ma));
  EXPECT_TRUE(sessions_->IsDialable("relay:remote"));
  EXPECT_TRUE(sessions_->PreferredPeerMultiaddr("relay:remote"));
  EXPECT_EQ(*sessions_->PreferredPeerMultiaddr("relay:remote"), ma);

  EXPECT_TRUE(sessions_->IsDialable(*remote));
  ASSERT_TRUE(sessions_->PreferredPeerMultiaddr(*remote));
  EXPECT_EQ(*sessions_->PreferredPeerMultiaddr(*remote), ma);
}

} // namespace
} // namespace pbr
