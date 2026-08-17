#include "base/p2p/DialBackService.h"
#include "base/p2p/Libp2pHost.h"
#include "base/p2p/PeerSessionManager.h"
#include "base/p2p/tests/libp2p_ephemeral_listen.h"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <string>
#include <thread>

namespace pbr {
namespace {

class DialBackServiceTest : public ::testing::Test {
protected:
  void SetUp() override {
    PeerSessionConfig config;
    config.dial_timeout = std::chrono::milliseconds(3000);
    config.dial_failure_backoff = std::chrono::milliseconds(100);

    auto seed_started = test::StartEphemeralLoopbackHost(seed_host_, seed_port_);
    ASSERT_TRUE(seed_started) << seed_started.error().message;
    seed_sessions_ = std::make_unique<PeerSessionManager>(seed_host_, config);
    seed_dial_back_ = std::make_unique<DialBackService>(seed_host_, *seed_sessions_);
    seed_dial_back_->Start();

    auto client_started = test::StartEphemeralLoopbackHost(client_host_, client_port_);
    ASSERT_TRUE(client_started) << client_started.error().message;
    client_sessions_ = std::make_unique<PeerSessionManager>(client_host_, config);
    client_dial_back_ = std::make_unique<DialBackService>(client_host_, *client_sessions_);
    client_dial_back_->Start();
  }

  void TearDown() override {
    client_dial_back_.reset();
    seed_dial_back_.reset();
    client_sessions_.reset();
    seed_sessions_.reset();
    client_host_.Stop();
    seed_host_.Stop();
  }

  int seed_port_ = 0;
  int client_port_ = 0;
  Libp2pHost seed_host_;
  Libp2pHost client_host_;
  std::unique_ptr<PeerSessionManager> seed_sessions_;
  std::unique_ptr<PeerSessionManager> client_sessions_;
  std::unique_ptr<DialBackService> seed_dial_back_;
  std::unique_ptr<DialBackService> client_dial_back_;
};

TEST_F(DialBackServiceTest, SeedCanDialBackClientListen) {
  auto seed_id = seed_host_.LocalPeerIdBase58();
  auto client_id = client_host_.LocalPeerIdBase58();
  ASSERT_TRUE(seed_id);
  ASSERT_TRUE(client_id);

  const std::string seed_ma = "/ip4/127.0.0.1/tcp/" + std::to_string(seed_port_) + "/p2p/" + *seed_id;
  const std::string client_ma =
      "/ip4/127.0.0.1/tcp/" + std::to_string(client_port_) + "/p2p/" + *client_id;

  ASSERT_TRUE(client_sessions_->RegisterEndpoint("seed", seed_ma));

  auto probed = client_dial_back_->Probe("seed", {client_ma}, 5000);
  ASSERT_TRUE(probed) << probed.error().message;
  EXPECT_TRUE(probed->ok) << probed->error;
  EXPECT_EQ(probed->dialed, client_ma);
}

TEST_F(DialBackServiceTest, ProbeFailsForUnreachableTarget) {
  auto seed_id = seed_host_.LocalPeerIdBase58();
  ASSERT_TRUE(seed_id);

  const std::string seed_ma = "/ip4/127.0.0.1/tcp/" + std::to_string(seed_port_) + "/p2p/" + *seed_id;
  ASSERT_TRUE(client_sessions_->RegisterEndpoint("seed", seed_ma));

  // Use a PeerId that is not already connected (reuse would false-positive on port 1).
  const std::string bad =
      "/ip4/127.0.0.1/tcp/1/p2p/12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR";
  auto probed = client_dial_back_->Probe("seed", {bad}, 2000);
  ASSERT_TRUE(probed) << probed.error().message;
  EXPECT_FALSE(probed->ok);
}

TEST_F(DialBackServiceTest, ConcurrentProbesFromTwoClients) {
  PeerSessionConfig config;
  config.dial_timeout = std::chrono::milliseconds(3000);
  config.dial_failure_backoff = std::chrono::milliseconds(100);

  Libp2pHost client_b_host;
  int client_b_port = 0;
  auto client_b_started = test::StartEphemeralLoopbackHost(client_b_host, client_b_port);
  ASSERT_TRUE(client_b_started) << client_b_started.error().message;
  auto client_b_sessions = std::make_unique<PeerSessionManager>(client_b_host, config);
  auto client_b_dial_back = std::make_unique<DialBackService>(client_b_host, *client_b_sessions);
  client_b_dial_back->Start();

  auto seed_id = seed_host_.LocalPeerIdBase58();
  auto client_a_id = client_host_.LocalPeerIdBase58();
  auto client_b_id = client_b_host.LocalPeerIdBase58();
  ASSERT_TRUE(seed_id);
  ASSERT_TRUE(client_a_id);
  ASSERT_TRUE(client_b_id);

  const std::string seed_ma = "/ip4/127.0.0.1/tcp/" + std::to_string(seed_port_) + "/p2p/" + *seed_id;
  const std::string client_a_ma =
      "/ip4/127.0.0.1/tcp/" + std::to_string(client_port_) + "/p2p/" + *client_a_id;
  const std::string client_b_ma =
      "/ip4/127.0.0.1/tcp/" + std::to_string(client_b_port) + "/p2p/" + *client_b_id;

  ASSERT_TRUE(client_sessions_->RegisterEndpoint("seed", seed_ma));
  ASSERT_TRUE(client_b_sessions->RegisterEndpoint("seed", seed_ma));

  auto probe_a = std::async(std::launch::async, [&] {
    return client_dial_back_->Probe("seed", {client_a_ma}, 5000);
  });
  auto probe_b = std::async(std::launch::async, [&] {
    return client_b_dial_back->Probe("seed", {client_b_ma}, 5000);
  });

  auto result_a = probe_a.get();
  auto result_b = probe_b.get();
  ASSERT_TRUE(result_a) << result_a.error().message;
  ASSERT_TRUE(result_b) << result_b.error().message;
  EXPECT_TRUE(result_a->ok) << result_a->error;
  EXPECT_TRUE(result_b->ok) << result_b->error;
  EXPECT_EQ(result_a->dialed, client_a_ma);
  EXPECT_EQ(result_b->dialed, client_b_ma);

  client_b_dial_back.reset();
  client_b_sessions.reset();
  client_b_host.Stop();
}

} // namespace
} // namespace pbr
