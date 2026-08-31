#include "base/adp/Clock.h"
#include "base/adp/Endpoint.h"
#include "base/adp/MemoryDatagramIo.h"
#include "base/crypto/MlDsa.h"
#include "base/mesh/channel/ChannelPolicy.h"
#include "base/mesh/link/AdpMultiaddr.h"
#include "base/mesh/link/MeshPump.h"
#include "base/mesh/link/PeerLinkManager.h"
#include "base/mesh/link/tests/mesh_test_harness.h"

#include <gtest/gtest.h>
#include <sodium.h>

#include <functional>
#include <optional>
#include <string>

namespace pbr::amp {
namespace {

struct MeshLinkFixture {
  std::shared_ptr<adp::VirtualClock> clock;
  std::shared_ptr<adp::MemoryDatagramHub> hub;
  std::shared_ptr<adp::MemoryDatagramIo> io_a;
  std::shared_ptr<adp::MemoryDatagramIo> io_b;
  std::unique_ptr<adp::Endpoint> ep_a;
  std::unique_ptr<adp::Endpoint> ep_b;
  adp::IpEndpoint addr_a;
  adp::IpEndpoint addr_b;
  MshIdentity alice;
  MshIdentity bob;
  std::unique_ptr<PeerLinkManager> mgr_a;
  std::unique_ptr<PeerLinkManager> mgr_b;
  std::unique_ptr<MeshPump> pump_a;
  std::unique_ptr<MeshPump> pump_b;

  static Roe<MeshLinkFixture> Create() {
    MeshLinkFixture f;
    f.clock = std::make_shared<adp::VirtualClock>(1'000'000);
    f.hub = adp::MemoryDatagramIo::MakeHub();
    f.addr_a = adp::IpEndpoint::V4(10, 0, 0, 1, 1000);
    f.addr_b = adp::IpEndpoint::V4(10, 0, 0, 2, 2000);
    f.io_a = std::make_shared<adp::MemoryDatagramIo>(f.hub, f.addr_a);
    f.io_b = std::make_shared<adp::MemoryDatagramIo>(f.hub, f.addr_b);
    f.ep_a = std::make_unique<adp::Endpoint>(f.io_a, f.clock);
    f.ep_b = std::make_unique<adp::Endpoint>(f.io_b, f.clock);
    f.ep_b->SetAcceptEnabled(true);

    auto alice_keys = MlDsa::GenerateKeyPair();
    auto bob_keys = MlDsa::GenerateKeyPair();
    if (!alice_keys || !bob_keys) {
      return Error("mesh link test: keygen failed");
    }
    f.alice.ml_dsa_secret_key = std::move(alice_keys->secret_key);
    f.alice.ml_dsa_public_key = std::move(alice_keys->public_key);
    f.bob.ml_dsa_secret_key = std::move(bob_keys->secret_key);
    f.bob.ml_dsa_public_key = std::move(bob_keys->public_key);

    f.mgr_a = std::make_unique<PeerLinkManager>(*f.ep_a, f.alice, "QmAlice");
    f.mgr_b = std::make_unique<PeerLinkManager>(*f.ep_b, f.bob, "QmBob");
    f.pump_a = std::make_unique<MeshPump>(*f.ep_a, *f.mgr_a);
    f.pump_b = std::make_unique<MeshPump>(*f.ep_b, *f.mgr_b);
    return f;
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
};

TEST(MeshLinkTest, EnsureAssociationOverMemoryIo) {
  ASSERT_GE(sodium_init(), 0);
  auto fixture = MeshLinkFixture::Create();
  ASSERT_TRUE(static_cast<bool>(fixture));

  auto bob_addr = FormatAdpMultiaddr(fixture->addr_b, "QmBob");
  ASSERT_TRUE(static_cast<bool>(bob_addr));
  ASSERT_TRUE(static_cast<bool>(fixture->mgr_a->RegisterEndpoint("bob", *bob_addr)));

  bool associated = false;
  std::string assoc_error;
  fixture->mgr_a->EnsureAssociation("bob", [&](Roe<void> result) {
    associated = result.isOk();
    if (!associated) {
      assoc_error = result.error().message;
    }
  });

  fixture->PumpUntil([&] {
    return associated && fixture->mgr_b->FindConnectedInboundLink() != nullptr;
  });

  EXPECT_TRUE(associated) << assoc_error;
  EXPECT_TRUE(fixture->mgr_a->IsConnected("bob"));
  ASSERT_NE(fixture->mgr_b->FindConnectedInboundLink(), nullptr);
}

TEST(MeshLinkTest, OpenChannelDataRoundTrip) {
  ASSERT_GE(sodium_init(), 0);
  auto fixture = MeshLinkFixture::Create();
  ASSERT_TRUE(static_cast<bool>(fixture));

  auto bob_addr = FormatAdpMultiaddr(fixture->addr_b, "QmBob");
  ASSERT_TRUE(static_cast<bool>(bob_addr));
  ASSERT_TRUE(static_cast<bool>(fixture->mgr_a->RegisterEndpoint("bob", *bob_addr)));

  bool associated = false;
  fixture->mgr_a->EnsureAssociation("bob", [&](Roe<void> result) { associated = static_cast<bool>(result); });
  fixture->PumpUntil([&] {
    return associated && fixture->mgr_b->FindConnectedInboundLink() != nullptr;
  });
  ASSERT_TRUE(associated);

  uint32_t channel_id = 0;
  bool channel_done = false;
  std::optional<uint32_t> channel_id_result;
  std::string channel_error;
  fixture->mgr_a->OpenChannel("bob", "/pp-browser/chat/1.0.0", ControlJsonChannelPolicy(),
                              [&](Roe<uint32_t> ch) {
                                if (ch.isOk()) {
                                  channel_id_result = ch.value();
                                } else {
                                  channel_error = ch.error().message;
                                }
                                channel_done = true;
                              });
  fixture->PumpUntil([&] { return channel_done; });
  ASSERT_TRUE(channel_id_result.has_value()) << channel_error;
  channel_id = *channel_id_result;

  fixture->PumpUntil([&] {
    auto* outbound = fixture->mgr_a->FindLink("bob");
    return outbound && outbound->Mux() && outbound->Mux()->State(channel_id) == ChannelState::Open;
  });

  std::vector<uint8_t> received;
  auto* inbound = fixture->mgr_b->FindConnectedInboundLink();
  ASSERT_NE(inbound, nullptr);
  inbound->Mux()->SetDataHandler(channel_id, [&](uint32_t, std::vector<uint8_t> payload) {
    received = std::move(payload);
  });

  auto* outbound = fixture->mgr_a->FindLink("bob");
  ASSERT_NE(outbound, nullptr);
  const std::vector<uint8_t> msg = {'h', 'i'};
  ASSERT_TRUE(static_cast<bool>(outbound->Mux()->SendData(channel_id, msg)));
  fixture->PumpBoth();
  EXPECT_EQ(received, msg);
}

TEST(MeshRuntimeTest, PumpDrivesAssociationRoundTrip) {
  ASSERT_GE(sodium_init(), 0);
  auto created = test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created));
  auto harness = std::move(*created);

