#include "libp2p/integration/host/CircuitBridgeTarget.h"

#include "libp2p/integration/host/Libp2pHost.h"
#include "libp2p/integration/host/PeerSessionManager.h"
#include "base/people/tests/libp2p_ephemeral_listen.h"

#include <gtest/gtest.h>

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
  int host_port = 0;
  auto host_started = test::StartEphemeralLoopbackHost(host, host_port);
  ASSERT_TRUE(host_started) << host_started.error().message;
  auto local = host.LocalPeerIdBase58();
  ASSERT_TRUE(local);

  Libp2pHost other;
  int other_port = 0;
  auto other_started = test::StartEphemeralLoopbackHost(other, other_port);
  ASSERT_TRUE(other_started) << other_started.error().message;
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
  int listen_port = 0;
  auto started = test::StartEphemeralLoopbackHost(host, listen_port);
  ASSERT_TRUE(started) << started.error().message;
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
