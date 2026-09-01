#include "base/p2p/CallMediaLegCoordinator.h"
#include "lib/amp/tests/support/mesh_harness_support.h"
#include "lib/amp/tests/support/mesh_test_harness.h"
#include "crypto/MlDsa.h"

#include <gtest/gtest.h>
#include <sodium.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace pbr {
namespace {

struct LegCompletion {
  std::atomic<bool> finished{false};
  pp::Roe<void> result = pp::Error("pending");

  CallMediaLegCoordinator::LegFinished Fn() {
    return [this](pp::Roe<void> r) {
      result = std::move(r);
      finished.store(true, std::memory_order_release);
    };
  }

  template <typename Harness>
  void PumpUntilDone(Harness& harness, const size_t max_rounds = 2000) {
    harness.PumpUntil([this] { return finished.load(std::memory_order_acquire); }, max_rounds);
    ASSERT_TRUE(finished.load(std::memory_order_acquire)) << "leg completion timed out";
  }
};

void WaitUntil(const std::function<bool()>& predicate, const std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  FAIL() << "Timed out waiting for condition";
}

class CallMediaLegCoordinatorTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_GE(sodium_init(), 0);
    stall_release_ = std::make_shared<std::atomic<bool>>(false);

    auto created = pbr::test::AmpMeshHarness::Create();
    ASSERT_TRUE(static_cast<bool>(created));
    harness_ = std::move(*created);

    ASSERT_TRUE(static_cast<bool>(harness_->mgr_a().RegisterEndpoint("b", harness_->ma_b)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_b().RegisterEndpoint("a", harness_->ma_a)));

    a_call_ = std::make_unique<CallMediaLegCoordinator>(*harness_->runtime_a);
    b_call_ = std::make_unique<CallMediaLegCoordinator>(*harness_->runtime_b);
    a_call_->Start();
    b_call_->Start();
  }

  void TearDown() override {
    if (stall_release_) {
      stall_release_->store(true, std::memory_order_release);
    }
    a_call_->Stop();
    b_call_->Stop();
    a_call_.reset();
    b_call_.reset();
    harness_.reset();
    stall_release_.reset();
  }

  std::shared_ptr<std::atomic<bool>> stall_release_;
  std::unique_ptr<pbr::test::AmpMeshHarness> harness_;
  std::unique_ptr<CallMediaLegCoordinator> a_call_;
  std::unique_ptr<CallMediaLegCoordinator> b_call_;
};

