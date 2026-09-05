#include "feature/conversations/AmpDirectChatService.h"

#include "common/chat/MessagingJson.h"
#include "domain/messaging/RelayWirePayload.h"
#include "domain/mesh/tests/support/mesh_test_harness.h"

#include <gtest/gtest.h>
#include <sodium.h>

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

class AmpDirectChatServiceTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_GE(sodium_init(), 0);
    auto created = pbr::test::AmpMeshHarness::Create();
    ASSERT_TRUE(static_cast<bool>(created));
    harness_ = std::move(*created);

    ASSERT_TRUE(static_cast<bool>(harness_->chat_a().RegisterEndpoint("b", harness_->ma_b)));
    ASSERT_TRUE(static_cast<bool>(harness_->chat_b().RegisterEndpoint("a", harness_->ma_a)));

    a_chat_ = std::make_unique<AmpDirectChatService>(harness_->chat_a(), [this] { harness_->PumpBoth(); });
    b_chat_ = std::make_unique<AmpDirectChatService>(harness_->chat_b(), [this] { harness_->PumpBoth(); });
    a_chat_->Start();
    b_chat_->Start();
  }

  void TearDown() override {
    a_chat_->Stop();
    b_chat_->Stop();
    a_chat_.reset();
    b_chat_.reset();
    harness_.reset();
  }

  std::unique_ptr<pbr::test::AmpMeshHarness> harness_;
  std::unique_ptr<AmpDirectChatService> a_chat_;
  std::unique_ptr<AmpDirectChatService> b_chat_;
};

TEST_F(AmpDirectChatServiceTest, SendEnvelopeRoundTrip) {
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
