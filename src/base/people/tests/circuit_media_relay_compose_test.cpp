#include "base/people/tests/loopback_partition_fixture.h"

#include "libp2p/integration/host/MediaRelayService.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace pbr {
namespace {

/**
 * Partition: A ↛ hop B; A circuits via R. Guest G dials hop B directly.
 * B hosts media_relay (SFU). Proves SoftMigrate-style quote/attach/fan-out over circuit.
 */
class CircuitMediaRelayComposeTest : public test::LoopbackPartitionFixture {
protected:
  void SetUp() override {
    LoopbackPartitionFixture::SetUp();

    hop_relay_ = std::make_unique<MediaRelayService>(b_host_, *b_sessions_);
    hop_relay_->Start();

    a_relay_ = std::make_unique<MediaRelayService>(a_host_, *a_sessions_);

    PeerSessionConfig config;
    config.dial_timeout = std::chrono::milliseconds(3000);
    config.dial_failure_backoff = std::chrono::milliseconds(100);
    auto g_started = test::StartEphemeralLoopbackHost(g_host_, g_port_);
    ASSERT_TRUE(g_started) << g_started.error().message;
    g_sessions_ = std::make_unique<PeerSessionManager>(g_host_, config);
    g_relay_ = std::make_unique<MediaRelayService>(g_host_, *g_sessions_);
    // Guest has direct path to hop (B).
    ASSERT_TRUE(g_sessions_->RegisterEndpoint(b_peer_id_, b_ma_));
  }

  void TearDown() override {
    if (a_relay_) {
      a_relay_->Detach();
    }
    if (g_relay_) {
      g_relay_->Detach();
    }
    a_relay_.reset();
    g_relay_.reset();
    hop_relay_.reset();
    g_sessions_.reset();
    g_host_.Stop();
    LoopbackPartitionFixture::TearDown();
  }

  int g_port_ = 0;
  Libp2pHost g_host_;
  std::unique_ptr<PeerSessionManager> g_sessions_;
  std::unique_ptr<MediaRelayService> hop_relay_;
  std::unique_ptr<MediaRelayService> a_relay_;
  std::unique_ptr<MediaRelayService> g_relay_;
};

TEST_F(CircuitMediaRelayComposeTest, CircuitBackedQuoteAttachFanout) {
  // Hop must be listening before circuit opens media-relay control stream.
  ASSERT_TRUE(hop_relay_->IsStarted());

  ASSERT_FALSE(a_sessions_->IsReachableForProtocol(b_peer_id_, kMediaRelayProtocolId));
  auto via = EnsureCircuitFromA(kMediaRelayProtocolId);
  ASSERT_TRUE(via) << via.error().message;
  ASSERT_TRUE(a_sessions_->IsCircuitBacked(b_peer_id_, kMediaRelayProtocolId));
  ASSERT_TRUE(a_sessions_->IsReachableForProtocol(b_peer_id_, kMediaRelayProtocolId));

  const std::string call_id = "call-circuit-sfu";
  MediaRelayQuoteRequest qreq;
  qreq.call_id = call_id;
  qreq.participants = 2;

  auto qa = a_relay_->RequestQuote(b_peer_id_, qreq, 8000);
  ASSERT_TRUE(qa) << qa.error().message;
  ASSERT_TRUE(qa->ok) << qa->error;
  EXPECT_EQ(qa->pricing_mode, "volunteer");
  EXPECT_TRUE(a_sessions_->IsCircuitBacked(b_peer_id_, kMediaRelayProtocolId));

  auto qg = g_relay_->RequestQuote(b_peer_id_, qreq, 8000);
  ASSERT_TRUE(qg) << qg.error().message;
  ASSERT_TRUE(qg->ok) << qg->error;

  std::mutex mu;
  std::condition_variable cv;
  bool got = false;
  MediaDataFrame received;

  auto attach_a = a_relay_->AcceptAndAttach(b_peer_id_, qa->quote_id, call_id, call_id,
                                            [](MediaDataFrame) {}, 8000);
  ASSERT_TRUE(attach_a) << attach_a.error().message;
  ASSERT_TRUE(attach_a->ok) << attach_a->error;
  EXPECT_TRUE(a_sessions_->IsCircuitBacked(b_peer_id_, kMediaRelayProtocolId));

  auto attach_g = g_relay_->AcceptAndAttach(
      b_peer_id_, qg->quote_id, call_id, call_id,
      [&](MediaDataFrame frame) {
        std::lock_guard lock(mu);
        received = std::move(frame);
        got = true;
        cv.notify_one();
      },
      8000);
  ASSERT_TRUE(attach_g) << attach_g.error().message;
  ASSERT_TRUE(attach_g->ok) << attach_g->error;

  a_relay_->StartClientFrameReader();
  g_relay_->StartClientFrameReader();

  ASSERT_TRUE(g_relay_->Subscribe(1, 0));

  MediaDataFrame sent;
  sent.stream_id = 1;
  sent.channel_id = 0;
  sent.channel_type = MediaChannelType::LatestLossy;
  sent.seq = 1;
  sent.payload = {'c', 'i', 'r'};
  {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    uint32_t seq = 1;
    while (std::chrono::steady_clock::now() < deadline) {
      {
        std::lock_guard lock(mu);
        if (got) {
          break;
        }
      }
      sent.seq = seq++;
      ASSERT_TRUE(a_relay_->SendFrame(sent));
      std::unique_lock lock(mu);
      if (cv.wait_for(lock, std::chrono::milliseconds(50), [&] { return got; })) {
        break;
      }
    }
    std::lock_guard lock(mu);
    ASSERT_TRUE(got);
  }
  EXPECT_EQ(received.stream_id, 1u);
  EXPECT_EQ(received.payload, sent.payload);

  a_relay_->Detach();
  g_relay_->Detach();
  a_sessions_->ClearCircuitHop(b_peer_id_, kMediaRelayProtocolId);
  EXPECT_FALSE(a_sessions_->IsCircuitBacked(b_peer_id_, kMediaRelayProtocolId));
}

} // namespace
} // namespace pbr
