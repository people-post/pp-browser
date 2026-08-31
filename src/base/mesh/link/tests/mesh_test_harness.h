#pragma once

#include "base/adp/Clock.h"
#include "base/adp/Endpoint.h"
#include "base/adp/MemoryDatagramIo.h"
#include "base/crypto/MlDsa.h"
#include "base/mesh/link/AdpMultiaddr.h"
#include "base/mesh/link/MeshRuntime.h"
#include "base/mesh/link/tests/mesh_harness_support.h"
#include "base/p2p/PeerIdUtil.h"

#include <functional>
#include <memory>
#include <string>

namespace pbr::test {

/** Two-peer MemoryDatagramIo harness for AMP L4 protocol tests. */
struct AmpMeshHarness {
  std::shared_ptr<adp::VirtualClock> clock;
  std::shared_ptr<adp::MemoryDatagramHub> hub;
  std::shared_ptr<adp::MemoryDatagramIo> io_a;
  std::shared_ptr<adp::MemoryDatagramIo> io_b;
  std::unique_ptr<adp::Endpoint> ep_a;
  std::unique_ptr<adp::Endpoint> ep_b;
  adp::IpEndpoint addr_a;
  adp::IpEndpoint addr_b;
  amp::MshIdentity alice;
  amp::MshIdentity bob;
  std::unique_ptr<amp::MeshRuntime> runtime_a;
  std::unique_ptr<amp::MeshRuntime> runtime_b;
  std::string peer_id_a;
  std::string peer_id_b;
  std::string ma_a;
  std::string ma_b;

  amp::PeerLinkManager& mgr_a() { return runtime_a->Links(); }
  amp::PeerLinkManager& mgr_b() { return runtime_b->Links(); }

  static Roe<std::unique_ptr<AmpMeshHarness>> Create() {
    auto harness = std::make_unique<AmpMeshHarness>();
    harness->clock = std::make_shared<adp::VirtualClock>(1'000'000);
    harness->hub = adp::MemoryDatagramIo::MakeHub();
    harness->addr_a = adp::IpEndpoint::V4(10, 0, 0, 1, 1000);
    harness->addr_b = adp::IpEndpoint::V4(10, 0, 0, 2, 2000);
    harness->io_a = std::make_shared<adp::MemoryDatagramIo>(harness->hub, harness->addr_a);
    harness->io_b = std::make_shared<adp::MemoryDatagramIo>(harness->hub, harness->addr_b);
    harness->ep_a = std::make_unique<adp::Endpoint>(harness->io_a, harness->clock);
    harness->ep_b = std::make_unique<adp::Endpoint>(harness->io_b, harness->clock);
    harness->ep_b->SetAcceptEnabled(true);

    auto alice_keys = MlDsa::GenerateKeyPair();
    auto bob_keys = MlDsa::GenerateKeyPair();
    if (!alice_keys || !bob_keys) {
      return Error("amp mesh harness: keygen failed");
    }
    harness->alice.ml_dsa_secret_key = std::move(alice_keys->secret_key);
    harness->alice.ml_dsa_public_key = std::move(alice_keys->public_key);
    harness->bob.ml_dsa_secret_key = std::move(bob_keys->secret_key);
    harness->bob.ml_dsa_public_key = std::move(bob_keys->public_key);

    auto alice_id = PeerIdFromMlDsaPublicKey(harness->alice.ml_dsa_public_key);
    auto bob_id = PeerIdFromMlDsaPublicKey(harness->bob.ml_dsa_public_key);
    if (!alice_id || !bob_id) {
      return Error("amp mesh harness: peer id derivation failed");
    }
    harness->peer_id_a = *alice_id;
    harness->peer_id_b = *bob_id;

    const auto link_config = AmpMeshTestLinkConfig();
    harness->runtime_a = std::make_unique<amp::MeshRuntime>(*harness->ep_a, harness->alice, harness->peer_id_a,
                                                            link_config);
    harness->runtime_b = std::make_unique<amp::MeshRuntime>(*harness->ep_b, harness->bob, harness->peer_id_b,
                                                            link_config);
    harness->runtime_a->Start();
    harness->runtime_b->Start();

    auto ma_b = amp::FormatAdpMultiaddr(harness->addr_b, harness->peer_id_b);
    auto ma_a = amp::FormatAdpMultiaddr(harness->addr_a, harness->peer_id_a);
    if (!ma_b || !ma_a) {
      return Error("amp mesh harness: multiaddr format failed");
    }
    harness->ma_a = *ma_a;
    harness->ma_b = *ma_b;
    return harness;
  }

  void PumpBoth() {
    runtime_a->Pump();
    runtime_b->Pump();
    runtime_a->Tick();
    runtime_b->Tick();
  }

  void PumpUntil(const std::function<bool()>& done, const size_t max_rounds = 500) {
    for (size_t i = 0; i < max_rounds && !done(); ++i) {
      PumpBoth();
    }
  }
};

} // namespace pbr::test