  ASSERT_TRUE(static_cast<bool>(harness->mgr_a().RegisterEndpoint("b", harness->ma_b)));
  ASSERT_TRUE(static_cast<bool>(harness->mgr_b().RegisterEndpoint("a", harness->ma_a)));

  bool associated = false;
  harness->mgr_a().EnsureAssociation("b", [&](Roe<void> result) { associated = static_cast<bool>(result); });
  harness->PumpUntil([&] {
    return associated && harness->mgr_a().IsConnected("b") && harness->mgr_b().FindLinkByPeerId(harness->peer_id_a);
  });

  EXPECT_TRUE(associated);
  EXPECT_TRUE(harness->mgr_a().IsConnected("b"));
  auto* inbound_on_b = harness->mgr_b().FindLinkByPeerId(harness->peer_id_a);
  ASSERT_NE(inbound_on_b, nullptr);
  EXPECT_EQ(inbound_on_b->RemotePeerId(), harness->peer_id_a);
  EXPECT_EQ(inbound_on_b->PeerKey(), "a");
}

TEST(MeshLinkTest, InboundLinkRekeysToRegisteredAlias) {
  ASSERT_GE(sodium_init(), 0);
  auto created = test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created));
  auto harness = std::move(*created);

  test::AmpMeshHarness& h = *harness;

  ASSERT_TRUE(static_cast<bool>(h.mgr_a().RegisterEndpoint("b", h.ma_b)));
  ASSERT_TRUE(static_cast<bool>(h.mgr_b().RegisterEndpoint("a", h.ma_a)));

  bool associated = false;
  h.mgr_a().EnsureAssociation("b", [&](Roe<void> result) { associated = static_cast<bool>(result); });
  h.PumpUntil([&] { return associated; });
  ASSERT_TRUE(associated);

  auto* outbound = h.mgr_a().FindLink("b");
  ASSERT_NE(outbound, nullptr);
  EXPECT_EQ(outbound->RemotePeerId(), h.peer_id_b);

  auto* inbound = h.mgr_b().FindLink("a");
  ASSERT_NE(inbound, nullptr);
  EXPECT_EQ(inbound->RemotePeerId(), h.peer_id_a);
  EXPECT_FALSE(inbound->IsOutbound());

  EXPECT_EQ(h.mgr_b().FindLinkByPeerId(h.peer_id_a), inbound);
  EXPECT_EQ(h.mgr_a().FindLinkByPeerId(h.peer_id_b), outbound);
}

} // namespace
} // namespace pbr::amp
