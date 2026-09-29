#include "app/node/tools/call_probe/ProductStackHarness.h"

#include "amp/L1/Clock.h"
#include "amp/L1/OsUdpDatagramIo.h"
#include "amp/L1/Types.h"
#include "amp/link/AdpMultiaddr.h"
#include "amp/link/AmpStack.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/identity/PeerIdUtil.h"

#include <gtest/gtest.h>
#include <memory>
#include <sodium.h>
#include <string>

namespace {

struct LoopbackStack {
  std::shared_ptr<pp::adp::WallClock> clock;
  std::unique_ptr<pp::amp::AmpStack> stack;
  std::string listen_ma;
};

/** A real Amp stack on 127.0.0.1 (ephemeral port) — what the hard-lab probe hands the harness. */
LoopbackStack MakeLoopbackStack() {
  LoopbackStack out;
  auto keys = pbr::MlDsa::GenerateKeyPair();
  EXPECT_TRUE(keys);
  auto peer_id = pbr::PeerIdFromMlDsaPublicKey(keys->public_key);
  EXPECT_TRUE(peer_id);
  auto bound = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 0));
  EXPECT_TRUE(bound);

  pp::amp::AmpStack::Config cfg;
  cfg.identity.ml_dsa_secret_key = std::move(keys->secret_key);
  cfg.identity.ml_dsa_public_key = std::move(keys->public_key);
  cfg.local_peer_id = *peer_id;
  out.clock = std::make_shared<pp::adp::WallClock>();
  std::shared_ptr<pp::adp::DatagramIo> io = std::move(*bound);
  auto stack = pp::amp::AmpStack::Create(std::move(io), out.clock, std::move(cfg));
  EXPECT_TRUE(stack);
  out.stack = std::move(*stack);
  out.stack->Start();
  auto listen = pp::amp::FormatAdpMultiaddr(out.stack->LocalEndpoint(), *peer_id);
  EXPECT_TRUE(listen);
  out.listen_ma = *listen;
  return out;
}

// Hard lab B-HARD-CALL-NAT-STACK (2026-09-27): the media plane replaced its media_relay client on
// mesh start while the call topology still held the old one, and the topology then unregistered
// its session-end observer from the destroyed client (SIGSEGV before warm-hop). The owner's mesh
// start, capability-refresh rewires and teardown must detach dependents before replacing objects.
TEST(ProductStackHarnessTest, MeshStartRelayRewiresAndTeardownKeepTopologyOffDroppedRelayClients) {
  ASSERT_GE(sodium_init(), 0);
  auto loopback = MakeLoopbackStack();
  ASSERT_TRUE(loopback.stack);
  auto harness = pbr::call_probe::ProductStackHarness::Create(std::move(loopback.stack), loopback.clock,
                                                              loopback.listen_ma, /*hop_ma=*/"");
  ASSERT_TRUE(harness) << harness.error().message;
  (*harness)->RefreshMeshMedia();
  (*harness)->RefreshMeshMedia();
  harness->reset();
}

} // namespace
