#include "base/messaging/ChatPayloadCodec.h"
#include "base/messaging/ChatPayloadValidator.h"
#include "base/messaging/MessagingJson.h"
#include "base/messaging/MessagingLimits.h"
#include "base/messaging/RelayWirePayload.h"
#include "base/messaging/SqliteThreadStore.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {
using namespace pbr;

ByteVector TestDek() {
  ByteVector dek(32);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0xa0 + i);
  }
  return dek;
}
} // namespace

TEST(ChatPayloadValidatorTest, RejectsUnknownContentType) {
  using namespace pbr;

  auto bytes = ChatPayloadCodec::EncodeText("nope");
  ASSERT_TRUE(static_cast<bool>(bytes));
  ASSERT_GT(bytes->size(), 1u);
  (*bytes)[1] = 99;
  EXPECT_FALSE(static_cast<bool>(ChatPayloadValidator::DecodeValidated(*bytes)));
}

TEST(ChatPayloadValidatorTest, RejectsOversizeOutboundText) {
  using namespace pbr;

  std::string oversized;
  oversized.assign(kMaxComposeTextBytes + 1, 'x');
  EXPECT_FALSE(static_cast<bool>(ChatPayloadValidator::ValidateOutboundText(oversized)));
}

TEST(ChatPayloadValidatorTest, RejectsOversizeMultibyteOutboundText) {
  using namespace pbr;

  const std::string one_cjk = "\xE4\xBD\xA0";
  std::string oversized;
  while (oversized.size() <= kMaxComposeTextBytes) {
    oversized += one_cjk;
  }
  EXPECT_FALSE(static_cast<bool>(ChatPayloadValidator::ValidateOutboundText(oversized)));
}

TEST(ChatPayloadValidatorTest, SanitizesInboundPresentationFields) {
  using namespace pbr;

  auto bytes = ChatPayloadCodec::EncodeText("hello");
  ASSERT_TRUE(static_cast<bool>(bytes));
  auto message = ChatPayloadValidator::DecodeValidated(*bytes);
  ASSERT_TRUE(static_cast<bool>(message));
  message->content_rml = "<div>evil</div>";
  message->chat_actions.push_back(TranscriptChatAction{.label = "x", .message = "y"});
  ChatPayloadValidator::SanitizeInboundFields(*message);
  EXPECT_FALSE(message->content_rml.has_value());
  EXPECT_TRUE(message->chat_actions.empty());
}

TEST(ChatPayloadValidatorTest, RelayWireRejectsLegacyBodyShapes) {
  using namespace pbr;

  const nlohmann::json public_relay = {
      {"envelope_version", 1},
      {"message_id", "m1"},
      {"sender_relay_id", "relay:a"},
      {"sender_contact_id", "relay:a"},
      {"route", {{"kind", "direct"}, {"channel", "public_relay"}}},
      {"body", {{"e2e", {{"payload_b64", "aGk="}}}}},
      {"timestamp", 1}};
  EXPECT_FALSE(static_cast<bool>(ParseRelayEnvelope(public_relay)));

  const nlohmann::json content_b64 = {
      {"envelope_version", 1},
      {"message_id", "m2"},
      {"sender_relay_id", "relay:a"},
      {"sender_contact_id", "relay:a"},
      {"route", {{"kind", "direct"}, {"channel", "e2e"}}},
      {"body", {{"content_b64", "aGk="}}},
      {"timestamp", 1}};
  EXPECT_FALSE(static_cast<bool>(ParseRelayEnvelope(content_b64)));

  const nlohmann::json remote_rml = {
      {"envelope_version", 1},
      {"message_id", "m3"},
      {"sender_relay_id", "relay:a"},
      {"sender_contact_id", "relay:a"},
      {"route", {{"kind", "direct"}, {"channel", "e2e"}}},
      {"body", {{"e2e", {{"payload_b64", "aGk="}}}, {"content_rml", "<div>x</div>"}}},
      {"timestamp", 1}};
  EXPECT_FALSE(static_cast<bool>(ParseRelayEnvelope(remote_rml)));
}

TEST(ChatPayloadValidatorTest, TransportColumnRoundTrip) {
  using namespace pbr;

  const std::filesystem::path data_dir =
      std::filesystem::temp_directory_path() / "pp_browser_transport_test";
  std::filesystem::remove_all(data_dir);
  SqliteThreadStore store(data_dir.string());
  ASSERT_TRUE(store.SetDek(TestDek()));

  Thread thread;
  thread.id = "thread-transport";
  thread.kind = ThreadKind::Direct;
  thread.channel = ThreadChannel::E2e;
  thread.title = "Peer";
  thread.updated_at = 1;
  ASSERT_TRUE(static_cast<bool>(store.UpsertThread(thread)));

  ThreadMessage message;
  message.id = "msg-transport";
  message.thread_id = thread.id;
  message.sender_contact_id = kLocalSelfContactId;
  message.text = "relay path";
  message.timestamp = 1;
  message.delivery = MessageDelivery::Relayed;
  message.transport = MessageTransport::Relay;
  ASSERT_TRUE(static_cast<bool>(store.AppendMessage(message)));

  auto loaded = store.GetMessages(thread.id);
  ASSERT_TRUE(static_cast<bool>(loaded));
  ASSERT_EQ(loaded->size(), 1u);
  ASSERT_TRUE(loaded->front().transport.has_value());
  EXPECT_EQ(*loaded->front().transport, MessageTransport::Relay);
}
