#include "feature/calls/AmpCircuitHopReach.h"

#include "common/directory/MeshHopDial.h"
#include "domain/mesh/host/MeshPorts.h"
#include "domain/mesh/l4/circuit/AmpCircuitHopRegistry.h"
#include "domain/mesh/l4/circuit/CircuitTunnelCoordinator.h"
#include "domain/mesh/l4/media_relay/MediaRelayTypes.h"
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
 * - nested call-media must not PreferredMultiaddr the *target* (private punch MA poison);
 *   relay Preferred may be read to skip undialable wildcard bind (dogfood 084055)
 * - must not EnsureAssociation on a known-but-undialable endpoint before circuit
 * - relay lookup by PeerId (not only alias "hop"/"relay") must work when that key has an endpoint
 * - CollectDialableCircuitRelayIds must not RegisterEndpoint private hop MAs over public Preferred
 * - StartBridge must skip relays whose Preferred is /ip4/0.0.0.0 (capability ingest poison)
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

  void WhenChannelOpen(const std::string& peer_key, uint32_t channel_id, int64_t deadline_ms,
                       std::function<void(bool ok)> done) override {
    inner_.WhenChannelOpen(peer_key, channel_id, deadline_ms, std::move(done));
  }

  std::shared_ptr<pp::amp::ChannelSession> BindChannel(const std::string& peer_key, uint32_t channel_id,
                                                       pp::amp::ChannelPolicy policy,
                                                       pp::amp::ChannelSession::FrameHandler on_frame,
                                                       pp::amp::ChannelSession::ClosedCallback on_closed) override {
    return inner_.BindChannel(peer_key, channel_id, std::move(policy), std::move(on_frame), std::move(on_closed));
  }

  pp::amp::LinkSnapshotEx SnapshotByPeerId(const std::string& peer_id) const override {
    return inner_.SnapshotByPeerId(peer_id);
  }

  bool IsReachable(const std::string& peer_id) const override { return inner_.IsReachable(peer_id); }

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

    chat_a_ = NewAmpChatPeerLinks(*harness_->runtime_a);
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
  EXPECT_NE(recording_->last_preferred_peer, harness_->peer_id_b)
      << "nested call-media must not PreferredMultiaddr the target (private punch MA poison)";
  EXPECT_GE(recording_->nested_over_carrier_calls, 1);
}

