#pragma once

#include "libp2p/integration/host/CircuitRelayService.h"
#include "libp2p/integration/host/Libp2pHost.h"
#include "libp2p/integration/host/PeerSessionManager.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>

#if defined(_WIN32)
#include <process.h>
static inline int LoopbackPartitionProcessId() { return _getpid(); }
#else
#include <unistd.h>
static inline int LoopbackPartitionProcessId() { return static_cast<int>(getpid()); }
#endif

namespace pbr {
namespace test {

/**
 * Three-host loopback partition: A can dial R, R can dial B, A has no direct path to B
 * until a circuit hop is established. Shared by circuit+call-media and circuit+media_relay
 * compose tests.
 */
class LoopbackPartitionFixture : public ::testing::Test {
protected:
  void SetUp() override {
    static std::atomic<int> port{49000 + (LoopbackPartitionProcessId() % 2000) * 10};
    a_port_ = port.fetch_add(1);
    r_port_ = port.fetch_add(1);
    b_port_ = port.fetch_add(1);

    PeerSessionConfig config;
    config.dial_timeout = std::chrono::milliseconds(3000);
    config.dial_failure_backoff = std::chrono::milliseconds(100);

    auto start_host = [](Libp2pHost& host, int listen_port) {
      Libp2pHostConfig cfg;
      cfg.listen_multiaddr = "/ip4/127.0.0.1/tcp/" + std::to_string(listen_port);
      return host.Start(cfg);
    };

    ASSERT_TRUE(start_host(a_host_, a_port_));
    ASSERT_TRUE(start_host(r_host_, r_port_));
    ASSERT_TRUE(start_host(b_host_, b_port_));

    a_sessions_ = std::make_unique<PeerSessionManager>(a_host_, config);
    r_sessions_ = std::make_unique<PeerSessionManager>(r_host_, config);
    b_sessions_ = std::make_unique<PeerSessionManager>(b_host_, config);

    a_circuit_ = std::make_unique<CircuitRelayService>(a_host_, *a_sessions_);
    r_circuit_ = std::make_unique<CircuitRelayService>(r_host_, *r_sessions_);
    r_circuit_->Start();

    auto a_id = a_host_.LocalPeerIdBase58();
    auto r_id = r_host_.LocalPeerIdBase58();
    auto b_id = b_host_.LocalPeerIdBase58();
    ASSERT_TRUE(a_id);
    ASSERT_TRUE(r_id);
    ASSERT_TRUE(b_id);
    a_peer_id_ = *a_id;
    r_peer_id_ = *r_id;
    b_peer_id_ = *b_id;

    a_ma_ = "/ip4/127.0.0.1/tcp/" + std::to_string(a_port_) + "/p2p/" + a_peer_id_;
    r_ma_ = "/ip4/127.0.0.1/tcp/" + std::to_string(r_port_) + "/p2p/" + r_peer_id_;
    b_ma_ = "/ip4/127.0.0.1/tcp/" + std::to_string(b_port_) + "/p2p/" + b_peer_id_;

    // Partition: A knows only R; R knows B (and can dial it). A must not register B.
    ASSERT_TRUE(a_sessions_->RegisterEndpoint(r_peer_id_, r_ma_));
    ASSERT_TRUE(r_sessions_->RegisterEndpoint(b_peer_id_, b_ma_));
    ASSERT_FALSE(a_sessions_->IsDialable(b_peer_id_));
  }

  void TearDown() override {
    a_circuit_.reset();
    r_circuit_.reset();
    a_sessions_.reset();
    r_sessions_.reset();
    b_sessions_.reset();
    a_host_.Stop();
    r_host_.Stop();
    b_host_.Stop();
  }

  /** Establish A→R→B circuit for `target_protocol`; leaves hop on A's session manager. */
  Roe<void> EnsureCircuitFromA(const std::string& target_protocol) {
    return a_sessions_->TryEnsureHopViaCircuit(b_peer_id_, *a_circuit_, {r_peer_id_}, target_protocol,
                                               8000);
  }

  int a_port_ = 0;
  int r_port_ = 0;
  int b_port_ = 0;
  std::string a_peer_id_;
  std::string r_peer_id_;
  std::string b_peer_id_;
  std::string a_ma_;
  std::string r_ma_;
  std::string b_ma_;
  Libp2pHost a_host_;
  Libp2pHost r_host_;
  Libp2pHost b_host_;
  std::unique_ptr<PeerSessionManager> a_sessions_;
  std::unique_ptr<PeerSessionManager> r_sessions_;
  std::unique_ptr<PeerSessionManager> b_sessions_;
  std::unique_ptr<CircuitRelayService> a_circuit_;
  std::unique_ptr<CircuitRelayService> r_circuit_;
};

} // namespace test
} // namespace pbr
