#include "libp2p/integration/host/CircuitBridgeTarget.h"

#include "libp2p/integration/host/Libp2pHost.h"
#include "libp2p/integration/host/PeerSessionManager.h"

#include <gtest/gtest.h>

#include <atomic>

namespace pbr {
namespace {

TEST(CircuitBridgeTargetTest, RequiresPeerIdOrMultiaddr) {
  Libp2pHost host;
  PeerSessionManager sessions(host);
  CircuitBridgeTarget target;
  auto normalized = NormalizeCircuitBridgeTarget(sessions, host, target);
  EXPECT_FALSE(normalized);
}

TEST(CircuitBridgeTargetTest, RejectsPeerIdMismatch) {
  Libp2pHost host;
  static std::atomic<int> port{41500};
  Libp2pHostConfig cfg;
  cfg.listen_multiaddr = "/ip4/127.0.0.1/tcp/" + std::to_string(port.fetch_add(1));
  ASSERT_TRUE(host.Start(cfg));
  auto local = host.LocalPeerIdBase58();
  ASSERT_TRUE(local);

  Libp2pHost other;
  Libp2pHostConfig other_cfg;
  other_cfg.listen_multiaddr = "/ip4/127.0.0.1/tcp/" + std::to_string(port.fetch_add(1));
  ASSERT_TRUE(other.Start(other_cfg));
  auto other_id = other.LocalPeerIdBase58();
  ASSERT_TRUE(other_id);

  PeerSessionManager sessions(host);
  CircuitBridgeTarget target;
  target.target_peer_id = *local;
  target.target_multiaddr = "/ip4/203.0.113.50/tcp/4001/p2p/" + *other_id;
  auto normalized = NormalizeCircuitBridgeTarget(sessions, host, target);
  EXPECT_FALSE(normalized);

  other.Stop();
  host.Stop();
}

TEST(CircuitBridgeTargetTest, ResolvesFromAddressBook) {
  Libp2pHost host;
  static std::atomic<int> port{41600};
  Libp2pHostConfig cfg;
  cfg.listen_multiaddr = "/ip4/127.0.0.1/tcp/" + std::to_string(port.fetch_add(1));
  ASSERT_TRUE(host.Start(cfg));
  auto local = host.LocalPeerIdBase58();
  ASSERT_TRUE(local);
  const std::string ma = "/ip4/203.0.113.51/tcp/4001/p2p/" + *local;

  PeerSessionManager sessions(host);
  ASSERT_TRUE(sessions.UpsertBookEntry(*local, ma, PeerAddrSource::Connection));

  CircuitBridgeTarget target;
  target.target_peer_id = *local;
  auto normalized = NormalizeCircuitBridgeTarget(sessions, host, target);
  ASSERT_TRUE(normalized);
  EXPECT_EQ(normalized->first, *local);
  EXPECT_EQ(normalized->second, ma);

  host.Stop();
}

} // namespace
} // namespace pbr
