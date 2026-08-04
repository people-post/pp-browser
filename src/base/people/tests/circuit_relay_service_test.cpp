#include "libp2p/integration/host/CircuitRelayService.h"
#include "libp2p/integration/host/Libp2pHost.h"
#include "libp2p/integration/host/Libp2pWorker.h"
#include "libp2p/integration/host/PeerSessionManager.h"
#include "libp2p/integration/host/StreamFrameIo.h"

#include "base/people/RelayScope.h"

#include <gtest/gtest.h>

#include <libp2p/connection/stream.hpp>
#include <libp2p/host/host.hpp>
#include <libp2p/peer/protocol.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
static int ProcessId() { return _getpid(); }
#else
#include <unistd.h>
static int ProcessId() { return static_cast<int>(getpid()); }
#endif

namespace pbr {
namespace {

using libp2p::connection::Stream;
using libp2p::peer::ProtocolName;

inline constexpr const char* kBridgeTargetProtocol = "/pp-browser/circuit-relay-bridge-test/1.0.0";

template <typename Result>
Result RunOnWorker(Libp2pHost& host, std::function<Result()> work) {
  std::promise<Result> promise;
  auto future = promise.get_future();
  PostLibp2pWorker(host, WorkerLane::Normal, [&] { promise.set_value(work()); });
  return future.get();
}

class CircuitRelayServiceTest : public ::testing::Test {
protected:
  void SetUp() override {
    static std::atomic<int> port{46000 + (ProcessId() % 2000) * 10};
    relay_port_ = port.fetch_add(1);
    client_port_ = port.fetch_add(1);
    target_port_ = port.fetch_add(1);
    stranger_port_ = port.fetch_add(1);

    PeerSessionConfig config;
    config.dial_timeout = std::chrono::milliseconds(3000);
    config.dial_failure_backoff = std::chrono::milliseconds(100);

    auto start_host = [&](Libp2pHost& host, int port) {
      Libp2pHostConfig cfg;
      cfg.listen_multiaddr = "/ip4/127.0.0.1/tcp/" + std::to_string(port);
      return host.Start(cfg);
    };

    ASSERT_TRUE(start_host(relay_host_, relay_port_));
    ASSERT_TRUE(start_host(client_host_, client_port_));
    ASSERT_TRUE(start_host(target_host_, target_port_));
    ASSERT_TRUE(start_host(stranger_host_, stranger_port_));

    relay_sessions_ = std::make_unique<PeerSessionManager>(relay_host_, config);
    client_sessions_ = std::make_unique<PeerSessionManager>(client_host_, config);
    stranger_sessions_ = std::make_unique<PeerSessionManager>(stranger_host_, config);

    relay_service_ = std::make_unique<CircuitRelayService>(relay_host_, *relay_sessions_);
    client_service_ = std::make_unique<CircuitRelayService>(client_host_, *client_sessions_);
    stranger_service_ = std::make_unique<CircuitRelayService>(stranger_host_, *stranger_sessions_);

    relay_service_->Start();

    auto relay_id = relay_host_.LocalPeerIdBase58();
    auto client_id = client_host_.LocalPeerIdBase58();
    auto target_id = target_host_.LocalPeerIdBase58();
    auto stranger_id = stranger_host_.LocalPeerIdBase58();
    ASSERT_TRUE(relay_id);
    ASSERT_TRUE(client_id);
    ASSERT_TRUE(target_id);
    ASSERT_TRUE(stranger_id);

    relay_ma_ = "/ip4/127.0.0.1/tcp/" + std::to_string(relay_port_) + "/p2p/" + *relay_id;
    client_ma_ = "/ip4/127.0.0.1/tcp/" + std::to_string(client_port_) + "/p2p/" + *client_id;
    target_ma_ = "/ip4/127.0.0.1/tcp/" + std::to_string(target_port_) + "/p2p/" + *target_id;
    stranger_ma_ = "/ip4/127.0.0.1/tcp/" + std::to_string(stranger_port_) + "/p2p/" + *stranger_id;

    ASSERT_TRUE(client_sessions_->RegisterEndpoint("relay", relay_ma_));
    ASSERT_TRUE(stranger_sessions_->RegisterEndpoint("relay", relay_ma_));
  }

  void TearDown() override {
    stranger_service_.reset();
    client_service_.reset();
    relay_service_.reset();
    stranger_sessions_.reset();
    client_sessions_.reset();
    relay_sessions_.reset();
    stranger_host_.Stop();
    target_host_.Stop();
    client_host_.Stop();
    relay_host_.Stop();
  }

