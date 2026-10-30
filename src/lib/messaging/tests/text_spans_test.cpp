// Mentions in group messages (WIRE_SCHEMAS § text_spans, group-chat G015): the wire form, its two
// levels of checks, and the local form a stored message keeps.
#include "lib/messaging/ChatPayloadCodec.h"
#include "lib/messaging/ChatPayloadValidator.h"
#include "lib/messaging/TextSpans.h"

#include "common/Serialize.hpp"

#include <gtest/gtest.h>

#include <sstream>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

TextSpan Mention(const std::string& account, uint32_t start, uint32_t end) {
  return TextSpan{.kind = static_cast<uint8_t>(TextSpanKind::Mention), .start = start, .end = end, .account_id = account};
}

TextSpan Everyone(uint32_t start, uint32_t end) {
  return TextSpan{.kind = static_cast<uint8_t>(TextSpanKind::Everyone), .start = start, .end = end};
}

/** The bytes a sender would put on the wire, spans as given (no checks: what a bad sender could send). */
std::vector<uint8_t> RawSpansPayload(const std::string& text, const std::vector<TextSpan>& spans) {
  std::ostringstream oss;
  OutputArchive ar(oss);
  ar & static_cast<uint8_t>(1);
  ar & static_cast<uint8_t>(6);
  WireLenUtf8 body_text{text};
  ar & body_text;
  ar & static_cast<uint8_t>(1);
  ar & static_cast<uint8_t>(spans.size());
  for (const TextSpan& span : spans) {
    ar & span.kind;
    ar & span.start;
    ar & span.end;
    WireLenBytes body{text_spans::EncodeBody(span)};
    ar & body;
  }
  const std::string bytes = oss.str();
  return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

std::vector<TextSpan> SpansOf(const ThreadMessage& message) { return text_spans::FromPayloadJson(message.payload_json); }

TEST(TextSpansTest, MentionsRoundTripAndPlainTextStaysTypeZero) {
  const std::string text = "Hi @Li Wei and @Ana, @everyone";
  const std::vector<TextSpan> spans{Mention("account:li", 3, 10), Mention("account:ana", 15, 19), Everyone(21, 30)};
  auto bytes = ChatPayloadCodec::EncodeText(text, spans);
  ASSERT_TRUE(bytes) << bytes.error().message;
  EXPECT_EQ((*bytes)[1], 6) << "content type text_spans";
  auto message = ChatPayloadValidator::DecodeValidated(*bytes);
  ASSERT_TRUE(message) << message.error().message;
  EXPECT_EQ(message->content_type, ChatContentType::Text) << "every reader of text keeps working";
  EXPECT_EQ(message->text, text);
  EXPECT_EQ(SpansOf(*message), spans);
  EXPECT_EQ(text_spans::MentionedAccounts(SpansOf(*message)),
            (std::vector<std::string>{"account:li", "account:ana"}));
  EXPECT_TRUE(text_spans::MentionsEveryone(SpansOf(*message)));

  // A stored message encodes back to the same bytes (history answer, resend).
  auto again = ChatPayloadCodec::EncodeToRow(*message);
  ASSERT_TRUE(again);
  EXPECT_EQ(*again, *bytes);

  auto plain = ChatPayloadCodec::EncodeText("no one mentioned", {});
  ASSERT_TRUE(plain);
  EXPECT_EQ((*plain)[1], 0) << "no span: plain text, unchanged";
  EXPECT_EQ(*plain, *ChatPayloadCodec::EncodeText("no one mentioned"));
}

TEST(TextSpansTest, AKindThisBuildDoesNotKnowIsKeptAndWrittenBack) {
  const std::string text = "see pp.example/doc";
  TextSpan link{.kind = 9, .start = 4, .end = 18, .body = {0x01, 0x02, 0x03}};
  const auto bytes = RawSpansPayload(text, {link});
  auto message = ChatPayloadCodec::DecodeToMessageFields(bytes);
  ASSERT_TRUE(message) << "an unknown kind is not an error";
  EXPECT_EQ(message->text, text);
  ASSERT_EQ(SpansOf(*message).size(), 1u);
  EXPECT_EQ(SpansOf(*message)[0].body, link.body);
  EXPECT_TRUE(text_spans::MentionedAccounts(SpansOf(*message)).empty()) << "not acted on";
  EXPECT_EQ(*ChatPayloadCodec::EncodeToRow(*message), bytes) << "passed on as received";
}

TEST(TextSpansTest, StructureErrorsRejectTheMessage) {
  const std::string text = "@a @b @everyone @everyone";
  EXPECT_FALSE(ChatPayloadCodec::DecodeToMessageFields(
      RawSpansPayload(text, {Mention("account:a", 0, 2), Mention("account:a", 3, 5)})))
      << "one account mentioned twice";
  EXPECT_FALSE(ChatPayloadCodec::DecodeToMessageFields(RawSpansPayload(text, {Everyone(6, 15), Everyone(16, 25)})))
      << "two everyone spans";
  std::vector<TextSpan> crowd;
  std::string long_text;
  for (int i = 0; i < 51; ++i) {
    const uint32_t start = static_cast<uint32_t>(long_text.size());
    long_text += "@m ";
    crowd.push_back(Mention("account:m" + std::to_string(i), start, start + 2));
  }
  EXPECT_FALSE(ChatPayloadCodec::DecodeToMessageFields(RawSpansPayload(long_text, crowd))) << "more than 50 mentions";
  EXPECT_FALSE(ChatPayloadCodec::EncodeText(long_text, crowd)) << "nor may we send it";

  auto trailing = RawSpansPayload(text, {Mention("account:a", 0, 2)});
  trailing.push_back(0x00);
  EXPECT_FALSE(ChatPayloadCodec::DecodeToMessageFields(trailing)) << "bytes after the last span";
  TextSpan everyone_with_body = Everyone(6, 15);
  everyone_with_body.kind = 9;  // write a body, then claim it is an everyone span
  everyone_with_body.body = {0x01};
  auto bodied = RawSpansPayload(text, {everyone_with_body});
  bodied[1 + 1 + 8 + text.size() + 1 + 1] = 2;  // the span's kind byte
  EXPECT_FALSE(ChatPayloadCodec::DecodeToMessageFields(bodied)) << "an everyone span with a body";
}

TEST(TextSpansTest, ABadRangeDropsOnlyItsSpan) {
  const std::string text = "Hi @Zoë, hi @Bo";  // "ë" is two bytes: @Zoë is bytes 3..8
  const std::vector<TextSpan> sent{
      Mention("account:zoe", 3, 8),     // fine
      Mention("account:bo", 13, 99),    // past the end
      Mention("account:x", 7, 9),       // ends inside "ë", and does not start with @
      Mention("account:y", 0, 2),       // "Hi": not an @
  };
  auto message = ChatPayloadCodec::DecodeToMessageFields(RawSpansPayload(text, sent));
  ASSERT_TRUE(message) << "the text still shows";
  EXPECT_EQ(message->text, text);
  EXPECT_EQ(text_spans::MentionedAccounts(SpansOf(*message)), std::vector<std::string>{"account:zoe"});

  // Overlapping spans: neither is kept.
  auto overlapping = ChatPayloadCodec::DecodeToMessageFields(
      RawSpansPayload("@Ann Lee", {Mention("account:ann", 0, 4), Mention("account:annlee", 0, 8)}));
  ASSERT_TRUE(overlapping);
  EXPECT_TRUE(SpansOf(*overlapping).empty());
}

} // namespace
} // namespace pbr
