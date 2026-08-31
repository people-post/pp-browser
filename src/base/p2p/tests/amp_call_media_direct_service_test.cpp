#include "base/p2p/AmpCallMediaDirectService.h"
#include "base/mesh/link/tests/mesh_test_harness.h"

#include <gtest/gtest.h>
#include <sodium.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace pbr {
namespace {

class AmpCallMediaDirectServiceTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_GE(sodium_init(), 0);
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
    a_call_->Stop();
    b_call_->Stop();
    a_call_.reset();
    b_call_.reset();
    harness_.reset();
  }

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

} // namespace
} // namespace pbr
