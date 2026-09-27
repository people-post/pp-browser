#include "domain/mesh/l4/circuit/CircuitRelayTypes.h"
#include "domain/mesh/l4/circuit/AmpCircuitHopRegistry.h"
#include "domain/mesh/l4/call_media/CallMediaLegCoordinator.h"
#include "domain/mesh/l4/circuit/CircuitTunnelCoordinator.h"
#include "amp/link/Types.h"
#include "domain/mesh/tests/support/mesh_triple_harness.h"

#include <gtest/gtest.h>
#include <sodium.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pbr {
namespace {

struct LegCompletion {
  std::atomic<bool> finished{false};
  Roe<void> result = Error("pending");

  CallMediaLegCoordinator::LegFinished Fn() {
    return [this](Roe<void> r) {
      result = std::move(r);
      finished.store(true, std::memory_order_release);
    };
  }

  void PumpUntilDone(pbr::test::AmpMeshTripleHarness& harness, const size_t max_rounds = 2500) {
    harness.PumpUntil([this] { return finished.load(std::memory_order_acquire); }, max_rounds);
    ASSERT_TRUE(finished.load(std::memory_order_acquire)) << "leg completion timed out";
  }
};

/**
 * A has no ADP path to B; 1:1 call-media uses nested Session over Amp circuit ([A024]).
 */
class AmpCircuitCallMediaComposeTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_GE(sodium_init(), 0);
    auto created = pbr::test::AmpMeshTripleHarness::Create();
    ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
    harness_ = std::move(*created);

