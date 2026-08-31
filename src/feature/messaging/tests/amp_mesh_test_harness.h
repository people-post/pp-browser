#pragma once

#include "base/adp/Clock.h"
#include "base/adp/Endpoint.h"
#include "base/adp/MemoryDatagramIo.h"
#include "base/crypto/MlDsa.h"
#include "base/mesh/link/AdpMultiaddr.h"
#include "base/mesh/link/MeshPump.h"
#include "base/mesh/link/PeerLinkManager.h"

#include <functional>
#include <memory>
#include <optional>
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
  std::unique_ptr<amp::PeerLinkManager> mgr_a;
  std::unique_ptr<amp::PeerLinkManager> mgr_b;
  std::unique_ptr<amp::MeshPump> pump_a;
  std::unique_ptr<amp::MeshPump> pump_b;
  std::string ma_a;
  std::string ma_b;

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

    harness->mgr_a = std::make_unique<amp::PeerLinkManager>(*harness->ep_a, harness->alice, "QmAlice");
    harness->mgr_b = std::make_unique<amp::PeerLinkManager>(*harness->ep_b, harness->bob, "QmBob");
    harness->pump_a = std::make_unique<amp::MeshPump>(*harness->ep_a, *harness->mgr_a);
    harness->pump_b = std::make_unique<amp::MeshPump>(*harness->ep_b, *harness->mgr_b);

    auto ma_b = amp::FormatAdpMultiaddr(harness->addr_b, "QmBob");
    auto ma_a = amp::FormatAdpMultiaddr(harness->addr_a, "QmAlice");
    if (!ma_b || !ma_a) {
      return Error("amp mesh harness: multiaddr format failed");
    }
    harness->ma_a = *ma_a;
    harness->ma_b = *ma_b;
    return harness;
  }

  void PumpBoth() {
    pump_a->Pump();
    pump_b->Pump();
    pump_a->Tick();
    pump_b->Tick();
  }

  void PumpUntil(const std::function<bool()>& done, const size_t max_rounds = 500) {
    for (size_t i = 0; i < max_rounds && !done(); ++i) {
      PumpBoth();
    }
  }

  Roe<void> ConnectPeers(const std::string& a_peer_key, const std::string& b_peer_key) {
    if (!mgr_a->RegisterEndpoint(b_peer_key, ma_b)) {
      return mgr_a->RegisterEndpoint(b_peer_key, ma_b).error();
    }
    if (!mgr_b->RegisterEndpoint(a_peer_key, ma_a)) {
      return mgr_b->RegisterEndpoint(a_peer_key, ma_a).error();
    }
    bool associated = false;
    mgr_a->EnsureAssociation(b_peer_key, [&](Roe<void> result) { associated = static_cast<bool>(result); });
    PumpUntil([&] { return associated && mgr_b->FindConnectedInboundLink() != nullptr; });
    if (!associated) {
      return Error("amp mesh harness: association failed");
    }
    return Roe<void>();
  }
};

} // namespace pbr::test
