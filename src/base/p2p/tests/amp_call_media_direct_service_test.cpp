#include "base/p2p/AmpCallMediaDirectService.h"
#include "base/mesh/link/tests/mesh_test_harness.h"

#include <gtest/gtest.h>
#include <sodium.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace pbr {
namespace {

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

class AmpCallMediaDirectServiceTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_GE(sodium_init(), 0);
    stall_release_ = std::make_shared<std::atomic<bool>>(false);

    auto created = test::AmpMeshHarness::Create();
    ASSERT_TRUE(static_cast<bool>(created));
    harness_ = std::move(*created);

    ASSERT_TRUE(static_cast<bool>(harness_->mgr_a->RegisterEndpoint("b", harness_->ma_b)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_b->RegisterEndpoint("a", harness_->ma_a)));

    a_call_ = std::make_unique<AmpCallMediaDirectService>(*harness_->mgr_a, [this] { harness_->PumpBoth(); });
    b_call_ = std::make_unique<AmpCallMediaDirectService>(*harness_->mgr_b, [this] { harness_->PumpBoth(); });
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
  std::unique_ptr<test::AmpMeshHarness> harness_;
  std::unique_ptr<AmpCallMediaDirectService> a_call_;
  std::unique_ptr<AmpCallMediaDirectService> b_call_;
};

TEST_F(AmpCallMediaDirectServiceTest, HelloAndEncryptedAudioRoundTrip) {
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

  auto connect = a_call_->Connect(params, std::move(cbs), 5000);
  ASSERT_TRUE(connect) << connect.error().message;

  harness_->PumpUntil([&] {
    return connected && offerer_connected.load(std::memory_order_acquire);
  });
  ASSERT_TRUE(connected);
  ASSERT_TRUE(offerer_connected.load(std::memory_order_acquire));

  const std::vector<uint8_t> opus = {0xde, 0xad, 0xbe, 0xef};
  auto sent = a_call_->SendAudio(opus, 1, 0);
  ASSERT_TRUE(sent) << sent.error().message;

  harness_->PumpUntil([&] { return got_audio; });
  EXPECT_EQ(received, opus);
  EXPECT_EQ(a_call_->Phase(), CallMediaSessionPhase::MediaReady);
  EXPECT_EQ(b_call_->Phase(), CallMediaSessionPhase::MediaReady);

  a_call_->Detach();
  EXPECT_EQ(a_call_->Phase(), CallMediaSessionPhase::Idle);
  EXPECT_FALSE(a_call_->IsActive());
}

TEST_F(AmpCallMediaDirectServiceTest, DetachUnblocksConnectWait) {
  b_call_->Stop();
  b_call_ = std::make_unique<AmpCallMediaDirectService>(
      *harness_->mgr_b, [this] { harness_->PumpBoth(); },
      [](std::function<void()> fn) { std::thread(std::move(fn)).detach(); });
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

  std::atomic<bool> connect_done{false};
  Roe<void> connect_result = Error("not run");
  std::thread th([&] {
    connect_result = a_call_->Connect(params, {}, 15000);
    connect_done.store(true, std::memory_order_release);
  });

  WaitUntil(
      [&] {
        return b_stalled.load(std::memory_order_acquire) &&
               a_call_->Phase() == CallMediaSessionPhase::HelloOutbound;
      },
      std::chrono::milliseconds(5000));
  ASSERT_TRUE(b_stalled.load(std::memory_order_acquire));
  ASSERT_EQ(a_call_->Phase(), CallMediaSessionPhase::HelloOutbound);
  ASSERT_FALSE(connect_done.load(std::memory_order_acquire));

  a_call_->Detach();
  th.join();

  ASSERT_TRUE(connect_done.load(std::memory_order_acquire));
  EXPECT_FALSE(connect_result);
  EXPECT_EQ(connect_result.error().message, "call-media aborted");
  EXPECT_EQ(a_call_->Phase(), CallMediaSessionPhase::Idle);
}

TEST_F(AmpCallMediaDirectServiceTest, HelloAndEncryptedVideoRoundTripOver16KiB) {
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

  auto connect = a_call_->Connect(params, std::move(cbs), 5000);
  ASSERT_TRUE(connect) << connect.error().message;

  harness_->PumpUntil([&] {
    return connected && offerer_connected.load(std::memory_order_acquire);
  });

  const std::vector<uint8_t> au(20 * 1024, 0x5a);
  auto sent = a_call_->SendMedia(1, au, 1, 1);
  ASSERT_TRUE(sent) << sent.error().message;

  harness_->PumpUntil([&] { return got_video; });
  EXPECT_EQ(received_ch, 1);
  EXPECT_EQ(received, au);

  a_call_->Detach();
}

TEST_F(AmpCallMediaDirectServiceTest, FailAfterDetachDoesNotCallOnFailed) {
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

  ASSERT_TRUE(a_call_->Connect(params, std::move(cbs), 5000));
  ASSERT_EQ(a_call_->Phase(), CallMediaSessionPhase::MediaReady);

  a_call_->Detach();
  EXPECT_EQ(a_call_->Phase(), CallMediaSessionPhase::Idle);
  harness_->PumpBoth();
  EXPECT_EQ(local_failed.load(), 0);
}

TEST_F(AmpCallMediaDirectServiceTest, ConnectTimeoutReturnsIdle) {
  ASSERT_TRUE(static_cast<bool>(
      harness_->mgr_a->RegisterEndpoint("blackhole", "/ip4/10.0.0.99/udp/59999/adp/1.0.0/p2p/QmBob")));

  CallMediaDirectConnectParams params;
  params.peer_key = "blackhole";
  params.call_id = "call-amp-connect-timeout";
  params.media_epoch = 1;
  params.media_key = ByteVector(32, 0x44);
  params.offerer = true;

  auto result = a_call_->Connect(params, {}, 400);
  EXPECT_FALSE(result);
  EXPECT_EQ(a_call_->Phase(), CallMediaSessionPhase::Idle);
  EXPECT_FALSE(a_call_->IsActive());
}

TEST_F(AmpCallMediaDirectServiceTest, ClearInboundHandlerRejectsLateInbound) {
  const std::string call_id = "call-amp-handler-cleared";
  ByteVector media_key(32, 0x55);

  a_call_->ClearInboundHandler();

  CallMediaDirectConnectParams params;
  params.peer_key = "a";
  params.call_id = call_id;
  params.media_epoch = 1;
  params.media_key = media_key;
  params.offerer = true;

  auto result = b_call_->Connect(params, {}, 3000);
  EXPECT_FALSE(result);
  EXPECT_EQ(a_call_->Phase(), CallMediaSessionPhase::Idle);
  EXPECT_FALSE(a_call_->IsActive());
}

TEST_F(AmpCallMediaDirectServiceTest, ConnectDetachKCycleNoHang) {
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

    ASSERT_TRUE(a_call_->Connect(params, cbs, 5000)) << "cycle " << cycle;

    harness_->PumpUntil([&] { return connected; });

    const std::vector<uint8_t> opus = {static_cast<uint8_t>(0x10 + cycle)};
    ASSERT_TRUE(a_call_->SendAudio(opus, 1, 0)) << "cycle " << cycle;

    harness_->PumpUntil([&] { return got_audio; });

    a_call_->Detach();
    b_call_->Detach();
    EXPECT_EQ(a_call_->Phase(), CallMediaSessionPhase::Idle) << "cycle " << cycle;
    EXPECT_EQ(b_call_->Phase(), CallMediaSessionPhase::Idle) << "cycle " << cycle;
  }
}

} // namespace
} // namespace pbr