TEST_F(CallMediaLegCoordinatorTest, HelloAndEncryptedAudioRoundTrip) {
  const std::string call_id = "call-amp-duplex";
  ByteVector media_key(32, 0x42);

  std::mutex mu;
  std::condition_variable cv;
  bool connected = false;
  bool got_audio = false;
  std::vector<uint8_t> received;

  b_call_->SetInboundHandler([&](CallMediaDirectConnectParams& params, CallMediaDirectCallbacks& cbs) {
    params.media_key = media_key;
    params.call_id = call_id;
    params.media_epoch = 1;
    params.offerer = false;
    cbs.on_connected = [&] {
      std::lock_guard lock(mu);
      connected = true;
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
  params.peer_key = "b";
  params.call_id = call_id;
  params.media_epoch = 1;
  params.media_key = media_key;
  params.offerer = true;

  CallMediaDirectCallbacks cbs;
  std::atomic<bool> offerer_connected{false};
  cbs.on_connected = [&] {
    offerer_connected.store(true, std::memory_order_release);
    cv.notify_one();
  };

  LegCompletion leg_done;
  const CallMediaLegId leg_id = a_call_->StartLeg(params, std::move(cbs), leg_done.Fn(), 5000);
  ASSERT_TRUE(leg_id);

  leg_done.PumpUntilDone(*harness_);
  ASSERT_TRUE(leg_done.result) << leg_done.result.error().message;

  harness_->PumpUntil([&] {
    return connected && offerer_connected.load(std::memory_order_acquire);
  });
  ASSERT_TRUE(connected);
  ASSERT_TRUE(offerer_connected.load(std::memory_order_acquire));

  const std::vector<uint8_t> opus = {0xde, 0xad, 0xbe, 0xef};
  auto sent = a_call_->SendAudio(leg_id, opus, 1, 0);
  ASSERT_TRUE(sent) << sent.error().message;

  harness_->PumpUntil([&] { return got_audio; });
  EXPECT_EQ(received, opus);
  EXPECT_EQ(a_call_->Phase(), CallMediaSessionPhase::MediaReady);
  EXPECT_EQ(b_call_->Phase(), CallMediaSessionPhase::MediaReady);

  a_call_->DetachLeg(leg_id);
  harness_->PumpBoth();
  EXPECT_EQ(a_call_->Phase(), CallMediaSessionPhase::Idle);
  EXPECT_FALSE(a_call_->IsLegActive(leg_id));
}

TEST_F(CallMediaLegCoordinatorTest, DetachUnblocksConnectWait) {
  b_call_->Stop();
  b_call_ = std::make_unique<CallMediaLegCoordinator>(
      *harness_->runtime_b, [](std::function<void()> fn) { std::thread(std::move(fn)).detach(); });
  b_call_->Start();

  const std::string call_id = "call-amp-detach-wait";
  ByteVector media_key(32, 0x11);
  std::atomic<bool> b_stalled{false};
  auto release_b = stall_release_;
  ASSERT_TRUE(release_b);

  b_call_->SetInboundHandler([release_b, &media_key, &call_id, &b_stalled](CallMediaDirectConnectParams& params,
                                                                             CallMediaDirectCallbacks&) {
    params.media_key = media_key;
    params.call_id = call_id;
    params.media_epoch = 1;
    b_stalled.store(true, std::memory_order_release);
    while (!release_b->load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });

  CallMediaDirectConnectParams params;
  params.peer_key = "b";
  params.call_id = call_id;
  params.media_epoch = 1;
  params.media_key = media_key;
  params.offerer = true;

  LegCompletion leg_done;
  const CallMediaLegId leg_id = a_call_->StartLeg(params, {}, leg_done.Fn(), 15000);
  ASSERT_TRUE(leg_id);

  WaitUntil(
      [&] {
        harness_->PumpBoth();
        return b_stalled.load(std::memory_order_acquire) &&
               a_call_->Phase() == CallMediaSessionPhase::HelloOutbound;
      },
      std::chrono::milliseconds(5000));
  ASSERT_TRUE(b_stalled.load(std::memory_order_acquire));
  ASSERT_EQ(a_call_->Phase(), CallMediaSessionPhase::HelloOutbound);
  ASSERT_FALSE(leg_done.finished.load(std::memory_order_acquire));

  a_call_->DetachLeg(leg_id);
  const auto detach_until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!leg_done.finished.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < detach_until) {
    harness_->PumpBoth();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  ASSERT_TRUE(leg_done.finished.load(std::memory_order_acquire));
  EXPECT_FALSE(leg_done.result);
  EXPECT_EQ(leg_done.result.error().message, "call-media aborted");
  EXPECT_EQ(a_call_->Phase(), CallMediaSessionPhase::Idle);
}

TEST_F(CallMediaLegCoordinatorTest, HelloAndEncryptedVideoRoundTripOver16KiB) {
  const std::string call_id = "call-amp-video";
  ByteVector media_key(32, 0x42);

  std::mutex mu;
  std::condition_variable cv;
  bool connected = false;
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
      connected = true;
      cv.notify_one();
    };
    cbs.on_media = [&](uint8_t channel, const std::vector<uint8_t>& payload) {
      std::lock_guard lock(mu);
      received_ch = channel;
      received = payload;
      got_video = true;
      cv.notify_one();
    };
  });

  CallMediaDirectConnectParams params;
  params.peer_key = "b";
  params.call_id = call_id;
  params.media_epoch = 1;
  params.media_key = media_key;
  params.offerer = true;

  CallMediaDirectCallbacks cbs;
  std::atomic<bool> offerer_connected{false};
  cbs.on_connected = [&] {
    offerer_connected.store(true, std::memory_order_release);
    cv.notify_one();
  };

  LegCompletion leg_done;
  const CallMediaLegId leg_id = a_call_->StartLeg(params, std::move(cbs), leg_done.Fn(), 5000);
  ASSERT_TRUE(leg_id);
  leg_done.PumpUntilDone(*harness_);

  harness_->PumpUntil([&] {
    return connected && offerer_connected.load(std::memory_order_acquire);
  });

  const std::vector<uint8_t> au(20 * 1024, 0x5a);
  auto sent = a_call_->SendMedia(leg_id, 1, au, 1, 1);
  ASSERT_TRUE(sent) << sent.error().message;

  harness_->PumpUntil([&] { return got_video; });
  EXPECT_EQ(received_ch, 1);
  EXPECT_EQ(received, au);

  a_call_->DetachLeg(leg_id);
  harness_->PumpBoth();
}

TEST_F(CallMediaLegCoordinatorTest, FailAfterDetachDoesNotCallOnFailed) {
  const std::string call_id = "call-amp-fail-after-detach";
  ByteVector media_key(32, 0x33);
  std::atomic<int> local_failed{0};

  b_call_->SetInboundHandler([&](CallMediaDirectConnectParams& params, CallMediaDirectCallbacks&) {
    params.media_key = media_key;
    params.call_id = call_id;
    params.media_epoch = 1;
  });

  CallMediaDirectConnectParams params;
  params.peer_key = "b";
  params.call_id = call_id;
  params.media_epoch = 1;
  params.media_key = media_key;
  params.offerer = true;
  CallMediaDirectCallbacks cbs;
  cbs.on_failed = [&](const std::string&) { local_failed.fetch_add(1); };

  LegCompletion leg_done;
  const CallMediaLegId leg_id = a_call_->StartLeg(params, std::move(cbs), leg_done.Fn(), 5000);
  ASSERT_TRUE(leg_id);
  leg_done.PumpUntilDone(*harness_);
  ASSERT_TRUE(leg_done.result);
  ASSERT_EQ(a_call_->Phase(), CallMediaSessionPhase::MediaReady);

  a_call_->DetachLeg(leg_id);
  harness_->PumpBoth();
  EXPECT_EQ(a_call_->Phase(), CallMediaSessionPhase::Idle);
  harness_->PumpBoth();
  EXPECT_EQ(local_failed.load(), 0);
}

TEST_F(CallMediaLegCoordinatorTest, ConnectTimeoutReturnsIdle) {
  ASSERT_TRUE(static_cast<bool>(
      harness_->mgr_a().RegisterEndpoint("blackhole", "/ip4/10.0.0.99/udp/59999/adp/1.0.0/p2p/QmBob")));

  CallMediaDirectConnectParams params;
  params.peer_key = "blackhole";
  params.call_id = "call-amp-connect-timeout";
  params.media_epoch = 1;
  params.media_key = ByteVector(32, 0x44);
  params.offerer = true;

  LegCompletion leg_done;
  const CallMediaLegId leg_id = a_call_->StartLeg(params, {}, leg_done.Fn(), 400);
  ASSERT_TRUE(leg_id);
  const auto wait_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
  while (!leg_done.finished.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < wait_until) {
    harness_->PumpBoth();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(leg_done.finished.load(std::memory_order_acquire));

  EXPECT_FALSE(leg_done.result);
  EXPECT_EQ(a_call_->Phase(), CallMediaSessionPhase::Idle);
  EXPECT_FALSE(a_call_->IsLegActive(leg_id));
}

TEST_F(CallMediaLegCoordinatorTest, ClearInboundHandlerRejectsLateInbound) {
  const std::string call_id = "call-amp-handler-cleared";
  ByteVector media_key(32, 0x55);

  a_call_->ClearInboundHandler();

  CallMediaDirectConnectParams params;
  params.peer_key = "a";
  params.call_id = call_id;
  params.media_epoch = 1;
  params.media_key = media_key;
  params.offerer = true;

  LegCompletion leg_done;
  const CallMediaLegId leg_id = b_call_->StartLeg(params, {}, leg_done.Fn(), 3000);
  ASSERT_TRUE(leg_id);
  const auto wait_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(6000);
  while (!leg_done.finished.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < wait_until) {
    harness_->PumpBoth();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(leg_done.finished.load(std::memory_order_acquire));

  EXPECT_FALSE(leg_done.result);
  EXPECT_EQ(a_call_->Phase(), CallMediaSessionPhase::Idle);
  EXPECT_FALSE(a_call_->IsLegActive(leg_id));
}

TEST_F(CallMediaLegCoordinatorTest, ConnectDetachKCycleNoHang) {
  constexpr int kCycles = 5;
  const ByteVector media_key(32, 0x42);

  for (int cycle = 0; cycle < kCycles; ++cycle) {
    const std::string call_id = "call-amp-kcycle-" + std::to_string(cycle);
    std::mutex mu;
    std::condition_variable cv;
    bool connected = false;
    bool got_audio = false;

    b_call_->SetInboundHandler([&](CallMediaDirectConnectParams& params, CallMediaDirectCallbacks& cbs) {
      params.media_key = media_key;
      params.call_id = call_id;
      params.media_epoch = 1;
      params.offerer = false;
      cbs.on_connected = [&] {
        std::lock_guard lock(mu);
        connected = true;
        cv.notify_one();
      };
      cbs.on_audio = [&](const std::vector<uint8_t>&) {
        std::lock_guard lock(mu);
        got_audio = true;
        cv.notify_one();
      };
    });

    CallMediaDirectConnectParams params;
    params.peer_key = "b";
    params.call_id = call_id;
    params.media_key = media_key;
    params.media_epoch = 1;
    params.offerer = true;

    CallMediaDirectCallbacks cbs;
    cbs.on_connected = [&] {
      std::lock_guard lock(mu);
      connected = true;
      cv.notify_one();
    };

    LegCompletion leg_done;
    const CallMediaLegId leg_id = a_call_->StartLeg(params, cbs, leg_done.Fn(), 5000);
    ASSERT_TRUE(leg_id) << "cycle " << cycle;
    leg_done.PumpUntilDone(*harness_);

    harness_->PumpUntil([&] { return connected; });

    const std::vector<uint8_t> opus = {static_cast<uint8_t>(0x10 + cycle)};
    ASSERT_TRUE(a_call_->SendAudio(leg_id, opus, 1, 0)) << "cycle " << cycle;

    harness_->PumpUntil([&] { return got_audio; });

    a_call_->DetachLeg(leg_id);
    b_call_->DetachLeg({});
    harness_->PumpBoth();
    EXPECT_EQ(a_call_->Phase(), CallMediaSessionPhase::Idle) << "cycle " << cycle;
    EXPECT_EQ(b_call_->Phase(), CallMediaSessionPhase::Idle) << "cycle " << cycle;
  }
}

TEST_F(CallMediaLegCoordinatorTest, DualDialExactlyOneAdoptEachSide) {
  const std::string call_id = "call-dual-dial";
  ByteVector media_key(32, 0x66);

  std::mutex mu;
  bool a_got_audio = false;
  bool b_got_audio = false;
  std::vector<uint8_t> a_received;
  std::vector<uint8_t> b_received;

  a_call_->SetInboundHandler([&](CallMediaDirectConnectParams& params, CallMediaDirectCallbacks& cbs) {
    params.media_key = media_key;
    params.call_id = call_id;
    params.media_epoch = 1;
    params.offerer = false;
    cbs.on_audio = [&](const std::vector<uint8_t>& opus) {
      std::lock_guard lock(mu);
      a_received = opus;
      a_got_audio = true;
    };
  });
  b_call_->SetInboundHandler([&](CallMediaDirectConnectParams& params, CallMediaDirectCallbacks& cbs) {
    params.media_key = media_key;
    params.call_id = call_id;
    params.media_epoch = 1;
    params.offerer = false;
    cbs.on_audio = [&](const std::vector<uint8_t>& opus) {
      std::lock_guard lock(mu);
      b_received = opus;
      b_got_audio = true;
    };
  });

  // Warm a single underlay (A→B), then alias it on B as "a". Mesh election ([A026]) keeps
  // one Connected PeerLink per PeerId; dual call-media opens share that mux (channel glare).
  // Ownership canary ([A027]): TearDown/CloseQuiet under dual open must not UAF.
  std::atomic<bool> a_assoc{false};
  harness_->mgr_a().EnsureAssociation("b", [&](pp::amp::PeerLinkManager::LinkRoe r) {
    a_assoc.store(static_cast<bool>(r), std::memory_order_release);
  });
  harness_->PumpUntil([&] { return a_assoc.load(std::memory_order_acquire); }, 2000);
  ASSERT_TRUE(a_assoc.load(std::memory_order_acquire));
  harness_->mgr_b().EnsureAssociation("a", {});
  harness_->PumpUntil([&] { return harness_->mgr_b().IsConnected("a"); }, 2000);
  ASSERT_TRUE(harness_->mgr_b().IsConnected("a"));
  ASSERT_TRUE(harness_->mgr_a().IsConnected("b"));

  CallMediaDirectConnectParams a_params;
  a_params.peer_key = "b";
  a_params.call_id = call_id;
  a_params.media_epoch = 1;
  a_params.media_key = media_key;
  a_params.offerer = true;

  CallMediaDirectConnectParams b_params;
  b_params.peer_key = "a";
  b_params.call_id = call_id;
  b_params.media_epoch = 1;
  b_params.media_key = media_key;
  b_params.offerer = true;

  CallMediaDirectCallbacks a_cbs;
  a_cbs.on_audio = [&](const std::vector<uint8_t>& opus) {
    std::lock_guard lock(mu);
    a_received = opus;
    a_got_audio = true;
  };
  CallMediaDirectCallbacks b_cbs;
  b_cbs.on_audio = [&](const std::vector<uint8_t>& opus) {
    std::lock_guard lock(mu);
    b_received = opus;
    b_got_audio = true;
  };

  auto a_done = std::make_shared<LegCompletion>();
  auto b_done = std::make_shared<LegCompletion>();
  const CallMediaLegId a_leg = a_call_->StartLeg(a_params, std::move(a_cbs), a_done->Fn(), 8000);
  const CallMediaLegId b_leg = b_call_->StartLeg(b_params, std::move(b_cbs), b_done->Fn(), 8000);
  ASSERT_TRUE(a_leg);
  ASSERT_TRUE(b_leg);

  harness_->PumpUntil(
      [&] {
        return a_done->finished.load(std::memory_order_acquire) && b_done->finished.load(std::memory_order_acquire) &&
               a_call_->Phase() == CallMediaSessionPhase::MediaReady &&
               b_call_->Phase() == CallMediaSessionPhase::MediaReady;
      },
      20000);

  ASSERT_TRUE(a_done->finished.load(std::memory_order_acquire));
  ASSERT_TRUE(b_done->finished.load(std::memory_order_acquire));
  ASSERT_TRUE(a_done->result) << a_done->result.error().message;
  ASSERT_TRUE(b_done->result) << b_done->result.error().message;
  EXPECT_EQ(a_call_->Phase(), CallMediaSessionPhase::MediaReady);
  EXPECT_EQ(b_call_->Phase(), CallMediaSessionPhase::MediaReady);
  EXPECT_TRUE(a_call_->IsLegActive(a_leg));
  EXPECT_TRUE(b_call_->IsLegActive(b_leg));

  const std::vector<uint8_t> a_opus = {0xa1, 0xa2};
  const std::vector<uint8_t> b_opus = {0xb1, 0xb2};
  ASSERT_TRUE(a_call_->SendAudio(a_leg, a_opus, 1, 0));
  ASSERT_TRUE(b_call_->SendAudio(b_leg, b_opus, 1, 0));
  harness_->PumpUntil([&] { return a_got_audio && b_got_audio; }, 1000);
  EXPECT_EQ(a_received, b_opus);
  EXPECT_EQ(b_received, a_opus);

  a_call_->DetachLeg(a_leg);
  b_call_->DetachLeg(b_leg);
  harness_->PumpBoth();
  EXPECT_EQ(a_call_->Phase(), CallMediaSessionPhase::Idle);
  EXPECT_EQ(b_call_->Phase(), CallMediaSessionPhase::Idle);
}

/** Regression: after inbound wins MediaReady, a late/yielded outbound must not TearDown the bundle. */
TEST_F(CallMediaLegCoordinatorTest, YieldedOutboundFailureKeepsInboundMediaReady) {
  const std::string call_id = "call-yield-keep-inbound";
  ByteVector media_key(32, 0x71);

  std::mutex mu;
  bool b_got_audio = false;
  std::vector<uint8_t> b_received;

  b_call_->SetInboundHandler([&](CallMediaDirectConnectParams& params, CallMediaDirectCallbacks& cbs) {
    params.media_key = media_key;
    params.call_id = call_id;
    params.media_epoch = 1;
    params.offerer = false;
    cbs.on_audio = [&](const std::vector<uint8_t>& opus) {
      std::lock_guard lock(mu);
      b_received = opus;
      b_got_audio = true;
    };
  });

  std::atomic<bool> a_assoc{false};
  harness_->mgr_a().EnsureAssociation("b", [&](pp::amp::PeerLinkManager::LinkRoe r) {
    a_assoc.store(static_cast<bool>(r), std::memory_order_release);
  });
  harness_->PumpUntil([&] { return a_assoc.load(std::memory_order_acquire); }, 2000);
  ASSERT_TRUE(a_assoc.load(std::memory_order_acquire));
  harness_->mgr_b().EnsureAssociation("a", {});
  harness_->PumpUntil([&] { return harness_->mgr_b().IsConnected("a"); }, 2000);
  ASSERT_TRUE(harness_->mgr_b().IsConnected("a"));

  CallMediaDirectConnectParams a_params;
  a_params.peer_key = "b";
  a_params.call_id = call_id;
  a_params.media_epoch = 1;
  a_params.media_key = media_key;
  a_params.offerer = true;

  auto a_done = std::make_shared<LegCompletion>();
  const CallMediaLegId a_leg = a_call_->StartLeg(a_params, {}, a_done->Fn(), 8000);
  ASSERT_TRUE(a_leg);

  harness_->PumpUntil(
      [&] {
        return a_done->finished.load(std::memory_order_acquire) &&
               a_call_->Phase() == CallMediaSessionPhase::MediaReady &&
               b_call_->Phase() == CallMediaSessionPhase::MediaReady;
      },
      20000);
  ASSERT_TRUE(a_done->result) << a_done->result.error().message;
  ASSERT_EQ(a_call_->Phase(), CallMediaSessionPhase::MediaReady);
  ASSERT_EQ(b_call_->Phase(), CallMediaSessionPhase::MediaReady);

  // Second StartLeg on the same call_id (simulates late dual-dial / Connect retry) must adopt
  // the live inbound winner — not TearDown MediaReady when a yielded outbound open fails.
  auto late = std::make_shared<LegCompletion>();
  const CallMediaLegId late_leg = a_call_->StartLeg(a_params, {}, late->Fn(), 2000);
  ASSERT_TRUE(late_leg);
  harness_->PumpUntil([&] { return late->finished.load(std::memory_order_acquire); }, 5000);
  ASSERT_TRUE(late->finished.load(std::memory_order_acquire));
  EXPECT_TRUE(late->result) << late->result.error().message;
  EXPECT_EQ(a_call_->Phase(), CallMediaSessionPhase::MediaReady);
  EXPECT_EQ(b_call_->Phase(), CallMediaSessionPhase::MediaReady);
  EXPECT_TRUE(a_call_->IsLegActive(late_leg));

  const std::vector<uint8_t> opus = {0xc1, 0xc2};
  ASSERT_TRUE(a_call_->SendAudio(late_leg, opus, 1, 0));
  harness_->PumpUntil([&] { return b_got_audio; }, 1000);
  EXPECT_EQ(b_received, opus);

  a_call_->DetachLeg(late_leg);
  b_call_->DetachLeg({});
  harness_->PumpBoth();
}

/** B-CONFLICT: while A–B is MediaReady, C's inbound is rejected; after A leaves, C can connect. */
struct AmpTripleMeshHarness {
  std::shared_ptr<pp::adp::VirtualClock> clock;
  std::shared_ptr<pp::adp::MemoryDatagramHub> hub;
  std::shared_ptr<pp::adp::MemoryDatagramIo> io_a;
  std::shared_ptr<pp::adp::MemoryDatagramIo> io_b;
  std::shared_ptr<pp::adp::MemoryDatagramIo> io_c;
  std::unique_ptr<pp::adp::Endpoint> ep_a;
  std::unique_ptr<pp::adp::Endpoint> ep_b;
  std::unique_ptr<pp::adp::Endpoint> ep_c;
  pp::adp::IpEndpoint addr_a;
  pp::adp::IpEndpoint addr_b;
  pp::adp::IpEndpoint addr_c;
  pp::amp::MshIdentity alice;
  pp::amp::MshIdentity bob;
  pp::amp::MshIdentity charlie;
  std::unique_ptr<pp::amp::MeshRuntime> runtime_a;
  std::unique_ptr<pp::amp::MeshRuntime> runtime_b;
  std::unique_ptr<pp::amp::MeshRuntime> runtime_c;
  std::string peer_id_a;
  std::string peer_id_b;
  std::string peer_id_c;
  std::string ma_a;
  std::string ma_b;
  std::string ma_c;

  pp::amp::PeerLinkManager& mgr_a() { return runtime_a->Links(); }
  pp::amp::PeerLinkManager& mgr_b() { return runtime_b->Links(); }
  pp::amp::PeerLinkManager& mgr_c() { return runtime_c->Links(); }

  static pp::Roe<std::unique_ptr<AmpTripleMeshHarness>> Create() {
    auto harness = std::make_unique<AmpTripleMeshHarness>();
    harness->clock = std::make_shared<pp::adp::VirtualClock>(1'000'000);
    harness->hub = pp::adp::MemoryDatagramIo::MakeHub();
    harness->addr_a = pp::adp::IpEndpoint::V4(10, 0, 0, 1, 1000);
    harness->addr_b = pp::adp::IpEndpoint::V4(10, 0, 0, 2, 2000);
    harness->addr_c = pp::adp::IpEndpoint::V4(10, 0, 0, 3, 3000);
    harness->io_a = std::make_shared<pp::adp::MemoryDatagramIo>(harness->hub, harness->addr_a);
    harness->io_b = std::make_shared<pp::adp::MemoryDatagramIo>(harness->hub, harness->addr_b);
    harness->io_c = std::make_shared<pp::adp::MemoryDatagramIo>(harness->hub, harness->addr_c);
    harness->ep_a = std::make_unique<pp::adp::Endpoint>(harness->io_a, harness->clock);
    harness->ep_b = std::make_unique<pp::adp::Endpoint>(harness->io_b, harness->clock);
    harness->ep_c = std::make_unique<pp::adp::Endpoint>(harness->io_c, harness->clock);
    harness->ep_b->SetAcceptEnabled(true);
    harness->ep_c->SetAcceptEnabled(true);

    auto alice_keys = pp::MlDsa::GenerateKeyPair();
    auto bob_keys = pp::MlDsa::GenerateKeyPair();
    auto charlie_keys = pp::MlDsa::GenerateKeyPair();
    if (!alice_keys || !bob_keys || !charlie_keys) {
      return pp::Error("triple harness: keygen failed");
    }
    harness->alice.ml_dsa_secret_key = std::move(alice_keys->secret_key);
    harness->alice.ml_dsa_public_key = std::move(alice_keys->public_key);
    harness->bob.ml_dsa_secret_key = std::move(bob_keys->secret_key);
    harness->bob.ml_dsa_public_key = std::move(bob_keys->public_key);
    harness->charlie.ml_dsa_secret_key = std::move(charlie_keys->secret_key);
    harness->charlie.ml_dsa_public_key = std::move(charlie_keys->public_key);

    auto alice_id = pbr::test::DeriveTestPeerId(harness->alice.ml_dsa_public_key);
    auto bob_id = pbr::test::DeriveTestPeerId(harness->bob.ml_dsa_public_key);
    auto charlie_id = pbr::test::DeriveTestPeerId(harness->charlie.ml_dsa_public_key);
    if (!alice_id || !bob_id || !charlie_id) {
      return pp::Error("triple harness: peer id derivation failed");
    }
    harness->peer_id_a = *alice_id;
    harness->peer_id_b = *bob_id;
    harness->peer_id_c = *charlie_id;

    const auto link_config = pbr::test::AmpMeshTestLinkConfig();
    harness->runtime_a =
        std::make_unique<pp::amp::MeshRuntime>(*harness->ep_a, harness->alice, harness->peer_id_a, link_config);
    harness->runtime_b =
        std::make_unique<pp::amp::MeshRuntime>(*harness->ep_b, harness->bob, harness->peer_id_b, link_config);
    harness->runtime_c =
        std::make_unique<pp::amp::MeshRuntime>(*harness->ep_c, harness->charlie, harness->peer_id_c, link_config);
    harness->runtime_a->Start();
    harness->runtime_b->Start();
    harness->runtime_c->Start();

    auto ma_b = pp::amp::FormatAdpMultiaddr(harness->addr_b, harness->peer_id_b);
    auto ma_a = pp::amp::FormatAdpMultiaddr(harness->addr_a, harness->peer_id_a);
    auto ma_c = pp::amp::FormatAdpMultiaddr(harness->addr_c, harness->peer_id_c);
    if (!ma_b || !ma_a || !ma_c) {
      return pp::Error("triple harness: multiaddr format failed");
    }
    harness->ma_a = *ma_a;
    harness->ma_b = *ma_b;
    harness->ma_c = *ma_c;
    return harness;
  }

  void PumpAll() {
    runtime_a->Pump();
    runtime_b->Pump();
    runtime_c->Pump();
    runtime_a->Tick();
    runtime_b->Tick();
    runtime_c->Tick();
  }

  void PumpUntil(const std::function<bool()>& done, const size_t max_rounds = 500) {
    for (size_t i = 0; i < max_rounds && !done(); ++i) {
      PumpAll();
    }
  }
};

TEST(CallMediaLegTripleTest, SecondInboundRejectedThenEndAndAccept) {
  ASSERT_GE(sodium_init(), 0);
  auto created = AmpTripleMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created));
  auto harness = std::move(*created);

  ASSERT_TRUE(static_cast<bool>(harness->mgr_a().RegisterEndpoint("b", harness->ma_b)));
  ASSERT_TRUE(static_cast<bool>(harness->mgr_b().RegisterEndpoint("a", harness->ma_a)));
  ASSERT_TRUE(static_cast<bool>(harness->mgr_b().RegisterEndpoint("c", harness->ma_c)));
  ASSERT_TRUE(static_cast<bool>(harness->mgr_c().RegisterEndpoint("b", harness->ma_b)));

  CallMediaLegCoordinator a_call(*harness->runtime_a);
  CallMediaLegCoordinator b_call(*harness->runtime_b);
  CallMediaLegCoordinator c_call(*harness->runtime_c);
  a_call.Start();
  b_call.Start();
  c_call.Start();

  const ByteVector media_key(32, 0x42);
  std::mutex mu;
  std::condition_variable cv;
  int b_connected = 0;
  int b_audio = 0;

  b_call.SetInboundHandler([&](CallMediaDirectConnectParams& params, CallMediaDirectCallbacks& cbs) {
    params.media_key = media_key;
    params.offerer = false;
    cbs.on_connected = [&] {
      std::lock_guard lock(mu);
      ++b_connected;
      cv.notify_all();
    };
    cbs.on_audio = [&](const std::vector<uint8_t>&) {
      std::lock_guard lock(mu);
      ++b_audio;
      cv.notify_all();
    };
  });

  CallMediaDirectConnectParams ab;
  ab.peer_key = "b";
  ab.call_id = "call-conflict-ab";
  ab.media_key = media_key;
  ab.media_epoch = 1;
  ab.offerer = true;
  std::atomic<bool> a_connected{false};
  CallMediaDirectCallbacks a_cbs;
  a_cbs.on_connected = [&] {
    a_connected.store(true, std::memory_order_release);
    cv.notify_all();
  };
  LegCompletion ab_done;
  const CallMediaLegId ab_leg = a_call.StartLeg(ab, a_cbs, ab_done.Fn(), 5000);
  ASSERT_TRUE(ab_leg);
  harness->PumpUntil([&] {
    return ab_done.finished.load(std::memory_order_acquire) && b_connected >= 1 && a_connected.load();
  }, 2000);
  ASSERT_TRUE(ab_done.result);

  ASSERT_TRUE(a_call.SendAudio(ab_leg, {0xab}, 1, 0));
  harness->PumpUntil([&] { return b_audio >= 1; });
  EXPECT_EQ(a_call.Phase(), CallMediaSessionPhase::MediaReady);
  EXPECT_EQ(b_call.Phase(), CallMediaSessionPhase::MediaReady);

  CallMediaDirectConnectParams cb;
  cb.peer_key = "b";
  cb.call_id = "call-conflict-cb";
  cb.media_key = media_key;
  cb.media_epoch = 1;
  cb.offerer = true;
  LegCompletion cb_busy;
  const CallMediaLegId cb_leg = c_call.StartLeg(cb, {}, cb_busy.Fn(), 2500);
  ASSERT_TRUE(cb_leg);
  harness->PumpUntil([&] { return cb_busy.finished.load(std::memory_order_acquire); }, 2000);
  ASSERT_FALSE(cb_busy.result) << "second inbound must not take the active session";
  EXPECT_EQ(b_call.Phase(), CallMediaSessionPhase::MediaReady);
  EXPECT_EQ(a_call.Phase(), CallMediaSessionPhase::MediaReady);

  a_call.DetachLeg(ab_leg);
  harness->PumpUntil([&] { return b_call.Phase() == CallMediaSessionPhase::Idle; }, 2000);
  EXPECT_EQ(b_call.Phase(), CallMediaSessionPhase::Idle);

  std::atomic<bool> c_connected{false};
  CallMediaDirectCallbacks c_cbs;
  c_cbs.on_connected = [&] {
    c_connected.store(true, std::memory_order_release);
    cv.notify_all();
  };
  LegCompletion cb_done;
  const CallMediaLegId cb_leg2 = c_call.StartLeg(cb, c_cbs, cb_done.Fn(), 5000);
  ASSERT_TRUE(cb_leg2);
  harness->PumpUntil([&] {
    return cb_done.finished.load(std::memory_order_acquire) && b_connected >= 2 && c_connected.load();
  }, 2000);
  ASSERT_TRUE(cb_done.result) << "end-and-accept after A left";

  ASSERT_TRUE(c_call.SendAudio(cb_leg2, {0xcb}, 1, 0));
  harness->PumpUntil([&] { return b_audio >= 2; });

  c_call.DetachLeg(cb_leg2);
  b_call.DetachLeg({});
  harness->PumpAll();
  c_call.Stop();
  b_call.Stop();
  a_call.Stop();
}

} // namespace
} // namespace pbr