  void ArmTargetReader() {
    target_host_.GetHost().setProtocolHandler(
        {ProtocolName{kBridgeTargetProtocol}},
        [this](libp2p::StreamAndProtocol stream_in) {
          auto stream = std::move(stream_in.stream);
          target_host_.Post([this, stream = std::move(stream)]() mutable {
            auto reader = std::make_shared<AsyncLengthPrefixedReader>();
            reader->Start(
                stream,
                [this](Roe<std::vector<uint8_t>> frame) {
                  if (!frame) {
                    return;
                  }
                  std::lock_guard lock(target_mu_);
                  target_received_ = *frame;
                  target_got_ = true;
                  target_cv_.notify_one();
                },
                [] { return false; });
          });
        });
  }

  int relay_port_ = 0;
  int client_port_ = 0;
  int target_port_ = 0;
  int stranger_port_ = 0;
  std::string relay_ma_;
  std::string client_ma_;
  std::string target_ma_;
  std::string stranger_ma_;
  Libp2pHost relay_host_;
  Libp2pHost client_host_;
  Libp2pHost target_host_;
  Libp2pHost stranger_host_;
  std::unique_ptr<PeerSessionManager> relay_sessions_;
  std::unique_ptr<PeerSessionManager> client_sessions_;
  std::unique_ptr<PeerSessionManager> stranger_sessions_;
  std::unique_ptr<CircuitRelayService> relay_service_;
  std::unique_ptr<CircuitRelayService> client_service_;
  std::unique_ptr<CircuitRelayService> stranger_service_;
  std::mutex target_mu_;
  std::condition_variable target_cv_;
  bool target_got_ = false;
  std::vector<uint8_t> target_received_;
};

TEST_F(CircuitRelayServiceTest, BridgeForwardsPayload) {
  ArmTargetReader();

  CircuitBridgeTarget target;
  target.target_multiaddr = target_ma_;
  target.target_protocol = kBridgeTargetProtocol;

  auto bridged = client_service_->RequestBridge("relay", target, 8000);
  ASSERT_TRUE(bridged) << bridged.error().message;
  ASSERT_TRUE(bridged->ok) << bridged->error;
  ASSERT_TRUE(bridged->stream);

  const std::vector<uint8_t> payload = {'c', 'i', 'r', 'c', 'u', 'i', 't'};
  auto write_result = RunOnWorker<Roe<void>>(
      client_host_, [&] { return BlockingWriteLengthPrefixedFrame(bridged->stream, payload); });
  ASSERT_TRUE(write_result) << write_result.error().message;

  {
    std::unique_lock lock(target_mu_);
    ASSERT_TRUE(target_cv_.wait_for(lock, std::chrono::seconds(3), [this] { return target_got_; }));
  }
  EXPECT_EQ(target_received_, payload);
}

TEST_F(CircuitRelayServiceTest, StrangerRefusedWhenContactsOnly) {
  auto client_id = client_host_.LocalPeerIdBase58();
  ASSERT_TRUE(client_id);

  CircuitRelayAdmissionPolicy policy;
  policy.prefer_contacts_only = true;
  policy.serve_scope_mask = kRelayScopeLinkSiteSocial;
  policy.contact_peer_ids = {*client_id};
  relay_service_->SetAdmissionPolicy(std::move(policy));

  CircuitBridgeTarget target;
  target.target_multiaddr = target_ma_;
  target.target_protocol = kBridgeTargetProtocol;

  auto bridged = stranger_service_->RequestBridge("relay", target, 5000);
  ASSERT_TRUE(bridged) << bridged.error().message;
  EXPECT_FALSE(bridged->ok);
  EXPECT_NE(bridged->error.find("stranger refused"), std::string::npos);
}

TEST_F(CircuitRelayServiceTest, ConcurrentBridges) {
  static std::atomic<int> extra_port{47000 + (ProcessId() % 2000) * 10};
  const int target2_port = extra_port.fetch_add(1);

  Libp2pHost target2_host;
  PeerSessionConfig config;
  config.dial_timeout = std::chrono::milliseconds(3000);
  config.dial_failure_backoff = std::chrono::milliseconds(100);
  Libp2pHostConfig target2_cfg;
  target2_cfg.listen_multiaddr = "/ip4/127.0.0.1/tcp/" + std::to_string(target2_port);
  ASSERT_TRUE(target2_host.Start(target2_cfg));
  auto target2_id = target2_host.LocalPeerIdBase58();
  ASSERT_TRUE(target2_id);
  const std::string target2_ma =
      "/ip4/127.0.0.1/tcp/" + std::to_string(target2_port) + "/p2p/" + *target2_id;

  std::mutex mu1;
  std::mutex mu2;
  std::condition_variable cv1;
  std::condition_variable cv2;
  bool got1 = false;
  bool got2 = false;
  std::vector<uint8_t> payload1;
  std::vector<uint8_t> payload2;

  auto arm_reader = [](Libp2pHost& host, std::mutex& mu, std::condition_variable& cv, bool& got,
                       std::vector<uint8_t>& out) {
    host.GetHost().setProtocolHandler(
        {ProtocolName{kBridgeTargetProtocol}},
        [&](libp2p::StreamAndProtocol stream_in) {
          auto stream = std::move(stream_in.stream);
          host.Post([&, stream = std::move(stream)]() mutable {
            auto reader = std::make_shared<AsyncLengthPrefixedReader>();
            reader->Start(
                stream,
                [&](Roe<std::vector<uint8_t>> frame) {
                  if (!frame) {
                    return;
                  }
                  std::lock_guard lock(mu);
                  out = *frame;
                  got = true;
                  cv.notify_one();
                },
                [] { return false; });
          });
        });
  };

  arm_reader(target_host_, mu1, cv1, got1, payload1);
  arm_reader(target2_host, mu2, cv2, got2, payload2);

  CircuitBridgeTarget target1;
  target1.target_multiaddr = target_ma_;
  target1.target_protocol = kBridgeTargetProtocol;
  CircuitBridgeTarget target2;
  target2.target_multiaddr = target2_ma;
  target2.target_protocol = kBridgeTargetProtocol;

  auto bridge1 = client_service_->RequestBridge("relay", target1, 8000);
  auto bridge2 = client_service_->RequestBridge("relay", target2, 8000);
  ASSERT_TRUE(bridge1) << bridge1.error().message;
  ASSERT_TRUE(bridge2) << bridge2.error().message;
  ASSERT_TRUE(bridge1->ok) << bridge1->error;
  ASSERT_TRUE(bridge2->ok) << bridge2->error;

  const std::vector<uint8_t> send1 = {'o', 'n', 'e'};
  const std::vector<uint8_t> send2 = {'t', 'w', 'o'};
  ASSERT_TRUE(RunOnWorker<Roe<void>>(client_host_, [&] {
    return BlockingWriteLengthPrefixedFrame(bridge1->stream, send1);
  }));
  ASSERT_TRUE(RunOnWorker<Roe<void>>(client_host_, [&] {
    return BlockingWriteLengthPrefixedFrame(bridge2->stream, send2);
  }));

  {
    std::unique_lock lock(mu1);
    ASSERT_TRUE(cv1.wait_for(lock, std::chrono::seconds(3), [&] { return got1; }));
  }
  {
    std::unique_lock lock(mu2);
    ASSERT_TRUE(cv2.wait_for(lock, std::chrono::seconds(3), [&] { return got2; }));
  }
  EXPECT_EQ(payload1, send1);
  EXPECT_EQ(payload2, send2);

  target2_host.Stop();
}

TEST(CircuitRelayServiceAbortTest, AbortInflightRequest) {
  static std::atomic<int> port{48000 + (ProcessId() % 2000) * 10};
  const int relay_port = port.fetch_add(1);
  const int client_port = port.fetch_add(1);

  PeerSessionConfig config;
  config.dial_timeout = std::chrono::milliseconds(3000);
  config.dial_failure_backoff = std::chrono::milliseconds(100);

  Libp2pHost relay_host;
  Libp2pHost client_host;
  Libp2pHostConfig relay_cfg;
  relay_cfg.listen_multiaddr = "/ip4/127.0.0.1/tcp/" + std::to_string(relay_port);
  Libp2pHostConfig client_cfg;
  client_cfg.listen_multiaddr = "/ip4/127.0.0.1/tcp/" + std::to_string(client_port);
  ASSERT_TRUE(relay_host.Start(relay_cfg));
  ASSERT_TRUE(client_host.Start(client_cfg));

  auto relay_sessions = std::make_unique<PeerSessionManager>(relay_host, config);
  auto client_sessions = std::make_unique<PeerSessionManager>(client_host, config);
  auto relay_service = std::make_unique<CircuitRelayService>(relay_host, *relay_sessions);
  auto client_service = std::make_unique<CircuitRelayService>(client_host, *client_sessions);
  relay_service->Start();

  auto relay_id = relay_host.LocalPeerIdBase58();
  ASSERT_TRUE(relay_id);
  const std::string relay_ma = "/ip4/127.0.0.1/tcp/" + std::to_string(relay_port) + "/p2p/" + *relay_id;
  ASSERT_TRUE(client_sessions->RegisterEndpoint("relay", relay_ma));

  std::thread waiter([&] {
    CircuitBridgeTarget target;
    target.target_multiaddr =
        "/ip4/127.0.0.1/tcp/1/p2p/12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR";
    target.target_protocol = kBridgeTargetProtocol;
    (void)client_service->RequestBridge("relay", target, 8000);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  client_service->AbortInflightRequests();
  waiter.join();

  client_service.reset();
  relay_service.reset();
  client_sessions.reset();
  relay_sessions.reset();
  client_host.Stop();
  relay_host.Stop();
}

} // namespace
} // namespace pbr