    // A↔R and R↔B only — A must not dial B directly.
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_a().RegisterEndpoint("relay", harness_->ma_r)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_r().RegisterEndpoint("a", harness_->ma_a)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_r().RegisterEndpoint("b", harness_->ma_b)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_r().RegisterEndpoint(harness_->peer_id_b, harness_->ma_b)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_b().RegisterEndpoint("relay", harness_->ma_r)));

    harness_->mgr_a().EnableNestedCarrierAccept(true);
    harness_->mgr_b().EnableNestedCarrierAccept(true);

    hops_ = std::make_unique<AmpCircuitHopRegistry>();
    circuit_r_ = std::make_unique<CircuitTunnelCoordinator>(*harness_->runtime_r);
    circuit_a_ = std::make_unique<CircuitTunnelCoordinator>(*harness_->runtime_a);
    a_call_ = std::make_unique<CallMediaLegCoordinator>(*harness_->runtime_a);
    b_call_ = std::make_unique<CallMediaLegCoordinator>(*harness_->runtime_b);

    circuit_r_->Start();
    circuit_r_->SetServeInbound(true);
    circuit_a_->Start();
    circuit_a_->SetServeInbound(false);
    a_call_->Start();
    b_call_->Start();
  }

  void TearDown() override {
    if (a_call_) {
      a_call_->Stop();
    }
    if (b_call_) {
      b_call_->Stop();
    }
    if (circuit_a_) {
      circuit_a_->Stop();
    }
    if (circuit_r_) {
      circuit_r_->Stop();
    }
    a_call_.reset();
    b_call_.reset();
    circuit_a_.reset();
    circuit_r_.reset();
    hops_.reset();
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

    void PumpUntilDone(pbr::test::AmpMeshTripleHarness& harness, const size_t max_rounds = 2000) {
      harness.PumpUntil([this] { return done.load(std::memory_order_acquire); }, max_rounds);
      ASSERT_TRUE(done.load(std::memory_order_acquire));
    }
  };

  /**
   * @param peer_id_only when true, omit target_multiaddr (hop must already know B — warm path).
   * @param override_multiaddr when set (and not peer_id_only), send this MA to the hop (poison probe).
   */
  Roe<void> EstablishNestedCallMediaPath(bool peer_id_only = false,
                                         const std::string& override_multiaddr = {}) {
    CircuitBridgeTarget target;
    target.target_peer_id = harness_->peer_id_b;
    if (!peer_id_only) {
      target.target_multiaddr = override_multiaddr.empty() ? harness_->ma_b : override_multiaddr;
    }
    target.target_protocol = pp::amp::kAmpCircuitCarrierProtocolId;

    Wait<CircuitTunnelBridgeResult> bridge_wait;
    auto tunnel_id = circuit_a_->StartBridge("relay", target, {}, {}, bridge_wait.Fn(), 8000);
    if (!tunnel_id) {
      return Error("start bridge failed");
    }
    bridge_wait.PumpUntilDone(*harness_);
    if (!bridge_wait.result) {
      return bridge_wait.result.error();
    }
    if (!bridge_wait.result->ok || !bridge_wait.result->session) {
      return Error(bridge_wait.result->error.empty() ? "bridge refused" : bridge_wait.result->error);
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

  std::unique_ptr<pbr::test::AmpMeshTripleHarness> harness_;
  std::unique_ptr<AmpCircuitHopRegistry> hops_;
  std::unique_ptr<CircuitTunnelCoordinator> circuit_r_;
  std::unique_ptr<CircuitTunnelCoordinator> circuit_a_;
  std::unique_ptr<CallMediaLegCoordinator> a_call_;
  std::unique_ptr<CallMediaLegCoordinator> b_call_;
};

TEST_F(AmpCircuitCallMediaComposeTest, CircuitNestedHelloAndEncryptedAudioRoundTrip) {
  ASSERT_FALSE(harness_->mgr_a().GetLinkSnapshot(harness_->peer_id_b).has_endpoint);
  ASSERT_FALSE(harness_->mgr_a().IsConnected(harness_->peer_id_b));

  auto nested = EstablishNestedCallMediaPath();
  ASSERT_TRUE(nested) << nested.error().message;
  ASSERT_TRUE(harness_->mgr_a().IsConnected(harness_->peer_id_b));
  ASSERT_TRUE(harness_->mgr_b().FindLinkByPeerId(harness_->peer_id_a) != nullptr);

  const std::string call_id = "call-amp-circuit-nested";
  ByteVector media_key(32, 0x42);

  std::mutex mu;
  std::condition_variable cv;
  bool answerer_connected = false;
  bool got_audio = false;
  std::vector<uint8_t> received;

  b_call_->SetInboundHandler(AnswerInline([&](CallMediaDirectConnectParams& params, CallMediaDirectCallbacks& cbs) {
    params.media_key = media_key;
    params.call_id = call_id;
    params.media_epoch = 1;
    params.offerer = false;
    cbs.on_connected = [&] {
      std::lock_guard lock(mu);
      answerer_connected = true;
      cv.notify_one();
    };
    cbs.on_audio = [&](const std::vector<uint8_t>& opus) {
      std::lock_guard lock(mu);
      received = opus;
      got_audio = true;
      cv.notify_one();
    };
  }));

  CallMediaDirectConnectParams params;
  params.peer_key = harness_->peer_id_b;
  params.call_id = call_id;
  params.media_epoch = 1;
  params.media_key = media_key;
  params.offerer = true;

  std::atomic<bool> offerer_connected{false};
  CallMediaDirectCallbacks cbs;
  cbs.on_connected = [&] {
    offerer_connected.store(true, std::memory_order_release);
    cv.notify_one();
  };

  LegCompletion leg_done;
  const CallMediaLegId leg_id = a_call_->StartLeg(params, std::move(cbs), leg_done.Fn(), 8000);
  ASSERT_TRUE(leg_id);

  leg_done.PumpUntilDone(*harness_);
  ASSERT_TRUE(leg_done.result) << leg_done.result.error().message;

  harness_->PumpUntil(
      [&] { return answerer_connected && offerer_connected.load(std::memory_order_acquire); }, 2500);
  ASSERT_TRUE(answerer_connected);
  ASSERT_TRUE(offerer_connected.load(std::memory_order_acquire));

  const std::vector<uint8_t> opus = {0xca, 0xfe, 0xba, 0xbe};
  auto sent = a_call_->SendAudio(leg_id, opus, 1, 0);
  ASSERT_TRUE(sent) << sent.error().message;

  harness_->PumpUntil([&] { return got_audio; }, 2500);
  EXPECT_EQ(received, opus);
  EXPECT_EQ(a_call_->Phase(), CallMediaSessionPhase::MediaReady);
  EXPECT_EQ(b_call_->Phase(), CallMediaSessionPhase::MediaReady);
  // Path label truth: both ends report the relay carrier (answerer showed "Punched" — dogfood 2026-09-24).
  EXPECT_EQ(a_call_->ActiveLinkKind(), CallMediaLinkKind::Relayed);
  EXPECT_EQ(b_call_->ActiveLinkKind(), CallMediaLinkKind::Relayed);

  a_call_->DetachLeg(leg_id);
  harness_->PumpUntil([&] { return a_call_->Phase() == CallMediaSessionPhase::Idle; }, 500);
  EXPECT_EQ(a_call_->Phase(), CallMediaSessionPhase::Idle);
}

// A live relayed call must keep caller→callee audio through the disturbances a real call sees:
// reservation renewals (k2), reserves aimed at the callee, lease expiry, burst loss on the relay hop.
// Dogfood 2026-09-24 16:17 lost that direction mid-call; these ruled out each local cause.
class RelayedCallDisturbanceTest : public AmpCircuitCallMediaComposeTest {
protected:
  bool LiveCall() {
    auto nested = EstablishNestedCallMediaPath();
    if (!nested) {
      ADD_FAILURE() << nested.error().message;
      return false;
    }
    b_call_->SetInboundHandler(AnswerInline([this](CallMediaDirectConnectParams& params, CallMediaDirectCallbacks& cbs) {
      params.media_key = ByteVector(32, 0x42);
      params.call_id = "call-renew-probe";
      params.media_epoch = 1;
      params.offerer = false;
      cbs.on_connected = [this] { answerer_connected_ = true; };
      cbs.on_audio = [this](const std::vector<uint8_t>& opus) { received_ = opus; };
    }));
    CallMediaDirectConnectParams params;
    params.peer_key = harness_->peer_id_b;
    params.call_id = "call-renew-probe";
    params.media_epoch = 1;
    params.media_key = ByteVector(32, 0x42);
    params.offerer = true;
    CallMediaDirectCallbacks cbs;
    cbs.on_connected = [this] { offerer_connected_ = true; };
    leg_id_ = a_call_->StartLeg(params, std::move(cbs), leg_done_.Fn(), 8000);
    leg_done_.PumpUntilDone(*harness_);
    harness_->PumpUntil([&] { return answerer_connected_ && offerer_connected_.load(); }, 2500);
    return answerer_connected_ && offerer_connected_.load() && AudioReaches({0x01}, 1);
  }

  bool AudioReaches(const std::vector<uint8_t>& opus, uint32_t seq) {
    received_.clear();
    if (!a_call_->SendAudio(leg_id_, opus, seq, 0)) {
      return false;
    }
    harness_->PumpUntil([&] { return received_ == opus; }, 1500);
    return received_ == opus;
  }

  void Reserve(const std::string& relay_key) {
    Wait<CircuitTunnelBridgeResult> w;
    (void)circuit_a_->StartReserve(relay_key, w.Fn(), 15000);
    w.PumpUntilDone(*harness_, 1500);
  }

  CallMediaLegId leg_id_{};
  LegCompletion leg_done_;
  bool answerer_connected_ = false;
  std::atomic<bool> offerer_connected_{false};
  std::vector<uint8_t> received_;
};

TEST_F(RelayedCallDisturbanceTest, CallerReserveOnCarryingRelayKeepsAudio) {
  ASSERT_TRUE(LiveCall());
  Reserve("relay");
  EXPECT_TRUE(AudioReaches({0x02, 0x02}, 2)) << "caller→callee audio lost after reserve on the carrying relay";
}

// Dogfood timeline: both ends parked on R before the call; B runs the product circuit coordinator
// (not serving); A renews on R and (B41 gap) on B every 10 s while earlier leases expire.
TEST_F(RelayedCallDisturbanceTest, RenewalsOverTimeKeepAudioBothWays) {
  CircuitTunnelCoordinator circuit_b(*harness_->runtime_b);
  circuit_b.Start();
  circuit_b.SetServeInbound(false);
  {
    Wait<CircuitTunnelBridgeResult> wb;
    (void)circuit_b.StartReserve("relay", wb.Fn(), 15000);
    wb.PumpUntilDone(*harness_, 1500);
  }
  Reserve("relay");
  ASSERT_TRUE(LiveCall());

  std::vector<uint8_t> back;
  for (int round = 0; round < 4; ++round) {
    for (int i = 0; i < 40; ++i) {  // 10 s of Amp time
      harness_->clock->Advance(250);
      harness_->PumpAll();
    }
    Reserve("relay");
    Reserve(harness_->peer_id_b);
    {
      Wait<CircuitTunnelBridgeResult> wb;
      (void)circuit_b.StartReserve("relay", wb.Fn(), 15000);
      wb.PumpUntilDone(*harness_, 1500);
    }
    const uint8_t tag = static_cast<uint8_t>(0x10 + round);
    EXPECT_TRUE(AudioReaches({tag, tag}, 10u + static_cast<uint32_t>(round)))
        << "caller→callee audio lost after renewal round " << round;
    EXPECT_EQ(a_call_->Phase(), CallMediaSessionPhase::MediaReady) << "round " << round;
    EXPECT_EQ(b_call_->Phase(), CallMediaSessionPhase::MediaReady) << "round " << round;
  }
  circuit_b.Stop();
}

// Same, with real-time lease expiry (coordinator deadlines use steady_clock): 300 ms leases renewed
// every 200 ms, so superseded reservations expire and tear down on client and relay mid-call.
TEST_F(RelayedCallDisturbanceTest, LeaseExpiryDuringCallKeepsAudio) {
  CircuitTunnelCoordinator circuit_b(*harness_->runtime_b);
  circuit_b.Start();
  circuit_b.SetServeInbound(false);
  auto reserve = [&](CircuitTunnelCoordinator& c, const std::string& key) {
    Wait<CircuitTunnelBridgeResult> w;
    (void)c.StartReserve(key, w.Fn(), 300);
    w.PumpUntilDone(*harness_, 1500);
  };
  reserve(circuit_b, "relay");
  reserve(*circuit_a_, "relay");
  ASSERT_TRUE(LiveCall());
  for (int round = 0; round < 12; ++round) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (std::chrono::steady_clock::now() < until) {
      harness_->PumpAll();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    reserve(*circuit_a_, "relay");
    reserve(*circuit_a_, harness_->peer_id_b);
    reserve(circuit_b, "relay");
    const uint8_t tag = static_cast<uint8_t>(0x20 + round);
    ASSERT_TRUE(AudioReaches({tag, tag}, 20u + static_cast<uint32_t>(round)))
        << "caller→callee audio lost after lease round " << round;
  }
  circuit_b.Stop();
}

// ~2 s blackout on the relay hop (longer than ADP reliable retransmits) must not wedge one direction.
TEST_F(RelayedCallDisturbanceTest, BurstLossOnRelayHopRecovers) {
  ASSERT_TRUE(LiveCall());
  // Keep media flowing while R's sends are blacked out for 2 s of Amp time.
  harness_->io_r->SetDropRate(1.0);
  for (int i = 0; i < 40; ++i) {
    (void)a_call_->SendAudio(leg_id_, {0x55}, 100u + static_cast<uint32_t>(i), 0);
    harness_->clock->Advance(50);
    harness_->PumpAll();
  }
  harness_->io_r->SetDropRate(0.0);
  for (int i = 0; i < 40; ++i) {
    harness_->clock->Advance(50);
    harness_->PumpAll();
  }
  EXPECT_TRUE(AudioReaches({0x66, 0x66}, 200)) << "caller→callee audio did not recover after burst loss";
  EXPECT_EQ(b_call_->Phase(), CallMediaSessionPhase::MediaReady);
}

// Hard lab CGNAT stack (delay 80 ms + 1 % loss on the caller) / dogfood 2026-09-24 16:17: the hop
// bound the dialer's circuit channel as Control (Reliable, strict in-order) while the dialer sends
// the call-media carrier best-effort. One lost / reordered caller frame and the hop rejected every
// later one ("out of order seq"): caller→callee dead for the rest of the call, reverse fine.
TEST_F(RelayedCallDisturbanceTest, CallerUplinkLossDoesNotWedgeCallerToCallee) {
  ASSERT_TRUE(LiveCall());
  harness_->io_a->SetRngSeed(7);
  harness_->io_a->SetDropRate(0.2);
  for (int i = 0; i < 60; ++i) {
    (void)a_call_->SendAudio(leg_id_, {0x77}, 300u + static_cast<uint32_t>(i), 0);
    harness_->clock->Advance(20);
    harness_->PumpAll();
  }
  harness_->io_a->SetDropRate(0.0);
  EXPECT_TRUE(AudioReaches({0x78, 0x78}, 400)) << "caller→callee wedged after caller uplink loss";
  EXPECT_EQ(b_call_->Phase(), CallMediaSessionPhase::MediaReady);
}

TEST_F(RelayedCallDisturbanceTest, CallerReserveOnCalleeViaCarrierKeepsAudio) {
  ASSERT_TRUE(LiveCall());
  Reserve(harness_->peer_id_b);
  EXPECT_TRUE(AudioReaches({0x03, 0x03}, 3)) << "caller→callee audio lost after reserve on the callee via nested carrier";
}

// Dogfood 2026-09-24 (call-path-resilience k0 red test): call rides the relay carrier, a direct
// (punched) link to the same peer comes up, then the relay path goes silent. Design (K001/K002,
// M4/M5): media continues on the direct path. Today the leg is pinned to the carrier mux and
// tears down with "amp call-media: peer link lost". Enable when k3/k4 land.
// k0's red test, green since k3-3: a relayed call whose peer becomes reachable over a direct link
// (a punch landed) moves there by itself; the relay then goes silent and the call does not notice.
TEST_F(AmpCircuitCallMediaComposeTest, CallSurvivesRelaySilenceWithDirectPath) {
  auto nested = EstablishNestedCallMediaPath();
  ASSERT_TRUE(nested) << nested.error().message;

  const std::string call_id = "call-relay-then-direct";
  ByteVector media_key(32, 0x42);
  std::atomic<bool> answerer_connected{false};
  std::mutex mu;
  std::vector<uint8_t> received;
  b_call_->SetInboundHandler(AnswerInline([&](CallMediaDirectConnectParams& params, CallMediaDirectCallbacks& cbs) {
    params.media_key = media_key;
    params.call_id = call_id;
    params.media_epoch = 1;
    params.offerer = false;
    cbs.on_connected = [&] { answerer_connected = true; };
    cbs.on_audio = [&](const std::vector<uint8_t>& opus) {
      std::lock_guard lock(mu);
      received = opus;
    };
  }));

  CallMediaDirectConnectParams params;
  params.peer_key = harness_->peer_id_b;
  params.call_id = call_id;
  params.media_epoch = 1;
  params.media_key = media_key;
  params.offerer = true;
  std::atomic<bool> offerer_connected{false};
  CallMediaDirectCallbacks cbs;
  cbs.on_connected = [&] { offerer_connected.store(true, std::memory_order_release); };
  LegCompletion leg_done;
  const CallMediaLegId leg_id = a_call_->StartLeg(params, std::move(cbs), leg_done.Fn(), 8000);
  ASSERT_TRUE(leg_id);
  leg_done.PumpUntilDone(*harness_);
  ASSERT_TRUE(leg_done.result) << leg_done.result.error().message;
  harness_->PumpUntil([&] { return answerer_connected.load() && offerer_connected.load(); }, 2500);
  ASSERT_EQ(a_call_->ActiveLinkKind(), CallMediaLinkKind::Relayed);

  // "Punch" succeeds: a direct ADP link A↔B now coexists with the nested carrier link (A026).
  ASSERT_TRUE(static_cast<bool>(harness_->mgr_a().RegisterEndpoint("b-direct", harness_->ma_b)));
  Wait<void> direct_wait;
  harness_->mgr_a().EnsureAssociation("b-direct", direct_wait.LinkFn());
  direct_wait.PumpUntilDone(*harness_);
  ASSERT_TRUE(direct_wait.result) << direct_wait.result.error().message;

  // The call moves onto it by itself (media flowing both ways, as in a call).
  uint32_t seq = 0;
  const auto send_both = [&] {
    (void)a_call_->SendAudio(leg_id, {0x01}, ++seq, 0);
    (void)b_call_->SendAudio(b_call_->PrimaryLegId(), {0x02}, seq, 0);
  };
  for (int i = 0; i < 400 && (a_call_->ActiveLinkKind() != CallMediaLinkKind::Direct ||
                              b_call_->ActiveLinkKind() != CallMediaLinkKind::Direct);
       ++i) {
    send_both();
    harness_->PumpAll();
  }
  ASSERT_EQ(a_call_->ActiveLinkKind(), CallMediaLinkKind::Direct);
  ASSERT_EQ(b_call_->ActiveLinkKind(), CallMediaLinkKind::Direct);

  // Relay goes silent (its sends dropped); run past the ADP liveness window on the Amp clock.
  harness_->io_r->SetDropRate(1.0);
  for (int i = 0; i < 40; ++i) {
    harness_->clock->Advance(250);
    send_both();
    harness_->PumpAll();
  }

  EXPECT_EQ(a_call_->Phase(), CallMediaSessionPhase::MediaReady);
  EXPECT_EQ(b_call_->Phase(), CallMediaSessionPhase::MediaReady);
  const std::vector<uint8_t> opus = {0x0d, 0x1e, 0xc7};
  auto sent = a_call_->SendAudio(leg_id, opus, ++seq, 0);
  ASSERT_TRUE(sent) << sent.error().message;
  harness_->PumpUntil([&] { std::lock_guard lock(mu); return received == opus; }, 2500);
  std::lock_guard lock(mu);
  EXPECT_EQ(received, opus) << "audio must keep flowing on the direct path";
}


// k3-2 make-before-break: a call on the relay moves to a direct link while audio keeps flowing —
// every frame arrives once, in order — then the relay path is released and the relay can go
// silent without touching the call.
class CallPathMigrationTest : public AmpCircuitCallMediaComposeTest {
protected:
  /** Relayed call A (offerer) → B, plus a coexisting direct A↔B link. */
  void LiveRelayedCallWithDirectLink() {
    // These tests drive MigrateLeg themselves (the automatic move has its own test).
    a_call_->SetAutoMigrateToDirect(false);
    b_call_->SetAutoMigrateToDirect(false);
    auto nested = EstablishNestedCallMediaPath();
    ASSERT_TRUE(nested) << nested.error().message;
    b_call_->SetInboundHandler(AnswerInline([&](CallMediaDirectConnectParams& params, CallMediaDirectCallbacks& cbs) {
      params.media_key = key_;
      params.call_id = call_id_;
      params.media_epoch = 1;
      params.offerer = false;
      cbs.on_connected = [&] { b_connected_ = true; };
      cbs.on_media = [&](uint8_t channel, uint32_t seq, uint8_t, const std::vector<uint8_t>&) {
        if (channel == 0) {
          std::lock_guard lock(mu_);
          b_seqs_.push_back(seq);
        }
      };
      cbs.on_path_changed = [&](CallMediaLinkKind kind) { b_path_ = kind; };
      cbs.on_failed = [&](const std::string&) { b_failed_ = true; };
    }));
    CallMediaDirectConnectParams params;
    params.peer_key = harness_->peer_id_b;
    params.call_id = call_id_;
    params.media_epoch = 1;
    params.media_key = key_;
    params.offerer = true;
    CallMediaDirectCallbacks cbs;
    cbs.on_connected = [&] { a_connected_ = true; };
    cbs.on_path_changed = [&](CallMediaLinkKind kind) { a_path_ = kind; };
    cbs.on_failed = [&](const std::string&) { a_failed_ = true; };
    LegCompletion leg_done;
    leg_ = a_call_->StartLeg(params, std::move(cbs), leg_done.Fn(), 8000);
    ASSERT_TRUE(leg_);
    leg_done.PumpUntilDone(*harness_);
    ASSERT_TRUE(leg_done.result) << leg_done.result.error().message;
    harness_->PumpUntil([&] { return b_connected_.load() && a_connected_.load(); }, 2500);
    ASSERT_EQ(a_call_->ActiveLinkKind(), CallMediaLinkKind::Relayed);

    ASSERT_TRUE(static_cast<bool>(harness_->mgr_a().RegisterEndpoint("b-direct", harness_->ma_b)));
    Wait<void> direct_wait;
    harness_->mgr_a().EnsureAssociation("b-direct", direct_wait.LinkFn());
    direct_wait.PumpUntilDone(*harness_);
    ASSERT_TRUE(direct_wait.result) << direct_wait.result.error().message;
    auto* direct = harness_->mgr_a().FindLink("b-direct");
    ASSERT_NE(direct, nullptr);
    ASSERT_FALSE(direct->IsCarrierBacked());
    direct_link_ = direct->Handle();
  }

  /** One audio frame each way, as a live call sends. */
  void SendNext() {
    ASSERT_TRUE(static_cast<bool>(a_call_->SendAudio(leg_, {0x11, 0x22}, ++seq_, 0)));
    (void)b_call_->SendAudio(b_call_->PrimaryLegId(), {0x33}, seq_, 0);
  }

  /** Pump (with real time passing — retire timers run on the steady clock) until `done`. */
  bool PumpRealUntil(const std::function<bool()>& done, std::chrono::milliseconds budget) {
    const auto until = std::chrono::steady_clock::now() + budget;
    while (!done() && std::chrono::steady_clock::now() < until) {
      harness_->PumpAll();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
  }

  const std::string call_id_ = "call-k3-migrate";
  ByteVector key_ = ByteVector(32, 0x33);
  CallMediaLegId leg_{};
  pp::amp::LinkHandle direct_link_{};
  uint32_t seq_ = 0;
  std::mutex mu_;
  std::vector<uint32_t> b_seqs_;
  std::atomic<bool> a_connected_{false};
  std::atomic<bool> b_connected_{false};
  std::atomic<bool> a_failed_{false};
  std::atomic<bool> b_failed_{false};
  std::atomic<CallMediaLinkKind> a_path_{CallMediaLinkKind::Unknown};
  std::atomic<CallMediaLinkKind> b_path_{CallMediaLinkKind::Unknown};
};

TEST_F(CallPathMigrationTest, RelayedCallMovesToDirectWithoutLosingAFrame) {
  LiveRelayedCallWithDirectLink();
  for (int i = 0; i < 5; ++i) {
    SendNext();
    harness_->PumpAll();
  }

  std::atomic<bool> migrated{false};
  Roe<void> result = Error("pending");
  a_call_->MigrateLeg(leg_, direct_link_, [&](Roe<void> r) {
    result = std::move(r);
    migrated = true;
  });
  // Audio keeps flowing through the whole switch.
  for (int i = 0; i < 400 && !(migrated.load() && b_path_.load() == CallMediaLinkKind::Direct); ++i) {
    SendNext();
    harness_->PumpAll();
  }
  ASSERT_TRUE(migrated.load());
  ASSERT_TRUE(result) << result.error().message;
  for (int i = 0; i < 20; ++i) {
    SendNext();
    harness_->PumpAll();
  }
  harness_->PumpUntil([&] { std::lock_guard lock(mu_); return b_seqs_.size() >= seq_; }, 2000);

  EXPECT_EQ(a_call_->ActiveLinkKind(), CallMediaLinkKind::Direct);
  EXPECT_EQ(b_call_->ActiveLinkKind(), CallMediaLinkKind::Direct);
  EXPECT_EQ(a_path_.load(), CallMediaLinkKind::Direct);
  EXPECT_EQ(b_path_.load(), CallMediaLinkKind::Direct);
  EXPECT_EQ(a_call_->PathState(leg_).active_gen, 1u);
  {
    std::lock_guard lock(mu_);
    std::vector<uint32_t> expected(seq_);
    for (uint32_t i = 0; i < seq_; ++i) {
      expected[i] = i + 1;
    }
    EXPECT_EQ(b_seqs_, expected) << "every frame once, in order, across the switch";
  }

  // The relay path is released on both ends (after media arrived on the new path).
  EXPECT_TRUE(PumpRealUntil(
      [&] {
        SendNext();
        return !a_call_->PathState(leg_).retiring && !b_call_->PathState(b_call_->PrimaryLegId()).retiring;
      },
      std::chrono::seconds(4)))
      << "old path released";
  EXPECT_FALSE(a_failed_.load());
  EXPECT_FALSE(b_failed_.load());

  // The relay can now go silent: the call does not depend on it any more.
  harness_->io_r->SetDropRate(1.0);
  for (int i = 0; i < 40; ++i) {
    harness_->clock->Advance(250);
    SendNext();
    harness_->PumpAll();
  }
  EXPECT_EQ(a_call_->Phase(), CallMediaSessionPhase::MediaReady);
  EXPECT_EQ(b_call_->Phase(), CallMediaSessionPhase::MediaReady);
  const size_t before = [&] { std::lock_guard lock(mu_); return b_seqs_.size(); }();
  SendNext();
  harness_->PumpUntil([&] { std::lock_guard lock(mu_); return b_seqs_.size() > before; }, 2000);
  std::lock_guard lock(mu_);
  EXPECT_GT(b_seqs_.size(), before) << "audio flows on the direct path";
}

/**
 * k4 fixture: a call moved relayed → direct, with the relay kept as both ends' standby.
 * Returns once the release has turned the relay path into standby on both sides.
 */
#define ASSERT_ON_DIRECT_WITH_RELAY_STANDBY()                                                            \
  do {                                                                                                   \
    LiveRelayedCallWithDirectLink();                                                                     \
    std::atomic<bool> moved{false};                                                                      \
    a_call_->MigrateLeg(leg_, direct_link_, [&](Roe<void> r) { moved = static_cast<bool>(r); });         \
    ASSERT_TRUE(PumpRealUntil(                                                                           \
        [&] {                                                                                            \
          SendNext();                                                                                    \
          return moved.load() && a_call_->PathState(leg_).standby &&                                     \
                 b_call_->PathState(b_call_->PrimaryLegId()).standby;                                    \
        },                                                                                               \
        std::chrono::seconds(4)));                                                                       \
    ASSERT_EQ(a_call_->ActiveLinkKind(), CallMediaLinkKind::Direct);                                     \
    ASSERT_EQ(b_call_->ActiveLinkKind(), CallMediaLinkKind::Direct);                                     \
  } while (0)

// k4: the direct link dies mid-call; both ends switch to the warm relay standby at once and the call
// carries on — no teardown, no failure.
TEST_F(CallPathMigrationTest, LostDirectLinkFailsOverToTheRelayStandby) {
  ASSERT_ON_DIRECT_WITH_RELAY_STANDBY();
  ASSERT_GT(harness_->mgr_a().RequestDropLink("b-direct"), 0u);
  for (int i = 0; i < 400 && (a_call_->ActiveLinkKind() != CallMediaLinkKind::Relayed ||
                              b_call_->ActiveLinkKind() != CallMediaLinkKind::Relayed);
       ++i) {
    SendNext();
    harness_->PumpAll();
  }
  EXPECT_EQ(a_call_->ActiveLinkKind(), CallMediaLinkKind::Relayed);
  EXPECT_EQ(b_call_->ActiveLinkKind(), CallMediaLinkKind::Relayed);
  EXPECT_EQ(a_path_.load(), CallMediaLinkKind::Relayed);
  EXPECT_EQ(b_path_.load(), CallMediaLinkKind::Relayed);
  const size_t before = [&] { std::lock_guard lock(mu_); return b_seqs_.size(); }();
  for (int i = 0; i < 10; ++i) {
    SendNext();
    harness_->PumpAll();
  }
  harness_->PumpUntil([&] { std::lock_guard lock(mu_); return b_seqs_.size() >= before + 10; }, 2000);
  {
    std::lock_guard lock(mu_);
    EXPECT_GE(b_seqs_.size(), before + 10) << "every frame after the switch arrives";
  }
  EXPECT_EQ(a_call_->Phase(), CallMediaSessionPhase::MediaReady);
  EXPECT_EQ(b_call_->Phase(), CallMediaSessionPhase::MediaReady);
  EXPECT_FALSE(a_failed_.load());
  EXPECT_FALSE(b_failed_.load());
}

// k4: the direct path goes quiet while its link stays up (one end's traffic stops getting through):
// after 1.5 s without heartbeat or media both ends move to the relay standby.
TEST_F(CallPathMigrationTest, SilentDirectPathFailsOverToTheRelayStandby) {
  ASSERT_ON_DIRECT_WITH_RELAY_STANDBY();
  b_call_->SetSilencedPathKindForTest(CallMediaLinkKind::Direct);
  EXPECT_TRUE(PumpRealUntil(
      [&] {
        SendNext();
        return a_call_->ActiveLinkKind() == CallMediaLinkKind::Relayed &&
               b_call_->ActiveLinkKind() == CallMediaLinkKind::Relayed;
      },
      std::chrono::seconds(5)))
      << "both ends fail over";
  b_call_->SetSilencedPathKindForTest(CallMediaLinkKind::Unknown);
  const size_t before = [&] { std::lock_guard lock(mu_); return b_seqs_.size(); }();
  SendNext();
  harness_->PumpUntil([&] { std::lock_guard lock(mu_); return b_seqs_.size() > before; }, 2000);
  std::lock_guard lock(mu_);
  EXPECT_GT(b_seqs_.size(), before);
  EXPECT_FALSE(a_failed_.load());
  EXPECT_FALSE(b_failed_.load());
}

// k4: a muted peer still heartbeats and sends silence frames — a quiet mic never trips failover.
TEST_F(CallPathMigrationTest, QuietButAliveDirectPathStays) {
  ASSERT_ON_DIRECT_WITH_RELAY_STANDBY();
  EXPECT_FALSE(PumpRealUntil(
      [&] {
        SendNext();  // "muted": frames of silence still flow
        return a_call_->ActiveLinkKind() != CallMediaLinkKind::Direct ||
               b_call_->ActiveLinkKind() != CallMediaLinkKind::Direct;
      },
      std::chrono::milliseconds(2500)));
}

// A candidate that goes away mid-migration is abandoned: the call stays on its path, unharmed.
TEST_F(CallPathMigrationTest, LostCandidateLeavesTheCallOnItsPath) {
  LiveRelayedCallWithDirectLink();
  std::atomic<bool> done{false};
  Roe<void> result = Roe<void>();
  a_call_->MigrateLeg(leg_, direct_link_, [&](Roe<void> r) {
    result = std::move(r);
    done = true;
  });
  ASSERT_GT(harness_->mgr_a().RequestDropLink("b-direct"), 0u);
  for (int i = 0; i < 400 && !done.load(); ++i) {
    SendNext();
    harness_->PumpAll();
  }
  ASSERT_TRUE(done.load());
  EXPECT_FALSE(result) << "the migration did not complete";
  EXPECT_EQ(a_call_->ActiveLinkKind(), CallMediaLinkKind::Relayed);
  EXPECT_EQ(a_call_->Phase(), CallMediaSessionPhase::MediaReady);
  EXPECT_EQ(b_call_->Phase(), CallMediaSessionPhase::MediaReady);
  EXPECT_FALSE(a_call_->PathState(leg_).candidate);
  harness_->PumpUntil([&] { return !b_call_->PathState(b_call_->PrimaryLegId()).candidate; }, 500);
  EXPECT_FALSE(b_call_->PathState(b_call_->PrimaryLegId()).candidate) << "the peer dropped its half too";
  const size_t before = [&] { std::lock_guard lock(mu_); return b_seqs_.size(); }();
  SendNext();
  harness_->PumpUntil([&] { std::lock_guard lock(mu_); return b_seqs_.size() > before; }, 2000);
  EXPECT_FALSE(a_failed_.load());
  EXPECT_FALSE(b_failed_.load());
}

// A peer from before k3 never answers `migrate`: the driver gives up at its timeout and the call
// stays on its path, unharmed.
TEST_F(CallPathMigrationTest, OlderPeerLeavesTheCallOnItsPath) {
  LiveRelayedCallWithDirectLink();
  b_call_->SetIgnoreMigrateForTest(true);
  a_call_->SetMigrateTimeoutForTest(std::chrono::milliseconds(200));
  std::atomic<bool> done{false};
  Roe<void> result = Roe<void>();
  a_call_->MigrateLeg(leg_, direct_link_, [&](Roe<void> r) {
    result = std::move(r);
    done = true;
  });
  EXPECT_TRUE(PumpRealUntil(
      [&] {
        SendNext();
        return done.load();
      },
      std::chrono::seconds(3)));
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().message.find("timed out"), std::string::npos) << result.error().message;
  EXPECT_EQ(a_call_->ActiveLinkKind(), CallMediaLinkKind::Relayed);
  EXPECT_EQ(a_call_->Phase(), CallMediaSessionPhase::MediaReady);
  EXPECT_EQ(b_call_->Phase(), CallMediaSessionPhase::MediaReady);
  EXPECT_FALSE(a_failed_.load());
  EXPECT_FALSE(b_failed_.load());
}

// k3-4: either end may migrate (the answerer can be the one that notices the path is bad).
TEST_F(CallPathMigrationTest, TheAnswererMayMigrateToo) {
  LiveRelayedCallWithDirectLink();
  std::atomic<bool> done{false};
  Roe<void> result = Error("pending");
  b_call_->MigrateLegToKind(b_call_->PrimaryLegId(), CallMediaLinkKind::Direct, [&](Roe<void> r) {
    result = std::move(r);
    done = true;
  });
  for (int i = 0; i < 400 && !(done.load() && a_call_->ActiveLinkKind() == CallMediaLinkKind::Direct); ++i) {
    SendNext();
    harness_->PumpAll();
  }
  ASSERT_TRUE(done.load());
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(a_call_->ActiveLinkKind(), CallMediaLinkKind::Direct);
  EXPECT_EQ(b_call_->ActiveLinkKind(), CallMediaLinkKind::Direct);
  EXPECT_FALSE(a_failed_.load());
  EXPECT_FALSE(b_failed_.load());
}

// Both ends start a migration at once: the glare winner's (the offerer's) goes ahead; the other
// yields and answers it. The call moves exactly once.
TEST_F(CallPathMigrationTest, SimultaneousMigrationsTheOffererWins) {
  LiveRelayedCallWithDirectLink();
  std::atomic<int> finished{0};
  Roe<void> a_result = Error("pending");
  Roe<void> b_result = Roe<void>();
  a_call_->MigrateLeg(leg_, direct_link_, [&](Roe<void> r) {
    a_result = std::move(r);
    ++finished;
  });
  b_call_->MigrateLegToKind(b_call_->PrimaryLegId(), CallMediaLinkKind::Direct, [&](Roe<void> r) {
    b_result = std::move(r);
    ++finished;
  });
  for (int i = 0; i < 400 && finished.load() < 2; ++i) {
    SendNext();
    harness_->PumpAll();
  }
  ASSERT_EQ(finished.load(), 2);
  EXPECT_TRUE(a_result) << a_result.error().message;
  EXPECT_FALSE(b_result) << "the answerer's own attempt yields";
  harness_->PumpUntil([&] { return b_call_->ActiveLinkKind() == CallMediaLinkKind::Direct; }, 500);
  EXPECT_EQ(a_call_->ActiveLinkKind(), CallMediaLinkKind::Direct);
  EXPECT_EQ(b_call_->ActiveLinkKind(), CallMediaLinkKind::Direct);
  EXPECT_EQ(a_call_->PathState(leg_).active_gen, 1u) << "moved exactly once";
  EXPECT_FALSE(a_failed_.load());
  EXPECT_FALSE(b_failed_.load());
}

// k3-4 TX-only escalation, make-before-break: a call on a direct link moves onto the relay by
// transport class; automatic migration never takes it back onto the link it left.
TEST_F(CallPathMigrationTest, MovedToTheRelayItStaysOffTheLinkItLeft) {
  LiveRelayedCallWithDirectLink();
  std::atomic<bool> to_direct{false};
  a_call_->MigrateLeg(leg_, direct_link_, [&](Roe<void> r) { to_direct = static_cast<bool>(r); });
  for (int i = 0; i < 400 && !(to_direct.load() && b_call_->ActiveLinkKind() == CallMediaLinkKind::Direct); ++i) {
    SendNext();
    harness_->PumpAll();
  }
  ASSERT_TRUE(to_direct.load());
  ASSERT_TRUE(PumpRealUntil(
      [&] {
        SendNext();
        return !a_call_->PathState(leg_).retiring && !b_call_->PathState(b_call_->PrimaryLegId()).retiring;
      },
      std::chrono::seconds(4)));

  // The direct path stops delivering to B (TX-only) — B moves the call back onto the relay.
  a_call_->SetAutoMigrateToDirect(true);
  b_call_->SetAutoMigrateToDirect(true);
  std::atomic<bool> to_relay{false};
  Roe<void> result = Error("pending");
  b_call_->MigrateLegToKind(b_call_->PrimaryLegId(), CallMediaLinkKind::Relayed, [&](Roe<void> r) {
    result = std::move(r);
    to_relay = true;
  });
  for (int i = 0; i < 400 && !(to_relay.load() && a_call_->ActiveLinkKind() == CallMediaLinkKind::Relayed); ++i) {
    SendNext();
    harness_->PumpAll();
  }
  ASSERT_TRUE(to_relay.load());
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(a_call_->ActiveLinkKind(), CallMediaLinkKind::Relayed);
  EXPECT_EQ(b_call_->ActiveLinkKind(), CallMediaLinkKind::Relayed);
  // The direct link is still Connected, but it is the one the call left: no bounce back.
  EXPECT_FALSE(PumpRealUntil(
      [&] {
        SendNext();
        return a_call_->ActiveLinkKind() == CallMediaLinkKind::Direct ||
               b_call_->ActiveLinkKind() == CallMediaLinkKind::Direct;
      },
      std::chrono::milliseconds(1500)));
  EXPECT_FALSE(a_failed_.load());
  EXPECT_FALSE(b_failed_.load());
}

TEST_F(AmpCircuitCallMediaComposeTest, CircuitNestedEncryptedVideoOver16KiB) {
  auto nested = EstablishNestedCallMediaPath();
  ASSERT_TRUE(nested) << nested.error().message;

  const std::string call_id = "call-amp-circuit-video";
  ByteVector media_key(32, 0x42);

  std::mutex mu;
  bool answerer_connected = false;
  bool got_video = false;
  std::vector<uint8_t> received;
  uint8_t received_ch = 255;

  b_call_->SetInboundHandler(AnswerInline([&](CallMediaDirectConnectParams& params, CallMediaDirectCallbacks& cbs) {
    params.media_key = media_key;
    params.call_id = call_id;
    params.media_epoch = 1;
    params.offerer = false;
    cbs.on_connected = [&] {
      std::lock_guard lock(mu);
      answerer_connected = true;
    };
    cbs.on_media = [&](uint8_t channel, uint32_t, uint8_t, const std::vector<uint8_t>& payload) {
      std::lock_guard lock(mu);
      received_ch = channel;
      received = payload;
      got_video = true;
    };
  }));

  CallMediaDirectConnectParams params;
  params.peer_key = harness_->peer_id_b;
  params.call_id = call_id;
  params.media_epoch = 1;
  params.media_key = media_key;
  params.offerer = true;

  std::atomic<bool> offerer_connected{false};
  CallMediaDirectCallbacks cbs;
  cbs.on_connected = [&] { offerer_connected.store(true, std::memory_order_release); };

  LegCompletion leg_done;
  const CallMediaLegId leg_id = a_call_->StartLeg(params, std::move(cbs), leg_done.Fn(), 8000);
  ASSERT_TRUE(leg_id);
  leg_done.PumpUntilDone(*harness_);
  ASSERT_TRUE(leg_done.result) << leg_done.result.error().message;

  harness_->PumpUntil(
      [&] { return answerer_connected && offerer_connected.load(std::memory_order_acquire); }, 2500);
  ASSERT_TRUE(answerer_connected);
  ASSERT_TRUE(offerer_connected.load(std::memory_order_acquire));
  ASSERT_EQ(a_call_->Phase(), CallMediaSessionPhase::MediaReady);
  ASSERT_EQ(b_call_->Phase(), CallMediaSessionPhase::MediaReady);

  std::vector<uint8_t> video(20 * 1024, 0x7e);
  auto sent = a_call_->SendMedia(leg_id, 1, video, 1, 0);
  ASSERT_TRUE(sent) << sent.error().message;

  harness_->PumpUntil([&] { return got_video; }, 5000);
  ASSERT_TRUE(got_video) << "video frame not received; a_phase=" << static_cast<int>(a_call_->Phase())
                         << " b_phase=" << static_cast<int>(b_call_->Phase());
  EXPECT_EQ(received_ch, 1);
  EXPECT_EQ(received, video);

  a_call_->DetachLeg(leg_id);
}

/** Double-NAT dogfood pattern: B holds Session to R before A StartBridge. */
TEST_F(AmpCircuitCallMediaComposeTest, BridgeAfterAnswererWarmToRelay) {
  Wait<void> b_assoc;
  harness_->mgr_b().EnsureAssociation("relay", b_assoc.LinkFn());
  b_assoc.PumpUntilDone(*harness_);
  ASSERT_TRUE(b_assoc.result) << b_assoc.result.error().message;
  ASSERT_TRUE(harness_->mgr_b().IsConnected("relay"));
  ASSERT_TRUE(harness_->mgr_r().FindLinkByPeerId(harness_->peer_id_b) != nullptr);

  Wait<void> a_assoc;
  harness_->mgr_a().EnsureAssociation("relay", a_assoc.LinkFn());
  a_assoc.PumpUntilDone(*harness_);
  ASSERT_TRUE(a_assoc.result) << a_assoc.result.error().message;

  auto nested = EstablishNestedCallMediaPath();
  ASSERT_TRUE(nested) << nested.error().message;
  EXPECT_TRUE(harness_->mgr_a().IsConnected(harness_->peer_id_b));
}

/**
 * hard-w5 Phase-2 regression: dialer has a private punch-synced advertise MA for B.
 * Peer-id-only StartBridge must not send that MA to the hop (hop book stays SNAT/public).
 */
TEST_F(AmpCircuitCallMediaComposeTest, PeerIdOnlyNestDoesNotPoisonRelayBookWithPrivateMa) {
  Wait<void> b_assoc;
  harness_->mgr_b().EnsureAssociation("relay", b_assoc.LinkFn());
  b_assoc.PumpUntilDone(*harness_);
  ASSERT_TRUE(b_assoc.result) << b_assoc.result.error().message;
  ASSERT_TRUE(harness_->mgr_r().FindLinkByPeerId(harness_->peer_id_b) != nullptr);

  Wait<void> a_assoc;
  harness_->mgr_a().EnsureAssociation("relay", a_assoc.LinkFn());
  a_assoc.PumpUntilDone(*harness_);
  ASSERT_TRUE(a_assoc.result) << a_assoc.result.error().message;

  const std::string private_ma =
      "/ip4/10.255.255.1/udp/9/adp/1.0.0/p2p/" + harness_->peer_id_b;
  ASSERT_TRUE(static_cast<bool>(harness_->mgr_a().RegisterEndpoint(harness_->peer_id_b, private_ma)));
  ASSERT_EQ(harness_->mgr_a().PreferredMultiaddr(harness_->peer_id_b).value_or(""), private_ma);

  const auto relay_ma_before = harness_->mgr_r().PreferredMultiaddr(harness_->peer_id_b);
  ASSERT_TRUE(relay_ma_before.has_value());
  EXPECT_EQ(*relay_ma_before, harness_->ma_b);

  auto nested = EstablishNestedCallMediaPath(/*peer_id_only=*/true);
  ASSERT_TRUE(nested) << nested.error().message;
  EXPECT_TRUE(harness_->mgr_a().IsConnected(harness_->peer_id_b));

  const auto relay_ma_after = harness_->mgr_r().PreferredMultiaddr(harness_->peer_id_b);
  ASSERT_TRUE(relay_ma_after.has_value());
  EXPECT_EQ(*relay_ma_after, harness_->ma_b)
      << "hop book must stay dialable; private punch MA must not overwrite";
}

/**
 * Hop Preferred poisoned private while answerer still Connected: peer-id-only must splice the
 * live link (not dial the private MA into WaitAck timeout).
 */
TEST_F(AmpCircuitCallMediaComposeTest, PeerIdOnlyPrefersConnectedOverStalePrivatePreferred) {
  Wait<void> b_assoc;
  harness_->mgr_b().EnsureAssociation("relay", b_assoc.LinkFn());
  b_assoc.PumpUntilDone(*harness_);
  ASSERT_TRUE(b_assoc.result) << b_assoc.result.error().message;
  ASSERT_TRUE(harness_->mgr_r().FindLinkByPeerId(harness_->peer_id_b) != nullptr);

  Wait<void> a_assoc;
  harness_->mgr_a().EnsureAssociation("relay", a_assoc.LinkFn());
  a_assoc.PumpUntilDone(*harness_);
  ASSERT_TRUE(a_assoc.result) << a_assoc.result.error().message;

  const std::string private_ma =
      "/ip4/10.255.255.1/udp/9/adp/1.0.0/p2p/" + harness_->peer_id_b;
  ASSERT_TRUE(static_cast<bool>(harness_->mgr_r().RegisterEndpoint(harness_->peer_id_b, private_ma)));
  ASSERT_EQ(harness_->mgr_r().PreferredMultiaddr(harness_->peer_id_b).value_or(""), private_ma);
  ASSERT_GT(harness_->mgr_r().CountConnectedLinksForPeerId(harness_->peer_id_b), 0);

  auto nested = EstablishNestedCallMediaPath(/*peer_id_only=*/true);
  ASSERT_TRUE(nested) << nested.error().message;
  EXPECT_TRUE(harness_->mgr_a().IsConnected(harness_->peer_id_b));
}

/**
 * Peer-id-only with private Preferred and no Connected answerer must not dial Preferred
 * (WaitAck hang). Hop may wait briefly for a far leg, then fail not-registered — still
 * well under a private-Preferred dial hang. Dogfood dual-NAT.
 */
TEST_F(AmpCircuitCallMediaComposeTest, PeerIdOnlyPrivatePreferredFastFailsWhenNotConnected) {
  Wait<void> a_assoc;
  harness_->mgr_a().EnsureAssociation("relay", a_assoc.LinkFn());
  a_assoc.PumpUntilDone(*harness_);
  ASSERT_TRUE(a_assoc.result) << a_assoc.result.error().message;

  // Do not park B. Poison hop book with a private Preferred for B.
  ASSERT_EQ(harness_->mgr_r().CountConnectedLinksForPeerId(harness_->peer_id_b), 0u);
  const std::string private_ma =
      "/ip4/10.255.255.1/udp/9/adp/1.0.0/p2p/" + harness_->peer_id_b;
  ASSERT_TRUE(static_cast<bool>(harness_->mgr_r().RegisterEndpoint(harness_->peer_id_b, private_ma)));
  ASSERT_EQ(harness_->mgr_r().PreferredMultiaddr(harness_->peer_id_b).value_or(""), private_ma);

  CircuitBridgeTarget target;
  target.target_peer_id = harness_->peer_id_b;
  target.target_protocol = pp::amp::kAmpCircuitCarrierProtocolId;
  ASSERT_TRUE(target.target_multiaddr.empty());

  Wait<CircuitTunnelBridgeResult> bridge_wait;
  const auto t0 = std::chrono::steady_clock::now();
  // Short bridge budget so ServeDial far-leg wait caps quickly in this test.
  auto tunnel_id = circuit_a_->StartBridge("relay", target, {}, {}, bridge_wait.Fn(), 800);
  ASSERT_TRUE(static_cast<bool>(tunnel_id));
  // Harness PumpAll is a spin — sleep so ServeDial far-leg wait / tunnel deadline can elapse.
  harness_->PumpUntil(
      [&] {
        if (bridge_wait.done.load(std::memory_order_acquire)) {
          return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return false;
      },
      2000);
  ASSERT_TRUE(bridge_wait.done.load(std::memory_order_acquire));
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - t0)
                              .count();

  ASSERT_FALSE(bridge_wait.result);
  EXPECT_NE(bridge_wait.result.error().message.find("not registered"), std::string::npos)
      << bridge_wait.result.error().message;
  EXPECT_LT(elapsed_ms, 2500) << "must not WaitAck-hang dialing private Preferred (elapsed_ms="
                              << elapsed_ms << ")";
}

/**
 * Event-driven ServeDial: StartBridge while B is not Connected, then associate B→R —
 * PeerConnectedListener must resume the hop waiter (no wall-clock poll).
 */
TEST_F(AmpCircuitCallMediaComposeTest, PeerIdOnlyServeDialResumesWhenFarLegConnects) {
  Wait<void> a_assoc;
  harness_->mgr_a().EnsureAssociation("relay", a_assoc.LinkFn());
  a_assoc.PumpUntilDone(*harness_);
  ASSERT_TRUE(a_assoc.result) << a_assoc.result.error().message;
  ASSERT_EQ(harness_->mgr_r().CountConnectedLinksForPeerId(harness_->peer_id_b), 0u);

  CircuitBridgeTarget target;
  target.target_peer_id = harness_->peer_id_b;
  target.target_protocol = pp::amp::kAmpCircuitCarrierProtocolId;

  Wait<CircuitTunnelBridgeResult> bridge_wait;
  auto tunnel_id = circuit_a_->StartBridge("relay", target, {}, {}, bridge_wait.Fn(), 8000);
  ASSERT_TRUE(static_cast<bool>(tunnel_id));

  // Let dialer open circuit channel + hop arm ServeDial wait (must still be pending).
  for (int i = 0; i < 80 && !bridge_wait.done.load(std::memory_order_acquire); ++i) {
    harness_->PumpAll();
  }
  ASSERT_FALSE(bridge_wait.done.load(std::memory_order_acquire))
      << "ServeDial should wait for far leg, not fail immediately";

  Wait<void> b_assoc;
  harness_->mgr_b().EnsureAssociation("relay", b_assoc.LinkFn());
  b_assoc.PumpUntilDone(*harness_);
  ASSERT_TRUE(b_assoc.result) << b_assoc.result.error().message;
  ASSERT_GT(harness_->mgr_r().CountConnectedLinksForPeerId(harness_->peer_id_b), 0u);

  bridge_wait.PumpUntilDone(*harness_);
  ASSERT_TRUE(bridge_wait.result) << bridge_wait.result.error().message;
  EXPECT_TRUE(bridge_wait.result->ok);
}

/**
 * Contrast: sending private target_multiaddr makes the hop RegisterEndpoint that MA
 * (book overwrite). Do not wait for dial timeout — only assert the poison write.
 */
TEST_F(AmpCircuitCallMediaComposeTest, PrivateTargetMultiaddrPoisonsRelayBook) {
  Wait<void> a_assoc;
  harness_->mgr_a().EnsureAssociation("relay", a_assoc.LinkFn());
  a_assoc.PumpUntilDone(*harness_);
  ASSERT_TRUE(a_assoc.result) << a_assoc.result.error().message;

  const std::string private_ma =
      "/ip4/10.255.255.1/udp/9/adp/1.0.0/p2p/" + harness_->peer_id_b;
  ASSERT_EQ(harness_->mgr_r().PreferredMultiaddr(harness_->peer_id_b).value_or(""), harness_->ma_b);

  CircuitBridgeTarget target;
  target.target_peer_id = harness_->peer_id_b;
  target.target_multiaddr = private_ma;
  target.target_protocol = pp::amp::kAmpCircuitCarrierProtocolId;

  Wait<CircuitTunnelBridgeResult> bridge_wait;
  auto tunnel_id = circuit_a_->StartBridge("relay", target, {}, {}, bridge_wait.Fn(), 500);
  ASSERT_TRUE(static_cast<bool>(tunnel_id));
  // Pump long enough for the hop to apply target_multiaddr RegisterEndpoint.
  harness_->PumpUntil(
      [&] {
        const auto ma = harness_->mgr_r().PreferredMultiaddr(harness_->peer_id_b);
        return ma && *ma == private_ma;
      },
      800);
  circuit_a_->CancelTunnel(tunnel_id);

  const auto relay_ma = harness_->mgr_r().PreferredMultiaddr(harness_->peer_id_b);
  ASSERT_TRUE(relay_ma.has_value());
  EXPECT_EQ(*relay_ma, private_ma) << "documents hop-book overwrite (hard-w5 poison mode)";
}

} // namespace
} // namespace pbr
