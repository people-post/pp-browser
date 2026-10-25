#include "domain/delivery/AmpDirectChatTransport.h"

#include "common/chat/MessagingJson.h"
#include "lib/messaging/RelayWirePayload.h"
#include "mesh/host/MeshPorts.h"
#include "mesh/l4/circuit/CircuitRelayTypes.h"
#include "mesh/l4/circuit/client/AmpCircuitHopRegistry.h"
#include "mesh/l4/circuit/client/CircuitClientCoordinator.h"
#include "mesh/l4/circuit/serve/CircuitRelayServer.h"
#include "amp/link/Types.h"
#include "mesh/testing/mesh_triple_harness.h"
#include "common/MeshAlias.h"

#include <gtest/gtest.h>
#include <sodium.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>

namespace pbr {
namespace {

RelayEnvelope MakeTestEnvelope(const std::string& message_id, const std::string& text) {
  RelayEnvelope envelope;
  envelope.envelope_version = kRelayEnvelopeVersion;
  envelope.message_id = message_id;
  envelope.sender_relay_id = "relay:sender";
  envelope.sender_contact_id = "relay:sender";
  envelope.sender_device = "QmTestDevice";
  envelope.route.kind = "direct";
  envelope.route.channel = ThreadChannel::E2e;
  auto payload_b64 = RelayWirePayload::EncodePlaintextText(text);
  envelope.body.e2e.payload_b64 = payload_b64 ? *payload_b64 : "";
  envelope.timestamp = 1;
  return envelope;
}

/**
 * Regression: AmpDirectChatTransport must treat nested circuit IsConnected as reachable
 * even when A has no RegisterEndpoint for B (hard-lab / via-hop chat path).
 */
/** The failure a mesh completion carries for `Result` (pp-cpp-mesh docs/ERRORS.md). */
template <typename Result>
struct WaitFailure {
  using type = Error;
};
template <>
struct WaitFailure<mesh::CircuitTunnelBridgeResult> {
  using type = mesh::CircuitClientCoordinator::Failure;
};

class AmpDirectChatCircuitNestedTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_GE(sodium_init(), 0);
    auto created = mesh::test::AmpMeshTripleHarness::Create();
    ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
    harness_ = std::move(*created);

    ASSERT_TRUE(static_cast<bool>(harness_->mgr_a().RegisterEndpoint("relay", harness_->ma_r)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_r().RegisterEndpoint("a", harness_->ma_a)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_r().RegisterEndpoint("b", harness_->ma_b)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_r().RegisterEndpoint(harness_->peer_id_b, harness_->ma_b)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_b().RegisterEndpoint("relay", harness_->ma_r)));

    harness_->mgr_a().EnableNestedCarrierAccept(true);
    harness_->mgr_b().EnableNestedCarrierAccept(true);

    chat_a_ = mesh::NewAmpChatPeerLinks(*harness_->runtime_a);
    chat_b_ = mesh::NewAmpChatPeerLinks(*harness_->runtime_b);

    hops_ = std::make_unique<mesh::AmpCircuitHopRegistry>();
    circuit_r_ = std::make_unique<mesh::CircuitRelayServer>(*harness_->runtime_r);
    circuit_a_ = std::make_unique<mesh::CircuitClientCoordinator>(*harness_->runtime_a);
    circuit_r_->Start();
    circuit_r_->SetServeInbound(true);
    circuit_a_->Start();

    a_chat_ = std::make_unique<AmpDirectChatTransport>(
        *chat_a_, [this] { harness_->PumpAll(); }, AmpDirectChatTransport::WorkerPost{},
        [this](std::function<void()> task) {
          if (harness_->runtime_a && task) {
            harness_->runtime_a->PostToIo(std::move(task));
          }
        },
        [this](std::chrono::milliseconds delay, std::function<void()> task) {
          if (harness_->runtime_a && task) {
            harness_->runtime_a->PostAfter(delay, std::move(task));
          }
        });
    b_chat_ = std::make_unique<AmpDirectChatTransport>(
        *chat_b_, [this] { harness_->PumpAll(); }, AmpDirectChatTransport::WorkerPost{},
        [this](std::function<void()> task) {
          if (harness_->runtime_b && task) {
            harness_->runtime_b->PostToIo(std::move(task));
          }
        },
        [this](std::chrono::milliseconds delay, std::function<void()> task) {
          if (harness_->runtime_b && task) {
            harness_->runtime_b->PostAfter(delay, std::move(task));
          }
        });
    a_chat_->Start();
    b_chat_->Start();
  }

  void TearDown() override {
    if (a_chat_) {
      a_chat_->Stop();
    }
    if (b_chat_) {
      b_chat_->Stop();
    }
    a_chat_.reset();
    b_chat_.reset();
    if (circuit_a_) {
      circuit_a_->Stop();
    }
    if (circuit_r_) {
      circuit_r_->Stop();
    }
    circuit_a_.reset();
    circuit_r_.reset();
    hops_.reset();
    chat_a_.reset();
    chat_b_.reset();
    harness_.reset();
  }

