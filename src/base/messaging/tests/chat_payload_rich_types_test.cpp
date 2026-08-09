#include "base/messaging/AtAiParser.h"
#include "base/messaging/ChatPayloadCodec.h"
#include "base/messaging/ChatPayloadValidator.h"
#include "base/messaging/ReactionTypes.h"
#include "common/EmojiKey.h"

#include <gtest/gtest.h>

TEST(ChatPayloadRichTypesTest, AnnotationRoundTrip) {
  using namespace pbr;

  ChatAnnotationFields fields;
  fields.text = "👍";
  fields.annotation_type = "reaction";
  fields.target_message_id = "msg-target-1";
  fields.value = "👍";

  auto bytes = ChatPayloadCodec::EncodeAnnotation(fields);
  ASSERT_TRUE(static_cast<bool>(bytes));

  auto message = ChatPayloadValidator::DecodeValidated(*bytes);
  ASSERT_TRUE(static_cast<bool>(message));
  EXPECT_EQ(message->content_type, ChatContentType::Annotation);
  EXPECT_EQ(message->text, "👍");
  ASSERT_TRUE(message->target_message_id.has_value());
  EXPECT_EQ(*message->target_message_id, "msg-target-1");
}

TEST(ChatPayloadRichTypesTest, ReactionClearRoundTrip) {
  using namespace pbr;

  ChatAnnotationFields fields;
  fields.text = "";
  fields.annotation_type = kAnnotationTypeReactionClear;
  fields.target_message_id = "msg-target-2";
  fields.value = "❤️";

  auto bytes = ChatPayloadCodec::EncodeAnnotation(fields);
  ASSERT_TRUE(static_cast<bool>(bytes));

  auto message = ChatPayloadValidator::DecodeValidated(*bytes);
  ASSERT_TRUE(static_cast<bool>(message));
  EXPECT_EQ(message->content_type, ChatContentType::Annotation);
  ASSERT_TRUE(message->target_message_id.has_value());
  EXPECT_EQ(*message->target_message_id, "msg-target-2");
  auto decoded = ChatPayloadCodec::DecodeAnnotationJson(message->payload_json);
  ASSERT_TRUE(static_cast<bool>(decoded));
  EXPECT_EQ(decoded->annotation_type, kAnnotationTypeReactionClear);
  EXPECT_EQ(decoded->value, "❤️");
}

TEST(EmojiKeyTest, StripsTrailingVariationSelector) {
  using namespace pbr;
  // thumbs-up + VS16
  const std::string with_vs = std::string("👍") + "\xEF\xB8\x8F";
  EXPECT_EQ(NormalizeEmojiKey(with_vs), NormalizeEmojiKey("👍"));
  EXPECT_EQ(NormalizeEmojiKey("  😂  "), "😂");
}

TEST(ChatPayloadRichTypesTest, ContactCardRoundTrip) {
  using namespace pbr;

  ChatContactCardFields fields;
  fields.contact_id = "contact:abc";
  fields.display_name = "Alice";
  fields.relay_user_id = "relay:alice";

  auto bytes = ChatPayloadCodec::EncodeContactCard(fields, "Alice");
  ASSERT_TRUE(static_cast<bool>(bytes));

  auto message = ChatPayloadValidator::DecodeValidated(*bytes);
  ASSERT_TRUE(static_cast<bool>(message));
  EXPECT_EQ(message->content_type, ChatContentType::ContactCard);
  EXPECT_EQ(message->text, "Alice");
}

TEST(ChatPayloadRichTypesTest, CryptoTxRoundTrip) {
  using namespace pbr;

  ChatCryptoTxFields fields;
  fields.chain_id = "eip155:1";
  fields.asset = "ETH";
  fields.amount = "0.5";
  fields.direction = "send";
  fields.status = "confirmed";

  auto bytes = ChatPayloadCodec::EncodeCryptoTx(fields, "Sent 0.5 ETH");
  ASSERT_TRUE(static_cast<bool>(bytes));

  auto message = ChatPayloadValidator::DecodeValidated(*bytes);
  ASSERT_TRUE(static_cast<bool>(message));
  EXPECT_EQ(message->content_type, ChatContentType::CryptoTx);
  EXPECT_EQ(message->text, "Sent 0.5 ETH");
}

TEST(AtAiParserTest, DetectsSharedModes) {
  using namespace pbr;

  const auto shared_reply = ParseAtAiPrefix("@ai+ summarize this thread");
  EXPECT_TRUE(shared_reply.is_ai_invoke);
  EXPECT_EQ(shared_reply.mode, AtAiMode::SharedReply);

  const auto shared_full = ParseAtAiPrefix("@ai++ draft a reply");
  EXPECT_TRUE(shared_full.is_ai_invoke);
  EXPECT_EQ(shared_full.mode, AtAiMode::SharedFull);

  const auto local = ParseAtAiPrefix("@ai help me");
  EXPECT_TRUE(local.is_ai_invoke);
  EXPECT_EQ(local.mode, AtAiMode::Local);
}