TEST_F(AmpCircuitHopReachTest, CallMediaEnsureSucceedsDespiteDialablePeerInDialBackoff) {
  // Dogfood two-net: peer has_endpoint (dialable) but ADP dial is in backoff. Product Ensure must
  // still reach Connected via nested circuit and must not call EnsureAssociation (hard-w5 Phase-2).
  WarmAnswererAndOfferer("relay");

  const std::string private_ma =
      "/ip4/10.255.255.9/udp/9/adp/1.0.0/p2p/" + harness_->peer_id_b;
  ASSERT_TRUE(static_cast<bool>(harness_->mgr_a().RegisterEndpoint(harness_->peer_id_b, private_ma)));
  ASSERT_TRUE(recording_->GetLinkSnapshot(harness_->peer_id_b).has_endpoint);
  ASSERT_FALSE(recording_->IsConnected(harness_->peer_id_b));

  // If EnsureAssociation were called, return dial-in-backoff (poison path CallMediaBridge used to hammer).
  class BackoffChatPeerLinks final : public IChatPeerLinks {
  public:
    explicit BackoffChatPeerLinks(RecordingChatPeerLinks& inner) : inner_(inner) {}
    std::optional<std::string> PreferredMultiaddr(const std::string& peer_id) const override {
      return inner_.PreferredMultiaddr(peer_id);
    }
    Roe<void> RegisterEndpoint(const std::string& peer_key, const std::string& multiaddr) override {
      return inner_.RegisterEndpoint(peer_key, multiaddr);
    }
    void EnsureAssociation(const std::string& peer_key, LinkCb on_complete) override {
      ++ensure_backoff_calls;
      last_ensure_peer = peer_key;
      if (on_complete) {
        on_complete(LinkRoe::error(Failure::Of(Err::DialInBackoff, "amp link: dial in backoff")));
      }
    }
    void OpenChannel(const std::string& peer_key, const std::string& protocol_id,
                     pp::amp::ChannelPolicy policy, ChannelCb on_complete) override {
      inner_.OpenChannel(peer_key, protocol_id, std::move(policy), std::move(on_complete));
    }
    void EstablishNestedOverCarrier(const std::string& peer_key,
                                    std::shared_ptr<pp::amp::ChannelSession> carrier, bool initiator,
                                    LinkCb on_complete) override {
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
    pp::amp::LinkSnapshotEx SnapshotByPeerId(const std::string& peer_id) const override {
      return inner_.SnapshotByPeerId(peer_id);
    }
    bool IsConnected(const std::string& peer_key) const override { return inner_.IsConnected(peer_key); }
    bool IsReachable(const std::string& peer_id) const override { return inner_.IsReachable(peer_id); }
    void MarkWarm(const std::string& peer_key) override { inner_.MarkWarm(peer_key); }
    void WhenChannelOpen(const std::string& peer_key, uint32_t channel_id, int64_t deadline_ms,
                         std::function<void(bool ok)> done) override {
      inner_.WhenChannelOpen(peer_key, channel_id, deadline_ms, std::move(done));
    }
    std::shared_ptr<pp::amp::ChannelSession> BindChannel(
        const std::string& peer_key, uint32_t channel_id, pp::amp::ChannelPolicy policy,
        pp::amp::ChannelSession::FrameHandler on_frame,
        pp::amp::ChannelSession::ClosedCallback on_closed) override {
      return inner_.BindChannel(peer_key, channel_id, std::move(policy), std::move(on_frame),
                                std::move(on_closed));
    }

    RecordingChatPeerLinks& inner_;
    int ensure_backoff_calls = 0;
    std::string last_ensure_peer;
  };

  BackoffChatPeerLinks backoff_links(*recording_);
  AmpCircuitHopReach reach(
      *circuit_a_, *hops_, backoff_links, [this] { harness_->PumpAll(); },
      [](const std::string&) { return std::vector<std::string>{"relay"}; },
      [](const std::string&, std::function<void(Roe<void>)> on_done) {
        on_done(Error("punch burst dial timed out"));
      });

  Wait<void> ensure_wait;
  reach.TryEnsureCallMediaReachableAsync(harness_->peer_id_b, ensure_wait.Fn());
  ensure_wait.PumpUntilDone(*harness_);
  ASSERT_TRUE(ensure_wait.result) << ensure_wait.result.error().message;
  EXPECT_TRUE(backoff_links.IsConnected(harness_->peer_id_b));
  EXPECT_EQ(backoff_links.ensure_backoff_calls, 0)
      << "call-media Ensure must not ADP-dial a dialable-but-backoff peer (prefer circuit)";
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
  EXPECT_NE(recording_->last_preferred_peer, harness_->peer_id_b);
}

TEST_F(AmpCircuitHopReachTest, PrivateHopMaDoesNotPoisonPublicPreferred) {
  // Dogfood: CollectDialableCircuitRelayIds used to RegisterEndpoint directory private hop MAs
  // over a seed-warmed public Preferred → StartBridge sendto fail. Gate keeps public Preferred;
  // seed already has_endpoint so EnsureViaCircuit still dials the MemoryIo path (ma_r).
  WarmAnswererAndOfferer(harness_->peer_id_r);
  ASSERT_TRUE(recording_->GetLinkSnapshot(harness_->peer_id_r).has_endpoint);

  const std::string public_ma =
      "/ip4/203.0.113.50/udp/4001/adp/1.0.0/p2p/" + harness_->peer_id_r;
  const std::string private_hop_ma =
      "/ip4/10.255.255.7/udp/9/adp/1.0.0/p2p/" + harness_->peer_id_r;
  ASSERT_TRUE(static_cast<bool>(harness_->mgr_a().RegisterEndpoint(harness_->peer_id_r, public_ma)));
  ASSERT_EQ(recording_->PreferredMultiaddr(harness_->peer_id_r).value_or(""), public_ma);
  EXPECT_FALSE(CircuitHopDialBookAllowsRegister(private_hop_ma));
  EXPECT_TRUE(CircuitHopDialBookAllowsRegister(public_ma));

  // Same gate CallMediaPlane::CollectDialableCircuitRelayIds uses.
  if (IsAdpMultiaddr(private_hop_ma) && CircuitHopDialBookAllowsRegister(private_hop_ma)) {
    (void)harness_->mgr_a().RegisterEndpoint(harness_->peer_id_r, private_hop_ma);
  }
  EXPECT_EQ(recording_->PreferredMultiaddr(harness_->peer_id_r).value_or(""), public_ma)
      << "private hop MA must not overwrite public PreferredMultiaddr";

  // Restore MemoryIo-dialable relay MA (public_ma is not on the harness fabric).
  ASSERT_TRUE(static_cast<bool>(harness_->mgr_a().RegisterEndpoint(harness_->peer_id_r, harness_->ma_r)));

  AmpCircuitHopReach reach(
      *circuit_a_, *hops_, *recording_, [this] { harness_->PumpAll(); },
      [this](const std::string& exclude) {
        std::vector<std::string> out;
        if (harness_->peer_id_r != exclude) {
          out.push_back(harness_->peer_id_r);
        }
        return out;
      },
      [](const std::string&, std::function<void(Roe<void>)> on_done) {
        on_done(Error("punch burst dial timed out"));
      });

  Wait<void> ensure_wait;
  reach.TryEnsureCallMediaReachableAsync(harness_->peer_id_b, ensure_wait.Fn());
  ensure_wait.PumpUntilDone(*harness_);
  ASSERT_TRUE(ensure_wait.result) << ensure_wait.result.error().message;
  EXPECT_TRUE(recording_->IsConnected(harness_->peer_id_b));
}

TEST_F(AmpCircuitHopReachTest, SkipsWildcardPreferredRelayThenUsesDialable) {
  // Dogfood 084055: capability ingest left Preferred=/ip4/0.0.0.0 on seed; StartBridge sendto
  // then AV on next relay. Skip undialable Preferred and advance to a dialable hop.
  WarmAnswererAndOfferer(harness_->peer_id_r);
  const std::string wildcard_ma =
      "/ip4/0.0.0.0/udp/53523/adp/1.0.0/p2p/" + harness_->peer_id_r;
  ASSERT_TRUE(static_cast<bool>(harness_->mgr_a().RegisterEndpoint(harness_->peer_id_r, wildcard_ma)));
  ASSERT_EQ(recording_->PreferredMultiaddr(harness_->peer_id_r).value_or(""), wildcard_ma);
  EXPECT_FALSE(CircuitHopMultiaddrIsUdpDialable(wildcard_ma));

  // Alias "relay" keeps MemoryIo-dialable MA so StartBridge can succeed after skip.
  ASSERT_TRUE(static_cast<bool>(harness_->mgr_a().RegisterEndpoint("relay", harness_->ma_r)));
  ASSERT_TRUE(CircuitHopMultiaddrIsUdpDialable(
      recording_->PreferredMultiaddr("relay").value_or("")));

  AmpCircuitHopReach reach(
      *circuit_a_, *hops_, *recording_, [this] { harness_->PumpAll(); },
      [this](const std::string& exclude) {
        std::vector<std::string> out;
        if (harness_->peer_id_r != exclude) {
          out.push_back(harness_->peer_id_r);
        }
        if ("relay" != exclude) {
          out.push_back("relay");
        }
        return out;
      },
      [](const std::string&, std::function<void(Roe<void>)> on_done) {
        on_done(Error("punch burst dial timed out"));
      });

  Wait<void> ensure_wait;
  reach.TryEnsureCallMediaReachableAsync(harness_->peer_id_b, ensure_wait.Fn());
  ensure_wait.PumpUntilDone(*harness_);
  ASSERT_TRUE(ensure_wait.result) << ensure_wait.result.error().message;
  EXPECT_TRUE(recording_->IsConnected(harness_->peer_id_b));
}

TEST_F(AmpCircuitHopReachTest, CallMediaEnsureRunsCircuitBeforePunch) {
  // Dogfood 130521: punch∥circuit overlapped ADP OpenChannel after sendto-miss and AVd.
  // Circuit must run first; punch stays idle while nested circuit succeeds.
  WarmAnswererAndOfferer("relay");

  auto punch_started = std::make_shared<std::atomic<bool>>(false);
  auto hold_punch = std::make_shared<std::function<void(Roe<void>)>>();

  AmpCircuitHopReach reach(
      *circuit_a_, *hops_, *recording_, [this] { harness_->PumpAll(); },
      [](const std::string&) { return std::vector<std::string>{"relay"}; },
      [punch_started, hold_punch](const std::string&, std::function<void(Roe<void>)> on_done) {
        punch_started->store(true, std::memory_order_release);
        *hold_punch = std::move(on_done);
      });

  Wait<void> ensure_wait;
  reach.TryEnsureCallMediaReachableAsync(harness_->peer_id_b, ensure_wait.Fn());
  ensure_wait.PumpUntilDone(*harness_);
  ASSERT_TRUE(ensure_wait.result) << ensure_wait.result.error().message;
  EXPECT_TRUE(recording_->IsConnected(harness_->peer_id_b));
  EXPECT_FALSE(punch_started->load(std::memory_order_acquire))
      << "successful circuit must not start punch (no ADP overlap)";
  EXPECT_GE(recording_->nested_over_carrier_calls, 1);
  EXPECT_FALSE(hold_punch && *hold_punch);
}

TEST_F(AmpCircuitHopReachTest, AbortPendingSkipsPunchFallback) {
  // Leave / ConnectFailed must not fall through to punch after an aborted circuit miss.
  WarmAnswererAndOfferer("relay");

  auto punch_started = std::make_shared<std::atomic<bool>>(false);
  auto hold_punch = std::make_shared<std::function<void(Roe<void>)>>();

  AmpCircuitHopReach reach(
      *circuit_a_, *hops_, *recording_, AmpCircuitHopReach::IoPump{},
      [](const std::string&) {
        // Force circuit miss without StartBridge so Abort can win before any tunnel work.
        return std::vector<std::string>{};
      },
      [punch_started, hold_punch](const std::string&, std::function<void(Roe<void>)> on_done) {
        punch_started->store(true, std::memory_order_release);
        *hold_punch = std::move(on_done);
      });

  Wait<void> ensure_wait;
  reach.TryEnsureCallMediaReachableAsync(harness_->peer_id_b, ensure_wait.Fn());
  // Empty relay list fails circuit synchronously, then starts punch (held). Abort before punch
  // completion — finish must report aborted and must not treat punch as success.
  ASSERT_TRUE(punch_started->load(std::memory_order_acquire));
  ASSERT_TRUE(hold_punch && *hold_punch);
  reach.AbortPending();
  (*hold_punch)(Roe<void>());
  ASSERT_TRUE(ensure_wait.done.load(std::memory_order_acquire));
  ASSERT_FALSE(ensure_wait.result);
  EXPECT_NE(ensure_wait.result.error().message.find("aborted"), std::string::npos)
      << ensure_wait.result.error().message;
}

/**
 * L3.25 SoftMigrate path (H002): punch epoch miss → circuit fallback.
 * TryEnsureHopReachable runs punch first; on window expiry / no dialable book entry,
 * EnsureViaCircuit must still Install a media_relay hop.
 */
TEST_F(AmpCircuitHopReachTest, HopEnsureFallsThroughToCircuitAfterPunchWindowExpiry) {
  WarmAnswererAndOfferer("relay");

  ASSERT_FALSE(recording_->GetLinkSnapshot(harness_->peer_id_b).has_endpoint);
  ASSERT_FALSE(hops_->Find(harness_->peer_id_b, kMediaRelayProtocolId).has_value());

  auto punch_calls = std::make_shared<int>(0);
  AmpCircuitHopReach reach(
      *circuit_a_, *hops_, *recording_, [this] { harness_->PumpAll(); },
      [](const std::string&) { return std::vector<std::string>{"relay"}; },
      [punch_calls](const std::string&, std::function<void(Roe<void>)> on_done) {
        ++(*punch_calls);
        on_done(Error("punch burst window expired"));
      });

  Wait<void> ensure_wait;
  reach.TryEnsureHopReachableAsync(harness_->peer_id_b, ensure_wait.Fn());
  ensure_wait.PumpUntilDone(*harness_);
  ASSERT_TRUE(ensure_wait.result) << ensure_wait.result.error().message;
  EXPECT_EQ(*punch_calls, 1);
  EXPECT_TRUE(hops_->Find(harness_->peer_id_b, kMediaRelayProtocolId).has_value())
      << "circuit fallback must Install media_relay hop after punch window expiry";
  EXPECT_TRUE(recording_->GetLinkSnapshot(harness_->peer_id_b).has_endpoint)
      << "circuit fallback registers PeerId endpoint for SoftMigrate dialability";
}

} // namespace
} // namespace pbr
