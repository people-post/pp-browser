#include "feature/messaging/Libp2pDirectChatService.h"

#include "base/messaging/MessagingJson.h"
#include "base/messaging/RelayWirePayload.h"
#include "base/p2p/CallMediaDirectService.h"
#include "base/p2p/tests/loopback_partition_fixture.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include "common/PbrCompat.h"

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

/** Same-session mix: A↛B except via R; call-media + product chat on distinct circuit hops. */
class CircuitCallMediaChatComposeTest : public test::LoopbackPartitionFixture {
protected:
  void SetUp() override {
    LoopbackPartitionFixture::SetUp();
    a_call_ = std::make_unique<CallMediaDirectService>(a_host_, *a_sessions_);
    b_call_ = std::make_unique<CallMediaDirectService>(b_host_, *b_sessions_);
    a_chat_ = std::make_unique<Libp2pDirectChatService>(a_host_, *a_sessions_);
    b_chat_ = std::make_unique<Libp2pDirectChatService>(b_host_, *b_sessions_);
    a_call_->Start();
    b_call_->Start();
    a_chat_->Start();
    b_chat_->Start();
  }

  void TearDown() override {
    if (a_call_) {
      a_call_->Stop();
    }
    if (b_call_) {
      b_call_->Stop();
    }
    if (a_chat_) {
      a_chat_->Stop();
    }
    if (b_chat_) {
      b_chat_->Stop();
    }
    a_call_.reset();
    b_call_.reset();
    a_chat_.reset();
    b_chat_.reset();
    LoopbackPartitionFixture::TearDown();
  }

  Roe<void> SendChatViaCircuit(const std::string& message_id, const std::string& text) {
    // Pass B's multiaddr so TryEnsureHopViaCircuit does not RegisterEndpoint(A→B).
    auto via = a_sessions_->TryEnsureHopViaCircuit(b_peer_id_, *a_circuit_, {r_peer_id_},
                                                   kDirectChatProtocolId, 8000, b_ma_);
    if (!via) {
      return via;
    }
    auto sent = a_chat_->SendEnvelope(b_peer_id_, MakeTestEnvelope(message_id, text));
    a_sessions_->ClearCircuitHop(b_peer_id_, kDirectChatProtocolId);
    return sent;
  }

  std::unique_ptr<CallMediaDirectService> a_call_;
  std::unique_ptr<CallMediaDirectService> b_call_;
  std::unique_ptr<Libp2pDirectChatService> a_chat_;
  std::unique_ptr<Libp2pDirectChatService> b_chat_;
};

TEST_F(CircuitCallMediaChatComposeTest, ChatDuringAndAfterCallViaCircuit) {
  ASSERT_FALSE(a_sessions_->IsDialable(b_peer_id_));
  ASSERT_FALSE(a_chat_->IsPeerReachable(b_peer_id_));

  std::mutex mu;
  std::condition_variable cv;
  int chat_got = 0;
  bool call_connected = false;
  bool got_audio = false;
  std::string last_chat_id;

  b_chat_->SetInboundHandler([&](RelayEnvelope envelope) {
    std::lock_guard lock(mu);
    last_chat_id = envelope.message_id;
    ++chat_got;
    cv.notify_all();
  });

  ASSERT_TRUE(SendChatViaCircuit("msg-before", "before")) << "chat before call via circuit";
  {
    std::unique_lock lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return chat_got >= 1; }));
  }
  EXPECT_EQ(last_chat_id, "msg-before");
  EXPECT_FALSE(a_sessions_->IsCircuitBacked(b_peer_id_, kDirectChatProtocolId));
  EXPECT_FALSE(a_sessions_->IsDialable(b_peer_id_)) << "chat hop must not install a direct A→B path";

  const ByteVector media_key(32, 0x42);
  b_call_->SetInboundHandler([&](CallMediaDirectConnectParams& params, CallMediaDirectCallbacks& cbs) {
    params.media_key = media_key;
    params.call_id = "call-circuit-chat";
    params.media_epoch = 1;
    params.offerer = false;
    cbs.on_connected = [&] {
      std::lock_guard lock(mu);
      call_connected = true;
      cv.notify_all();
    };
    cbs.on_audio = [&](const std::vector<uint8_t>&) {
      std::lock_guard lock(mu);
      got_audio = true;
      cv.notify_all();
    };
  });

  auto media_hop = a_sessions_->TryEnsureHopViaCircuit(b_peer_id_, *a_circuit_, {r_peer_id_},
                                                      kCallMediaDirectProtocolId, 8000, b_ma_);
  ASSERT_TRUE(media_hop) << media_hop.error().message;
  ASSERT_TRUE(a_sessions_->IsCircuitBacked(b_peer_id_, kCallMediaDirectProtocolId));
  EXPECT_FALSE(a_chat_->IsPeerReachable(b_peer_id_)) << "media hop must not imply chat reachability";

  CallMediaDirectConnectParams params;
  params.peer_key = b_peer_id_;
  params.call_id = "call-circuit-chat";
  params.media_key = media_key;
  params.media_epoch = 1;
  params.offerer = true;
  ASSERT_TRUE(a_call_->Connect(params, {}, 8000));
  {
    std::unique_lock lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(8), [&] { return call_connected; }));
  }
  ASSERT_TRUE(a_call_->SendAudio({0x11}, 1, 0));
  {
    std::unique_lock lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(8), [&] { return got_audio; }));
  }
  EXPECT_EQ(a_call_->Phase(), CallMediaSessionPhase::MediaReady);

  ASSERT_TRUE(SendChatViaCircuit("msg-during", "during")) << "chat during MediaReady via circuit";
  {
    std::unique_lock lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(8), [&] { return chat_got >= 2; }));
  }
  EXPECT_EQ(last_chat_id, "msg-during");
  EXPECT_TRUE(a_sessions_->IsCircuitBacked(b_peer_id_, kCallMediaDirectProtocolId));
  got_audio = false;
  ASSERT_TRUE(a_call_->SendAudio({0x22}, 2, 0)) << "audio after chat during call";
  {
    std::unique_lock lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(8), [&] { return got_audio; }));
  }

  a_call_->Detach();
  a_sessions_->ClearCircuitHop(b_peer_id_, kCallMediaDirectProtocolId);
  EXPECT_EQ(a_call_->Phase(), CallMediaSessionPhase::Idle);
  EXPECT_FALSE(a_sessions_->IsCircuitBacked(b_peer_id_, kCallMediaDirectProtocolId));

  ASSERT_TRUE(SendChatViaCircuit("msg-after", "after")) << "chat after leave via circuit";
  {
    std::unique_lock lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(8), [&] { return chat_got >= 3; }));
  }
  EXPECT_EQ(last_chat_id, "msg-after");
}

} // namespace
} // namespace pbr
