#include "domain/messaging/ChatPayloadCodec.h"
#include "domain/messaging/ChatPayloadValidator.h"
#include "common/chat/MessagingJson.h"
#include "common/chat/MessagingLimits.h"
#include "domain/messaging/RelayWirePayload.h"
#include "domain/messaging/SqliteThreadStore.h"
#include "common/ValueJson.h"

#include <filesystem>
#include <gtest/gtest.h>
#include "common/PbrCompat.h"

namespace {
using namespace pbr;

Object MakeObject(std::initializer_list<std::pair<const char*, Value>> fields) {
  Object object;
  for (const auto& [key, value] : fields) {
    object.set(key, value);
  }
  return object;
}

ByteVector TestDek() {
  ByteVector dek(32);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0xa0 + i);
  }
  return dek;
}
} // namespace

TEST(ChatPayloadValidatorTest, AcceptsUnknownContentTypeAsUnsupported) {
  using namespace pbr;

  auto bytes = ChatPayloadCodec::EncodeText("future");
  ASSERT_TRUE(static_cast<bool>(bytes));
  ASSERT_GT(bytes->size(), 1u);
  (*bytes)[1] = 99;
  auto message = ChatPayloadValidator::DecodeValidated(*bytes);
  ASSERT_TRUE(static_cast<bool>(message));
  EXPECT_EQ(message->content_type, ChatContentType::Unsupported);
  EXPECT_EQ(message->text, "future");
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

  Object public_route = MakeObject({{"kind", Value("direct")}, {"channel", Value("public_relay")}});
  Object public_body;
  {
    Object e2e;
    e2e.set("payload_b64", "aGk=");
    public_body.set("e2e", e2e);
  }
  const Object public_relay = MakeObject({{"envelope_version", Value(int64_t{1})},
                                          {"message_id", Value("m1")},
                                          {"sender_relay_id", Value("relay:a")},
                                          {"sender_contact_id", Value("relay:a")},
                                          {"route", ObjectValue(std::move(public_route))},
                                          {"body", ObjectValue(std::move(public_body))},
                                          {"timestamp", Value(int64_t{1})}});
  EXPECT_FALSE(static_cast<bool>(ParseRelayEnvelope(public_relay)));

  Object content_route = MakeObject({{"kind", Value("direct")}, {"channel", Value("e2e")}});
  Object content_body = MakeObject({{"content_b64", Value("aGk=")}});
  const Object content_b64 = MakeObject({{"envelope_version", Value(int64_t{1})},
                                         {"message_id", Value("m2")},
                                         {"sender_relay_id", Value("relay:a")},
                                         {"sender_contact_id", Value("relay:a")},
                                         {"route", ObjectValue(std::move(content_route))},
                                         {"body", ObjectValue(std::move(content_body))},
                                         {"timestamp", Value(int64_t{1})}});
  EXPECT_FALSE(static_cast<bool>(ParseRelayEnvelope(content_b64)));

  Object remote_route = MakeObject({{"kind", Value("direct")}, {"channel", Value("e2e")}});
  Object remote_body;
  {
    Object e2e;
    e2e.set("payload_b64", "aGk=");
    remote_body.set("e2e", e2e);
    remote_body.set("content_rml", "<div>x</div>");
  }
  const Object remote_rml = MakeObject({{"envelope_version", Value(int64_t{1})},
                                        {"message_id", Value("m3")},
                                        {"sender_relay_id", Value("relay:a")},
                                        {"sender_contact_id", Value("relay:a")},
                                        {"route", ObjectValue(std::move(remote_route))},
                                        {"body", ObjectValue(std::move(remote_body))},
                                        {"timestamp", Value(int64_t{1})}});
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
