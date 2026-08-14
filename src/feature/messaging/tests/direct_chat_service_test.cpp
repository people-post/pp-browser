#include "feature/messaging/Libp2pDirectChatService.h"

#include "base/messaging/MessagingJson.h"
#include "base/messaging/RelayWirePayload.h"
#include "base/people/tests/libp2p_ephemeral_listen.h"
#include "libp2p/integration/host/Libp2pHost.h"
#include "libp2p/integration/host/PeerSessionManager.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>

namespace pbr {
namespace {

RelayEnvelope MakeTestEnvelope(const std::string& message_id, const std::string& text) {
  RelayEnvelope envelope;
  envelope.envelope_version = kRelayEnvelopeVersion;
  envelope.message_id = message_id;
  envelope.sender_relay_id = "relay:sender";
  envelope.sender_contact_id = "relay:sender";
  envelope.route.kind = "direct";
  envelope.route.channel = ThreadChannel::E2e;
  auto payload_b64 = RelayWirePayload::EncodePlaintextText(text);
  envelope.body.e2e.payload_b64 = payload_b64 ? *payload_b64 : "";
  envelope.timestamp = 1;
  return envelope;
}

class Libp2pDirectChatServiceTest : public ::testing::Test {
protected:
  void SetUp() override {
    PeerSessionConfig config;
    config.dial_timeout = std::chrono::milliseconds(3000);
    config.dial_failure_backoff = std::chrono::milliseconds(100);

    auto a_started = test::StartEphemeralLoopbackHost(a_host_, a_port_);
    ASSERT_TRUE(a_started) << a_started.error().message;
    a_sessions_ = std::make_unique<PeerSessionManager>(a_host_, config);
    a_chat_ = std::make_unique<Libp2pDirectChatService>(a_host_, *a_sessions_);

    auto b_started = test::StartEphemeralLoopbackHost(b_host_, b_port_);
    ASSERT_TRUE(b_started) << b_started.error().message;
    b_sessions_ = std::make_unique<PeerSessionManager>(b_host_, config);
    b_chat_ = std::make_unique<Libp2pDirectChatService>(b_host_, *b_sessions_);

    auto a_id = a_host_.LocalPeerIdBase58();
    auto b_id = b_host_.LocalPeerIdBase58();
    ASSERT_TRUE(a_id);
    ASSERT_TRUE(b_id);
    b_ma_ = test::LoopbackP2pMultiaddr(b_port_, *b_id);
    a_ma_ = test::LoopbackP2pMultiaddr(a_port_, *a_id);
    ASSERT_TRUE(a_sessions_->RegisterEndpoint("b", b_ma_));
    ASSERT_TRUE(b_sessions_->RegisterEndpoint("a", a_ma_));

    a_chat_->Start();
    b_chat_->Start();
  }

  void TearDown() override {
    a_chat_->Stop();
    b_chat_->Stop();
    a_chat_.reset();
    b_chat_.reset();
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
  std::unique_ptr<Libp2pDirectChatService> a_chat_;
  std::unique_ptr<Libp2pDirectChatService> b_chat_;
};

TEST_F(Libp2pDirectChatServiceTest, SendEnvelopeRoundTrip) {
  std::mutex mu;
  std::condition_variable cv;
  bool got = false;
  RelayEnvelope received;

  b_chat_->SetInboundHandler([&](RelayEnvelope envelope) {
    std::lock_guard lock(mu);
    received = std::move(envelope);
    got = true;
    cv.notify_one();
  });

  const auto sent = a_chat_->SendEnvelope("b", MakeTestEnvelope("msg-1", "hello direct"));
  ASSERT_TRUE(sent) << sent.error().message;

  {
    std::unique_lock lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return got; }));
  }
  EXPECT_EQ(received.message_id, "msg-1");
}

} // namespace
} // namespace pbr
