#include "base/p2p/tests/loopback_partition_fixture.h"

#include "base/p2p/CallMediaDirectService.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pbr {
namespace {

class CircuitCallMediaComposeTest : public test::LoopbackPartitionFixture {
protected:
  void SetUp() override {
    LoopbackPartitionFixture::SetUp();
    a_call_media_ = std::make_unique<CallMediaDirectService>(a_host_, *a_sessions_);
    b_call_media_ = std::make_unique<CallMediaDirectService>(b_host_, *b_sessions_);
    a_call_media_->Start();
    b_call_media_->Start();
  }

  void TearDown() override {
    if (a_call_media_) {
      a_call_media_->Stop();
    }
    if (b_call_media_) {
      b_call_media_->Stop();
    }
    a_call_media_.reset();
    b_call_media_.reset();
    LoopbackPartitionFixture::TearDown();
  }

  std::unique_ptr<CallMediaDirectService> a_call_media_;
  std::unique_ptr<CallMediaDirectService> b_call_media_;
};

TEST_F(CircuitCallMediaComposeTest, CircuitBackedHelloAndEncryptedAudioRoundTrip) {
  const std::string call_id = "call-circuit-compose";
  ByteVector media_key(32, 0x42);

  std::mutex mu;
  std::condition_variable cv;
  bool answerer_connected = false;
  bool got_audio = false;
  std::vector<uint8_t> received;

  // Arm answerer before circuit: bridge opens call-media on B during RequestBridge.
  b_call_media_->SetInboundHandler([&](CallMediaDirectConnectParams& params, CallMediaDirectCallbacks& cbs) {
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

  ASSERT_FALSE(a_sessions_->IsReachableForProtocol(b_peer_id_, kCallMediaDirectProtocolId));
  auto via = EnsureCircuitFromA(kCallMediaDirectProtocolId);
  ASSERT_TRUE(via) << via.error().message;
  ASSERT_TRUE(a_sessions_->IsCircuitBacked(b_peer_id_, kCallMediaDirectProtocolId));
  ASSERT_TRUE(a_sessions_->IsReachableForProtocol(b_peer_id_, kCallMediaDirectProtocolId));

  CallMediaDirectConnectParams params;
  params.peer_key = b_peer_id_;
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

  auto connect = a_call_media_->Connect(params, std::move(cbs), 8000);
  ASSERT_TRUE(connect) << connect.error().message;
  EXPECT_TRUE(a_sessions_->IsCircuitBacked(b_peer_id_, kCallMediaDirectProtocolId));

  {
    std::unique_lock lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(8), [&] { return answerer_connected; }));
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(8),
                            [&] { return offerer_connected.load(std::memory_order_acquire); }));
  }

  const std::vector<uint8_t> opus = {0xca, 0xfe, 0xba, 0xbe};
  auto sent = a_call_media_->SendAudio(opus, 1, 0);
  ASSERT_TRUE(sent) << sent.error().message;

  {
    std::unique_lock lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(8), [&] { return got_audio; }));
  }
  EXPECT_EQ(received, opus);
  EXPECT_EQ(a_call_media_->Phase(), CallMediaSessionPhase::MediaReady);
  EXPECT_EQ(b_call_media_->Phase(), CallMediaSessionPhase::MediaReady);

  a_call_media_->Detach();
  a_sessions_->ClearCircuitHop(b_peer_id_, kCallMediaDirectProtocolId);
  EXPECT_FALSE(a_sessions_->IsCircuitBacked(b_peer_id_, kCallMediaDirectProtocolId));
  EXPECT_EQ(a_call_media_->Phase(), CallMediaSessionPhase::Idle);
}

TEST_F(CircuitCallMediaComposeTest, CircuitBackedEncryptedVideoRoundTripOver16KiB) {
  const std::string call_id = "call-circuit-video";
  ByteVector media_key(32, 0x42);

  std::mutex mu;
  std::condition_variable cv;
  bool answerer_connected = false;
  bool got_video = false;
  std::vector<uint8_t> received;
  uint8_t received_ch = 255;

  b_call_media_->SetInboundHandler([&](CallMediaDirectConnectParams& params, CallMediaDirectCallbacks& cbs) {
    params.media_key = media_key;
    params.call_id = call_id;
    params.media_epoch = 1;
    params.offerer = false;
    cbs.on_connected = [&] {
      std::lock_guard lock(mu);
      answerer_connected = true;
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

  auto via = EnsureCircuitFromA(kCallMediaDirectProtocolId);
  ASSERT_TRUE(via) << via.error().message;

  CallMediaDirectConnectParams params;
  params.peer_key = b_peer_id_;
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

  auto connect = a_call_media_->Connect(params, std::move(cbs), 8000);
  ASSERT_TRUE(connect) << connect.error().message;

  {
    std::unique_lock lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(8), [&] { return answerer_connected; }));
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(8),
                            [&] { return offerer_connected.load(std::memory_order_acquire); }));
  }

  const std::vector<uint8_t> au(20 * 1024, 0x33);
  ASSERT_TRUE(a_call_media_->SendMedia(1, au, 1, 1));
  {
    std::unique_lock lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(8), [&] { return got_video; }));
  }
  EXPECT_EQ(received_ch, 1);
  EXPECT_EQ(received, au);

  a_call_media_->Detach();
  a_sessions_->ClearCircuitHop(b_peer_id_, kCallMediaDirectProtocolId);
}

} // namespace
} // namespace pbr
