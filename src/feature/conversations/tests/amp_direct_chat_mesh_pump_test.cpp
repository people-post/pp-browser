#include "feature/conversations/AmpDirectChatTransport.h"

#include "amp/L1/Clock.h"
#include "amp/L1/OsUdpDatagramIo.h"
#include "amp/link/AdpMultiaddr.h"
#include "amp/link/AmpStack.h"
#include "domain/mesh/host/MeshHost.h"
#include "domain/mesh/tests/support/mesh_harness_support.h"
#include "domain/messaging/RelayWirePayload.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/identity/PeerIdUtil.h"

#include <gtest/gtest.h>
#include <sodium.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

namespace pbr {
namespace {

struct PumpedHost {
  std::unique_ptr<MeshHost> host;
  std::string peer_id;
  std::string listen_ma;
};

/** Wall-clock UDP host on 127.0.0.1 driven by its own MeshPump (pp-call-probe threading). */
Roe<PumpedHost> MakePumpedHost() {
  auto keys = MlDsa::GenerateKeyPair();
  if (!keys) {
    return keys.error();
  }
  auto peer_id = PeerIdFromMlDsaPublicKey(keys->public_key);
  if (!peer_id) {
    return peer_id.error();
  }
  auto bound = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 0));
  if (!bound) {
    return Error(bound.error().message);
  }
  pp::amp::AmpStack::Config cfg;
  cfg.identity.ml_dsa_secret_key = std::move(keys->secret_key);
  cfg.identity.ml_dsa_public_key = std::move(keys->public_key);
  cfg.local_peer_id = *peer_id;
  cfg.link_config = pbr::test::AmpMeshTestLinkConfig();
  std::shared_ptr<pp::adp::DatagramIo> io = std::move(*bound);
  auto stack = pp::amp::AmpStack::Create(std::move(io), std::make_shared<pp::adp::WallClock>(), std::move(cfg));
  if (!stack) {
    return Error(stack.error().message);
  }
  (*stack)->Start();
  (*stack)->GetEndpoint().SetAcceptEnabled(true);
  auto ma = pp::amp::FormatAdpMultiaddr((*stack)->LocalEndpoint(), *peer_id);
  if (!ma) {
    return Error(ma.error().message);
  }
  PumpedHost out;
  out.host = std::make_unique<MeshHost>();
  out.peer_id = *peer_id;
  out.listen_ma = *ma;
  if (auto attached = out.host->AttachAmpStack(std::move(*stack), *ma, MeshHost::AttachDrive::MeshPump);
      !attached) {
    return attached.error();
  }
  return out;
}

std::unique_ptr<AmpDirectChatTransport> MakeChat(MeshHost& host) {
  auto deps = host.ChatDeps();
  if (!deps) {
    return nullptr;
  }
  return std::make_unique<AmpDirectChatTransport>(deps->links, deps->io.io_pump, deps->io.post_worker,
                                                  deps->io.post_io, deps->io.post_after);
}

RelayEnvelope MakeEnvelope() {
  RelayEnvelope envelope;
  envelope.envelope_version = kRelayEnvelopeVersion;
  envelope.message_id = "m-pump";
  envelope.sender_relay_id = "relay:a";
  envelope.sender_contact_id = "relay:a";
  envelope.route.kind = "direct";
  envelope.route.channel = ThreadChannel::E2e;
  auto payload_b64 = RelayWirePayload::EncodePlaintextText("hello over MeshPump");
  envelope.body.e2e.payload_b64 = payload_b64 ? *payload_b64 : "";
  envelope.timestamp = 1;
  return envelope;
}

// pp-call-probe runs AttachDrive::MeshPump and sends call control with the sync SendEnvelope from
// its UI thread (not the Drive thread). Hard lab: every invite timed out ("amp direct chat send
// timed out") after the probe moved off manual Tick (2026-09-25).
TEST(AmpDirectChatMeshPumpTest, SyncSendFromNonDriveThreadIsAcked) {
  ASSERT_GE(sodium_init(), 0);
  auto a = MakePumpedHost();
  ASSERT_TRUE(static_cast<bool>(a)) << a.error().message;
  auto b = MakePumpedHost();
  ASSERT_TRUE(static_cast<bool>(b)) << b.error().message;

  auto a_chat = MakeChat(*a->host);
  auto b_chat = MakeChat(*b->host);
  ASSERT_NE(a_chat, nullptr);
  ASSERT_NE(b_chat, nullptr);
  std::atomic<bool> received{false};
  b_chat->SetInboundHandler([&received](RelayEnvelope) { received.store(true, std::memory_order_release); });
  a_chat->Start();
  b_chat->Start();

  ASSERT_TRUE(static_cast<bool>(a->host->Amp()->Links().RegisterEndpoint(b->peer_id, b->listen_ma)));
  std::atomic<bool> associated{false};
  a->host->Amp()->Links().EnsureAssociation(b->peer_id, [&associated](auto) {
    associated.store(true, std::memory_order_release);
  });
  const auto assoc_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!a->host->Amp()->Links().IsConnected(b->peer_id) && std::chrono::steady_clock::now() < assoc_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(a->host->Amp()->Links().IsConnected(b->peer_id)) << "associated_cb=" << associated.load();
  auto sent = a_chat->SendEnvelope(b->peer_id, MakeEnvelope());
  EXPECT_TRUE(static_cast<bool>(sent)) << sent.error().message;

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!received.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_TRUE(received.load(std::memory_order_acquire));

  a_chat.reset();
  b_chat.reset();
  a->host->Stop();
  b->host->Stop();
}

} // namespace
} // namespace pbr
