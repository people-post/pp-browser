#include "feature/calls/AmpCircuitHopReach.h"

#include "domain/mesh/host/MeshPorts.h"
#include "domain/mesh/l4/circuit/AmpCircuitHopRegistry.h"
#include "domain/mesh/l4/circuit/CircuitTunnelCoordinator.h"
#include "domain/mesh/tests/support/mesh_triple_harness.h"

#include <gtest/gtest.h>
#include <sodium.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pbr {
namespace {

/**
 * hard-w5 Phase-2 policy locks for AmpCircuitHopReach:
 * - nested call-media must not call PreferredMultiaddr (private punch MA poison)
 * - must not EnsureAssociation on a known-but-undialable endpoint before circuit
 * - relay lookup by PeerId (not only alias "hop"/"relay") must work when that key has an endpoint
 */
class RecordingChatPeerLinks final : public IChatPeerLinks {
public:
  explicit RecordingChatPeerLinks(IChatPeerLinks& inner) : inner_(inner) {}

  std::optional<std::string> PreferredMultiaddr(const std::string& peer_id) const override {
    ++preferred_multiaddr_calls;
    last_preferred_peer = peer_id;
    return inner_.PreferredMultiaddr(peer_id);
  }

  Roe<void> RegisterEndpoint(const std::string& peer_key, const std::string& multiaddr) override {
    return inner_.RegisterEndpoint(peer_key, multiaddr);
  }

  void EnsureAssociation(const std::string& peer_key, LinkCb on_complete) override {
    ++ensure_association_calls;
    last_ensure_peer = peer_key;
    inner_.EnsureAssociation(peer_key, std::move(on_complete));
  }

  void OpenChannel(const std::string& peer_key, const std::string& protocol_id, pp::amp::ChannelPolicy policy,
                   ChannelCb on_complete) override {
    inner_.OpenChannel(peer_key, protocol_id, std::move(policy), std::move(on_complete));
  }

  void EstablishNestedOverCarrier(const std::string& peer_key, std::shared_ptr<pp::amp::ChannelSession> carrier,
                                  bool initiator, LinkCb on_complete) override {
    ++nested_over_carrier_calls;
    inner_.EstablishNestedOverCarrier(peer_key, std::move(carrier), initiator, std::move(on_complete));
  }

  void SetProtocolHandler(const std::string& protocol_id, ProtocolHandler handler) override {
    inner_.SetProtocolHandler(protocol_id, std::move(handler));
  }

  void RemoveProtocolHandler(const std::string& protocol_id) override {
    inner_.RemoveProtocolHandler(protocol_id);
  }

  MeshPeerLinkSnapshot GetLinkSnapshot(const std::string& peer_key) const override {
    return inner_.GetLinkSnapshot(peer_key);
  }

  bool IsConnected(const std::string& peer_key) const override { return inner_.IsConnected(peer_key); }

  void MarkWarm(const std::string& peer_key) override { inner_.MarkWarm(peer_key); }

  pp::amp::PeerLink* FindLink(const std::string& peer_key) override { return inner_.FindLink(peer_key); }

  const pp::amp::PeerLink* FindLink(const std::string& peer_key) const override {
    return inner_.FindLink(peer_key);
  }

  mutable int preferred_multiaddr_calls = 0;
  mutable std::string last_preferred_peer;
  int ensure_association_calls = 0;
  std::string last_ensure_peer;
  int nested_over_carrier_calls = 0;

private:
  IChatPeerLinks& inner_;
};

class AmpCircuitHopReachTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_GE(sodium_init(), 0);
    auto created = pbr::test::AmpMeshTripleHarness::Create();
    ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
    harness_ = std::move(*created);

    ASSERT_TRUE(static_cast<bool>(harness_->mgr_a().RegisterEndpoint("relay", harness_->ma_r)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_a().RegisterEndpoint(harness_->peer_id_r, harness_->ma_r)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_r().RegisterEndpoint("a", harness_->ma_a)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_r().RegisterEndpoint("b", harness_->ma_b)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_r().RegisterEndpoint(harness_->peer_id_b, harness_->ma_b)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_b().RegisterEndpoint("relay", harness_->ma_r)));

    harness_->mgr_a().EnableNestedCarrierAccept(true);
    harness_->mgr_b().EnableNestedCarrierAccept(true);

