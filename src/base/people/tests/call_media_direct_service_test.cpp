#include "libp2p/integration/host/CallMediaDirectService.h"
#include "libp2p/integration/host/Libp2pHost.h"
#include "libp2p/integration/host/PeerSessionManager.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
static int ProcessId() { return _getpid(); }
#else
#include <unistd.h>
static int ProcessId() { return static_cast<int>(getpid()); }
#endif

namespace pbr {
namespace {

class CallMediaDirectServiceTest : public ::testing::Test {
protected:
  void SetUp() override {
    static std::atomic<int> port{44000 + (ProcessId() % 2000) * 10};
    a_port_ = port.fetch_add(1);
    b_port_ = port.fetch_add(1);

    PeerSessionConfig config;
    config.dial_timeout = std::chrono::milliseconds(3000);
    config.dial_failure_backoff = std::chrono::milliseconds(100);

    Libp2pHostConfig a_cfg;
    a_cfg.listen_multiaddr = "/ip4/127.0.0.1/tcp/" + std::to_string(a_port_);
    ASSERT_TRUE(a_host_.Start(a_cfg));
    a_sessions_ = std::make_unique<PeerSessionManager>(a_host_, config);
    a_call_media_ = std::make_unique<CallMediaDirectService>(a_host_, *a_sessions_);

    Libp2pHostConfig b_cfg;
    b_cfg.listen_multiaddr = "/ip4/127.0.0.1/tcp/" + std::to_string(b_port_);
    ASSERT_TRUE(b_host_.Start(b_cfg));
    b_sessions_ = std::make_unique<PeerSessionManager>(b_host_, config);
    b_call_media_ = std::make_unique<CallMediaDirectService>(b_host_, *b_sessions_);

    auto a_id = a_host_.LocalPeerIdBase58();
    auto b_id = b_host_.LocalPeerIdBase58();
    ASSERT_TRUE(a_id);
    ASSERT_TRUE(b_id);
    b_ma_ = "/ip4/127.0.0.1/tcp/" + std::to_string(b_port_) + "/p2p/" + *b_id;
    a_ma_ = "/ip4/127.0.0.1/tcp/" + std::to_string(a_port_) + "/p2p/" + *a_id;
    ASSERT_TRUE(a_sessions_->RegisterEndpoint("b", b_ma_));
    ASSERT_TRUE(b_sessions_->RegisterEndpoint("a", a_ma_));

    a_call_media_->Start();
    b_call_media_->Start();
  }

  void TearDown() override {
    a_call_media_->Stop();
    b_call_media_->Stop();
    a_call_media_.reset();
    b_call_media_.reset();
    a_sessions_.reset();
    b_sessions_.reset();
    a_host_.Stop();
    b_host_.Stop();
  }

  int a_port_ = 0;
  int b_port_ = 0;
  std::string a_ma_;
  std::string b_ma_;
  Libp2pHost a_host_;
  Libp2pHost b_host_;
  std::unique_ptr<PeerSessionManager> a_sessions_;
  std::unique_ptr<PeerSessionManager> b_sessions_;
  std::unique_ptr<CallMediaDirectService> a_call_media_;
  std::unique_ptr<CallMediaDirectService> b_call_media_;
};

TEST_F(CallMediaDirectServiceTest, HelloAndEncryptedAudioRoundTrip) {
  const std::string call_id = "call-duplex-test";
  ByteVector media_key(32, 0x42);

  std::mutex mu;
  std::condition_variable cv;
  bool connected = false;
  bool got_audio = false;
  std::vector<uint8_t> received;

  b_call_media_->SetInboundHandler([&](CallMediaDirectConnectParams& params, CallMediaDirectCallbacks& cbs) {
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

  auto connect = a_call_media_->Connect(params, std::move(cbs), 5000);
  ASSERT_TRUE(connect) << connect.error().message;

  {
    std::unique_lock lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return connected; }));
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5),
                            [&] { return offerer_connected.load(std::memory_order_acquire); }));
  }

  const std::vector<uint8_t> opus = {0xde, 0xad, 0xbe, 0xef};
  auto sent = a_call_media_->SendAudio(opus, 1, 0);
  ASSERT_TRUE(sent) << sent.error().message;

  {
    std::unique_lock lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return got_audio; }));
  }
  EXPECT_EQ(received, opus);
  EXPECT_EQ(a_call_media_->Phase(), CallMediaSessionPhase::MediaReady);
  EXPECT_EQ(b_call_media_->Phase(), CallMediaSessionPhase::MediaReady);

  a_call_media_->Detach();
  EXPECT_EQ(a_call_media_->Phase(), CallMediaSessionPhase::Idle);
  EXPECT_FALSE(a_call_media_->IsActive());
}

TEST_F(CallMediaDirectServiceTest, DetachUnblocksConnectWait) {
  // B stalls inside inbound hello handler so A's Connect blocks in hello_ack read.
  const std::string call_id = "call-detach-wait";
  ByteVector media_key(32, 0x11);
  std::atomic<bool> release_b{false};
  b_call_media_->SetInboundHandler([&](CallMediaDirectConnectParams& params, CallMediaDirectCallbacks&) {
    params.media_key = media_key;
    params.call_id = call_id;
    params.media_epoch = 1;
    while (!release_b.load(std::memory_order_acquire)) {
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
    connect_result = a_call_media_->Connect(params, {}, 15000);
    connect_done.store(true, std::memory_order_release);
  });

  for (int i = 0; i < 100 && a_call_media_->Phase() != CallMediaSessionPhase::HelloOutbound; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  ASSERT_EQ(a_call_media_->Phase(), CallMediaSessionPhase::HelloOutbound);
  ASSERT_FALSE(connect_done.load(std::memory_order_acquire));

  a_call_media_->Detach();
  th.join();
  release_b.store(true, std::memory_order_release);

  ASSERT_TRUE(connect_done.load(std::memory_order_acquire));
  EXPECT_FALSE(connect_result);
  EXPECT_EQ(connect_result.error().message, "call-media aborted");
  EXPECT_EQ(a_call_media_->Phase(), CallMediaSessionPhase::Idle);
}

TEST_F(CallMediaDirectServiceTest, FailAfterDetachDoesNotCallOnFailed) {
  const std::string call_id = "call-fail-after-detach";
  ByteVector media_key(32, 0x33);
  std::atomic<int> local_failed{0};

  b_call_media_->SetInboundHandler([&](CallMediaDirectConnectParams& params, CallMediaDirectCallbacks&) {
    params.media_key = media_key;
    params.call_id = call_id;
    params.media_epoch = 1;
    // Peer may see read_eof when we Detach — that is remote close, not this assertion.
  });

  CallMediaDirectConnectParams params;
  params.peer_key = "b";
  params.call_id = call_id;
  params.media_epoch = 1;
  params.media_key = media_key;
  params.offerer = true;
  CallMediaDirectCallbacks cbs;
  cbs.on_failed = [&](const std::string&) { local_failed.fetch_add(1); };

  ASSERT_TRUE(a_call_media_->Connect(params, std::move(cbs), 5000));
  ASSERT_EQ(a_call_media_->Phase(), CallMediaSessionPhase::MediaReady);

  // Intentional local Detach: late duplex EOF on this service must not notify on_failed.
  a_call_media_->Detach();
  EXPECT_EQ(a_call_media_->Phase(), CallMediaSessionPhase::Idle);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(local_failed.load(), 0);
}

} // namespace
} // namespace pbr
