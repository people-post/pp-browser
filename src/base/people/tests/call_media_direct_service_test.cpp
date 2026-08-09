#include "libp2p/integration/host/CallMediaDirectService.h"
#include "libp2p/integration/host/Libp2pHost.h"
#include "libp2p/integration/host/PeerSessionManager.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
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

class CallMediaDirectServiceTest : public ::testing::Test {
protected:
  void SetUp() override {
    stall_release_ = std::make_shared<std::atomic<bool>>(false);
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
    // Release any inbound-handler stall before WorkerPool::Shutdown joins.
    // Flag is shared_ptr so it outlives TestBody stack (UAF used to hang Stop).
    if (stall_release_) {
      stall_release_->store(true, std::memory_order_release);
    }
    a_call_media_->Stop();
    b_call_media_->Stop();
    a_call_media_.reset();
    b_call_media_.reset();
    a_sessions_.reset();
    b_sessions_.reset();
    a_host_.Stop();
    b_host_.Stop();
    stall_release_.reset();
  }

  int a_port_ = 0;
  int b_port_ = 0;
  std::string a_ma_;
  std::string b_ma_;
  /** Shared with tests that stall B's inbound handler on the Normal worker lane. */
  std::shared_ptr<std::atomic<bool>> stall_release_;
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
  // stall_release_ is fixture-owned shared_ptr: TearDown flips it before host Stop so the
  // Normal-lane worker can leave the sleep loop (stack atomic + RAII UAF used to hang Shutdown).
  const std::string call_id = "call-detach-wait";
  ByteVector media_key(32, 0x11);
  std::atomic<bool> b_stalled{false};
  auto release_b = stall_release_;
  ASSERT_TRUE(release_b);

  b_call_media_->SetInboundHandler([release_b, &media_key, &call_id, &b_stalled](
                                       CallMediaDirectConnectParams& params,
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
    connect_result = a_call_media_->Connect(params, {}, 15000);
    connect_done.store(true, std::memory_order_release);
  });
  struct JoinThread {
    std::thread& t;
    ~JoinThread() {
      if (t.joinable()) {
        t.join();
      }
    }
  } join_guard{th};

  // Wait until hello was exchanged (B in stall ⇒ A has written hello and is in ack ReadJson).
  WaitUntil([&] { return b_stalled.load(std::memory_order_acquire) &&
                         a_call_media_->Phase() == CallMediaSessionPhase::HelloOutbound; },
            std::chrono::milliseconds(5000));
  ASSERT_TRUE(b_stalled.load(std::memory_order_acquire));
  ASSERT_EQ(a_call_media_->Phase(), CallMediaSessionPhase::HelloOutbound);
  ASSERT_FALSE(connect_done.load(std::memory_order_acquire));

  a_call_media_->Detach();
  th.join();

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
  // Sync on peer observing the close instead of a wall-clock settle sleep.
  a_call_media_->Detach();
  EXPECT_EQ(a_call_media_->Phase(), CallMediaSessionPhase::Idle);
  WaitUntil([&] { return !b_call_media_->IsActive(); }, std::chrono::milliseconds(5000));
  EXPECT_EQ(local_failed.load(), 0);
}

TEST_F(CallMediaDirectServiceTest, ConnectTimeoutReturnsIdleAndIgnoresLateOpen) {
  // SESSION_MACHINES golden #7 (loopback wiring). Stall-the-peer patterns leave an outbound
  // hello ReadJson alive across TearDown and poison DualDial; dial a blackhole instead.
  // Idle+late OpenStreamOk ignore is covered by CallMediaSessionLogicTest.
  auto peer_id = b_host_.LocalPeerIdBase58();
  ASSERT_TRUE(peer_id);
  ASSERT_TRUE(a_sessions_->RegisterEndpoint(
      "blackhole", "/ip4/192.0.2.1/tcp/4001/p2p/" + *peer_id));

  CallMediaDirectConnectParams params;
  params.peer_key = "blackhole";
  params.call_id = "call-connect-timeout";
  params.media_epoch = 1;
  params.media_key = ByteVector(32, 0x44);
  params.offerer = true;

  auto result = a_call_media_->Connect(params, {}, 400);
  EXPECT_FALSE(result);
  EXPECT_TRUE(result.error().message == "call-media connect timed out" ||
              result.error().message.find("call-media dial failed") != std::string::npos)
      << result.error().message;
  EXPECT_EQ(a_call_media_->Phase(), CallMediaSessionPhase::Idle);
  EXPECT_FALSE(a_call_media_->IsActive());
}

TEST_F(CallMediaDirectServiceTest, ClearInboundHandlerRejectsLateInbound) {
  // SESSION_MACHINES golden #6: ClearInboundHandler → late inbound no-ops.
  const std::string call_id = "call-handler-cleared";
  ByteVector media_key(32, 0x55);

  a_call_media_->ClearInboundHandler();

  CallMediaDirectConnectParams params;
  params.peer_key = "a";
  params.call_id = call_id;
  params.media_epoch = 1;
  params.media_key = media_key;
  params.offerer = true;

  auto result = b_call_media_->Connect(params, {}, 3000);
  EXPECT_FALSE(result);
  EXPECT_EQ(a_call_media_->Phase(), CallMediaSessionPhase::Idle);
  EXPECT_FALSE(a_call_media_->IsActive());
}

TEST_F(CallMediaDirectServiceTest, DualDialExactlyOneAdoptEachSide) {
  // SESSION_MACHINES golden #3: dual dial → one adopt per side; no hang.
  const std::string call_id = "call-dual-dial";
  ByteVector media_key(32, 0x66);

  a_call_media_->SetInboundHandler([&](CallMediaDirectConnectParams& params, CallMediaDirectCallbacks&) {
    params.media_key = media_key;
    params.call_id = call_id;
    params.media_epoch = 1;
    params.offerer = false;
  });
  b_call_media_->SetInboundHandler([&](CallMediaDirectConnectParams& params, CallMediaDirectCallbacks&) {
    params.media_key = media_key;
    params.call_id = call_id;
    params.media_epoch = 1;
    params.offerer = false;
  });

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

  Roe<void> a_result = Error("not run");
  Roe<void> b_result = Error("not run");
  std::thread ta([&] { a_result = a_call_media_->Connect(a_params, {}, 8000); });
  std::thread tb([&] { b_result = b_call_media_->Connect(b_params, {}, 8000); });
  ta.join();
  tb.join();

  ASSERT_TRUE(a_result) << a_result.error().message;
  ASSERT_TRUE(b_result) << b_result.error().message;
  EXPECT_EQ(a_call_media_->Phase(), CallMediaSessionPhase::MediaReady);
  EXPECT_EQ(b_call_media_->Phase(), CallMediaSessionPhase::MediaReady);
  EXPECT_TRUE(a_call_media_->IsActive());
  EXPECT_TRUE(b_call_media_->IsActive());

  a_call_media_->Detach();
  b_call_media_->Detach();
  EXPECT_EQ(a_call_media_->Phase(), CallMediaSessionPhase::Idle);
  EXPECT_EQ(b_call_media_->Phase(), CallMediaSessionPhase::Idle);
}

} // namespace
} // namespace pbr