  template <typename Result>
  struct Wait {
    using Failure = typename WaitFailure<Result>::type;
    using RoeT = pp::ResultOrError<Result, Failure>;
    std::atomic<bool> done{false};
    RoeT result = Pending();

    static Failure Pending() {
      Failure pending;
      pending.message = "pending";
      return pending;
    }

    std::function<void(RoeT)> Fn() {
      return [this](RoeT r) {
        result = std::move(r);
        done.store(true, std::memory_order_release);
      };
    }

    pp::amp::PeerLinkManager::LinkCb LinkFn() {
      return [this](pp::amp::PeerLinkManager::LinkRoe r) {
        if (r) {
          result = Roe<void>();
        } else {
          result = Error(r.error().message);
        }
        done.store(true, std::memory_order_release);
      };
    }

    void PumpUntilDone(mesh::test::AmpMeshTripleHarness& harness, const size_t max_rounds = 2000) {
      harness.PumpUntil([this] { return done.load(std::memory_order_acquire); }, max_rounds);
      ASSERT_TRUE(done.load(std::memory_order_acquire));
    }
  };

  Roe<void> EstablishNestedChatPath() {
    mesh::CircuitBridgeTarget target;
    target.target_peer_id = harness_->peer_id_b;
    target.target_multiaddr = harness_->ma_b;
    target.target_protocol = pp::amp::kAmpCircuitCarrierProtocolId;

    Wait<mesh::CircuitTunnelBridgeResult> bridge_wait;
    auto tunnel_id = circuit_a_->StartBridge("relay", target, {}, {}, bridge_wait.Fn(), 8000);
    if (!tunnel_id) {
      return Error("start bridge failed");
    }
    bridge_wait.PumpUntilDone(*harness_);
    if (!bridge_wait.result) {
      return Error(bridge_wait.result.error().message);
    }
    if (!bridge_wait.result->session) {
      return Error("bridge without a session");
    }

    Wait<void> nested_wait;
    harness_->mgr_a().EstablishNestedOverCarrier(
        harness_->peer_id_b, bridge_wait.result->session, true, nested_wait.LinkFn());
    nested_wait.PumpUntilDone(*harness_);
    if (!nested_wait.result) {
      return nested_wait.result.error();
    }
    if (!harness_->mgr_a().IsConnected(harness_->peer_id_b)) {
      return Error("nested link not connected on A");
    }
    (void)hops_->Install(harness_->peer_id_b, "relay", pp::amp::kAmpCircuitCarrierProtocolId,
                         bridge_wait.result->session, tunnel_id);
    return {};
  }

  std::unique_ptr<mesh::test::AmpMeshTripleHarness> harness_;
  std::unique_ptr<mesh::IChatPeerLinks> chat_a_;
  std::unique_ptr<mesh::IChatPeerLinks> chat_b_;
  std::unique_ptr<mesh::AmpCircuitHopRegistry> hops_;
  std::unique_ptr<mesh::CircuitRelayServer> circuit_r_;
  std::unique_ptr<mesh::CircuitClientCoordinator> circuit_a_;
  std::unique_ptr<AmpDirectChatTransport> a_chat_;
  std::unique_ptr<AmpDirectChatTransport> b_chat_;
};

TEST_F(AmpDirectChatCircuitNestedTest, NestedConnectedWithoutEndpointIsReachableAndSends) {
  ASSERT_FALSE(harness_->mgr_a().GetLinkSnapshot(harness_->peer_id_b).has_endpoint);
  ASSERT_FALSE(a_chat_->IsPeerReachable(harness_->peer_id_b));

  auto nested = EstablishNestedChatPath();
  ASSERT_TRUE(nested) << nested.error().message;
  ASSERT_TRUE(harness_->mgr_a().IsConnected(harness_->peer_id_b));
  ASSERT_FALSE(harness_->mgr_a().GetLinkSnapshot(harness_->peer_id_b).has_endpoint)
      << "nested path must not require RegisterEndpoint for B";
  ASSERT_TRUE(a_chat_->IsPeerReachable(harness_->peer_id_b));

  std::mutex mu;
  std::condition_variable cv;
  bool got = false;
  RelayEnvelope received;
  b_chat_->SetInboundHandler([&](RelayEnvelope envelope, std::string /*from_peer_id*/) {
    std::lock_guard lock(mu);
    received = std::move(envelope);
    got = true;
    cv.notify_one();
  });

  const auto sent = a_chat_->SendEnvelope(harness_->peer_id_b, MakeTestEnvelope("msg-nested", "via hop"));
  ASSERT_TRUE(sent) << sent.error().message;

  {
    std::unique_lock lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return got; }));
  }
  EXPECT_EQ(received.message_id, "msg-nested");
}

} // namespace
} // namespace pbr
