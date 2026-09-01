#include "base/p2p/AmpCircuitHopRegistry.h"
#include "base/p2p/CallMediaLegCoordinator.h"
#include "base/p2p/CircuitTunnelCoordinator.h"
#include "amp/link/Types.h"
#include "base/p2p/tests/support/mesh_triple_harness.h"

#include <gtest/gtest.h>
#include <sodium.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
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

  Roe<void> EstablishNestedCallMediaPath() {
    CircuitBridgeTarget target;
    target.target_peer_id = harness_->peer_id_b;
    target.target_multiaddr = harness_->ma_b;
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

  b_call_->SetInboundHandler([&](CallMediaDirectConnectParams& params, CallMediaDirectCallbacks& cbs) {
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
  });

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

  a_call_->DetachLeg(leg_id);
  harness_->PumpUntil([&] { return a_call_->Phase() == CallMediaSessionPhase::Idle; }, 500);
  EXPECT_EQ(a_call_->Phase(), CallMediaSessionPhase::Idle);
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

  b_call_->SetInboundHandler([&](CallMediaDirectConnectParams& params, CallMediaDirectCallbacks& cbs) {
    params.media_key = media_key;
    params.call_id = call_id;
    params.media_epoch = 1;
    params.offerer = false;
    cbs.on_connected = [&] {
      std::lock_guard lock(mu);
      answerer_connected = true;
    };
    cbs.on_media = [&](uint8_t channel, const std::vector<uint8_t>& payload) {
      std::lock_guard lock(mu);
      received_ch = channel;
      received = payload;
      got_video = true;
    };
  });

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

} // namespace
} // namespace pbr