    chat_a_ = NewAmpChatPeerLinks(harness_->mgr_a());
    recording_ = std::make_unique<RecordingChatPeerLinks>(*chat_a_);
    hops_ = std::make_unique<AmpCircuitHopRegistry>();
    circuit_r_ = std::make_unique<CircuitTunnelCoordinator>(*harness_->runtime_r);
    circuit_a_ = std::make_unique<CircuitTunnelCoordinator>(*harness_->runtime_a);
    circuit_r_->Start();
    circuit_r_->SetServeInbound(true);
    circuit_a_->Start();
    circuit_a_->SetServeInbound(false);
  }

  void TearDown() override {
    if (circuit_a_) {
      circuit_a_->Stop();
    }
    if (circuit_r_) {
      circuit_r_->Stop();
    }
    circuit_a_.reset();
    circuit_r_.reset();
    hops_.reset();
    recording_.reset();
    chat_a_.reset();
    harness_.reset();
  }

  template <typename Result>
  struct Wait {
    std::atomic<bool> done{false};
    Roe<Result> result = Error("pending");

    std::function<void(Roe<Result>)> Fn() {
      return [this](Roe<Result> r) {
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

    void PumpUntilDone(pbr::test::AmpMeshTripleHarness& harness, const size_t max_rounds = 2500) {
      harness.PumpUntil([this] { return done.load(std::memory_order_acquire); }, max_rounds);
      ASSERT_TRUE(done.load(std::memory_order_acquire));
    }
  };

  void WarmAnswererAndOfferer(const std::string& hop_key) {
    Wait<void> b_assoc;
    harness_->mgr_b().EnsureAssociation("relay", b_assoc.LinkFn());
    b_assoc.PumpUntilDone(*harness_);
    ASSERT_TRUE(b_assoc.result) << b_assoc.result.error().message;

    Wait<void> a_assoc;
    harness_->mgr_a().EnsureAssociation(hop_key, a_assoc.LinkFn());
    a_assoc.PumpUntilDone(*harness_);
    ASSERT_TRUE(a_assoc.result) << a_assoc.result.error().message;
  }

  std::unique_ptr<pbr::test::AmpMeshTripleHarness> harness_;
  std::unique_ptr<IChatPeerLinks> chat_a_;
  std::unique_ptr<RecordingChatPeerLinks> recording_;
  std::unique_ptr<AmpCircuitHopRegistry> hops_;
  std::unique_ptr<CircuitTunnelCoordinator> circuit_r_;
  std::unique_ptr<CircuitTunnelCoordinator> circuit_a_;
};

TEST_F(AmpCircuitHopReachTest, CallMediaEnsureSkipsEnsureAssociationAndPreferredMultiaddr) {
  WarmAnswererAndOfferer("relay");

  const std::string private_ma =
      "/ip4/10.255.255.1/udp/9/adp/1.0.0/p2p/" + harness_->peer_id_b;
  ASSERT_TRUE(static_cast<bool>(harness_->mgr_a().RegisterEndpoint(harness_->peer_id_b, private_ma)));
  ASSERT_TRUE(recording_->GetLinkSnapshot(harness_->peer_id_b).has_endpoint);
  ASSERT_FALSE(recording_->IsConnected(harness_->peer_id_b));

  recording_->ensure_association_calls = 0;
  recording_->preferred_multiaddr_calls = 0;
  recording_->nested_over_carrier_calls = 0;

  AmpCircuitHopReach reach(
      *circuit_a_, *hops_, *recording_, [this] { harness_->PumpAll(); },
      [](const std::string&) { return std::vector<std::string>{"relay"}; },
      // Punch miss (expected under dual-NAT) — fall through to nested circuit.
      [](const std::string&, std::function<void(Roe<void>)> on_done) {
        on_done(Error("punch burst dial timed out"));
      });

  Wait<void> ensure_wait;
  reach.TryEnsureCallMediaReachableAsync(harness_->peer_id_b, ensure_wait.Fn());
  ensure_wait.PumpUntilDone(*harness_);
  ASSERT_TRUE(ensure_wait.result) << ensure_wait.result.error().message;
  EXPECT_TRUE(recording_->IsConnected(harness_->peer_id_b));

  EXPECT_EQ(recording_->ensure_association_calls, 0)
      << "must not dial private punch MA before circuit (hard-w5 Phase-2)";
  EXPECT_EQ(recording_->preferred_multiaddr_calls, 0)
      << "nested call-media must be peer-id-only (no PreferredMultiaddr poison)";
  EXPECT_GE(recording_->nested_over_carrier_calls, 1);
}

TEST_F(AmpCircuitHopReachTest, CallMediaEnsureAcceptsHopPeerIdRelayKey) {
  WarmAnswererAndOfferer(harness_->peer_id_r);
  ASSERT_TRUE(recording_->GetLinkSnapshot(harness_->peer_id_r).has_endpoint);

  recording_->preferred_multiaddr_calls = 0;

  AmpCircuitHopReach reach(
      *circuit_a_, *hops_, *recording_, [this] { harness_->PumpAll(); },
      [this](const std::string& exclude) {
        std::vector<std::string> out;
        if (harness_->peer_id_r != exclude) {
          out.push_back(harness_->peer_id_r);
        }
        return out;
      },
      AmpCircuitHopReach::TryPunchAsync{});

  Wait<void> ensure_wait;
  reach.TryEnsureCallMediaReachableAsync(harness_->peer_id_b, ensure_wait.Fn());
  ensure_wait.PumpUntilDone(*harness_);
  ASSERT_TRUE(ensure_wait.result) << ensure_wait.result.error().message;
  EXPECT_TRUE(recording_->IsConnected(harness_->peer_id_b));
  EXPECT_EQ(recording_->preferred_multiaddr_calls, 0);
}

} // namespace
} // namespace pbr
