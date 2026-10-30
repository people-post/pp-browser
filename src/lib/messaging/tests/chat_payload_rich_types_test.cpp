#include "lib/messaging/AtAiParser.h"
#include "lib/messaging/ChatPayloadCodec.h"
#include "lib/messaging/ChatPayloadValidator.h"
#include "lib/messaging/GroupMentions.h"
#include "common/Serialize.hpp"
#include "lib/messaging/ReactionTypes.h"
#include "foundation/crypto/AttachmentContentCipher.h"
#include "foundation/crypto/AttachmentContentHash.h"
#include "foundation/crypto/CryptoConstants.h"
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
  fields.display_name = "Alice";
  fields.card_json = R"({"account_id":"account:alice","signing_public_key_b64":"c2ln"})";
  fields.relay_user_id = "relay:alice";

  auto bytes = ChatPayloadCodec::EncodeContactCard(fields, "Alice");
  ASSERT_TRUE(static_cast<bool>(bytes));

  auto message = ChatPayloadValidator::DecodeValidated(*bytes);
  ASSERT_TRUE(static_cast<bool>(message));
  EXPECT_EQ(message->content_type, ChatContentType::ContactCard);
  EXPECT_EQ(message->text, "Alice");
  auto decoded = ChatPayloadCodec::DecodeContactCardJson(message->payload_json);
  ASSERT_TRUE(decoded) << decoded.error().message;
  EXPECT_EQ(decoded->display_name, "Alice");
  EXPECT_EQ(decoded->relay_user_id, "relay:alice");
  EXPECT_NE(decoded->card_json.find("account:alice"), std::string::npos);

  ChatContactCardFields no_card = fields;
  no_card.card_json.clear();
  auto without = ChatPayloadCodec::EncodeContactCard(no_card, "Alice");
  ASSERT_TRUE(without);
  EXPECT_FALSE(ChatPayloadValidator::DecodeValidated(*without)) << "a contact card carries the person's card";
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

TEST(ChatPayloadRichTypesTest, AttachmentRoundTrip) {
  using namespace pbr;

  ChatAttachmentFields fields;
  fields.url = "https://cdn.example/blobs/abc";
  fields.mime = "image/png";
  fields.filename = "diagram.png";
  fields.byte_length = 4096;
  fields.content_hash.assign(kAttachmentContentHashSize, 0x11);
  fields.content_key.assign(kSessionKeySize, 0x22);
  fields.blob_nonce.assign(kAeadNonceSize, 0x33);

  auto bytes = ChatPayloadCodec::EncodeAttachment(fields, "diagram.png");
  ASSERT_TRUE(static_cast<bool>(bytes));

  auto message = ChatPayloadValidator::DecodeValidated(*bytes);
  ASSERT_TRUE(static_cast<bool>(message));
  EXPECT_EQ(message->content_type, ChatContentType::Attachment);
  EXPECT_EQ(message->text, "diagram.png");
  auto decoded = ChatPayloadCodec::DecodeAttachmentJson(message->payload_json);
  ASSERT_TRUE(static_cast<bool>(decoded));
  EXPECT_EQ(decoded->url, fields.url);
  EXPECT_EQ(decoded->mime, fields.mime);
  EXPECT_EQ(decoded->filename, fields.filename);
  EXPECT_EQ(decoded->byte_length, fields.byte_length);
  EXPECT_EQ(decoded->content_hash, fields.content_hash);
  EXPECT_EQ(decoded->content_key, fields.content_key);
  EXPECT_EQ(decoded->blob_nonce, fields.blob_nonce);
}

TEST(ChatPayloadRichTypesTest, AttachmentContentCipherRoundTrip) {
  using namespace pbr;

  auto key = AttachmentContentCipher::GenerateContentKey();
  ASSERT_TRUE(static_cast<bool>(key));
  const ByteVector plaintext = {'h', 'e', 'l', 'l', 'o'};
  auto encrypted = AttachmentContentCipher::Encrypt(*key, plaintext);
  ASSERT_TRUE(static_cast<bool>(encrypted));
  auto hash = AttachmentContentHash(plaintext);
  ASSERT_TRUE(static_cast<bool>(hash));
  auto decrypted =
      AttachmentContentCipher::Decrypt(*key, encrypted->nonce, encrypted->ciphertext, hash.value());
  ASSERT_TRUE(static_cast<bool>(decrypted));
  EXPECT_EQ(*decrypted, plaintext);
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

// ---- text_mentions (content type 6) -------------------------------------------------------------------

namespace {

using namespace pbr;

struct RawMention {
  std::string id;
  uint32_t start;
  uint32_t end;
};

/** Hand-built type 6 bytes, so malformed layouts can be made. */
std::vector<uint8_t> RawTextMentions(const std::string& text, const std::vector<RawMention>& mentions, uint8_t flags,
                                     const std::optional<std::pair<uint32_t, uint32_t>>& all = std::nullopt,
                                     const size_t trailing_bytes = 0, const uint16_t count_override = 0xFFFF) {
  std::ostringstream oss;
  OutputArchive ar(oss);
  const uint8_t version = 1;
  const uint8_t type = 6;
  const uint8_t sub = 1;
  ar & version;
  ar & type;
  WireLenUtf8 body{text};
  ar & body;
  ar & sub;
  ar & static_cast<uint16_t>(count_override == 0xFFFF ? mentions.size() : count_override);
  for (const RawMention& m : mentions) {
    WireLenUtf8 id{m.id};
    ar & id;
    ar & m.start;
    ar & m.end;
  }
  ar & flags;
  if (all) {
    ar & all->first;
    ar & all->second;
  }
  for (size_t i = 0; i < trailing_bytes; ++i) {
    ar & static_cast<uint8_t>(0);
  }
  const std::string data = oss.str();
  return std::vector<uint8_t>(data.begin(), data.end());
}

ThreadMessage TextWithMentions(const std::string& text, const MentionsPayload& mentions) {
  ThreadMessage message;
  message.content_type = ChatContentType::Text;
  message.text = text;
  message.payload_json = BuildMentionsPayloadJson(text, mentions);
  return message;
}

MentionRange Range(const std::string& id, const uint32_t start, const uint32_t end) {
  MentionRange range;
  range.account_id = id;
  range.start = start;
  range.end = end;
  return range;
}

Roe<ThreadMessage> RoundTrip(const ThreadMessage& message) {
  auto bytes = ChatPayloadCodec::EncodeToRow(message);
  if (!bytes) {
    return bytes.error();
  }
  return ChatPayloadValidator::DecodeValidated(*bytes);
}

constexpr uint8_t kWireTypeOffset = 1;

} // namespace

TEST(TextMentionsCodecTest, RoundTripsZeroOneAndManyMentions) {
  for (const size_t count : {size_t{0}, size_t{1}, size_t{3}, size_t{50}}) {
    std::string text;
    MentionsPayload mentions;
    for (size_t i = 0; i < count; ++i) {
      const uint32_t start = static_cast<uint32_t>(text.size());
      text += "@u" + std::to_string(i);
      mentions.mentions.push_back(Range("acct-" + std::to_string(i), start, static_cast<uint32_t>(text.size())));
      text += " ";
    }
    text += "hello";
    auto bytes = ChatPayloadCodec::EncodeToRow(TextWithMentions(text, mentions));
    ASSERT_TRUE(static_cast<bool>(bytes)) << count;
    EXPECT_EQ((*bytes)[kWireTypeOffset], count == 0 ? 0 : 6) << "no mentions stays type 0";
    auto decoded = ChatPayloadValidator::DecodeValidated(*bytes);
    ASSERT_TRUE(static_cast<bool>(decoded)) << count;
    EXPECT_EQ(decoded->content_type, ChatContentType::Text);
    EXPECT_EQ(decoded->text, text);
    const MentionsPayload back = ParseMentionsPayload(decoded->payload_json);
    ASSERT_EQ(back.mentions.size(), count);
    for (size_t i = 0; i < count; ++i) {
      EXPECT_EQ(back.mentions[i].account_id, mentions.mentions[i].account_id);
      EXPECT_EQ(back.mentions[i].start, mentions.mentions[i].start);
      EXPECT_EQ(back.mentions[i].end, mentions.mentions[i].end);
    }
    EXPECT_FALSE(back.all.has_value());
  }
}

TEST(TextMentionsCodecTest, AllRangeRoundTripsAndIsOmittedWhenAbsent) {
  MentionsPayload mentions;
  mentions.all = Range("", 0, 9);
  mentions.mentions.push_back(Range("acct-b", 10, 13));
  auto decoded = RoundTrip(TextWithMentions("@Everyone @b hi", mentions));
  ASSERT_TRUE(static_cast<bool>(decoded));
  const MentionsPayload back = ParseMentionsPayload(decoded->payload_json);
  ASSERT_TRUE(back.all.has_value());
  EXPECT_EQ(back.all->start, 0u);
  EXPECT_EQ(back.all->end, 9u);
  EXPECT_EQ(back.mentions.size(), 1u);

  MentionsPayload only_all;
  only_all.all = Range("", 0, 9);
  auto only = RoundTrip(TextWithMentions("@Everyone hi", only_all));
  ASSERT_TRUE(static_cast<bool>(only));
  EXPECT_TRUE(ParseMentionsPayload(only->payload_json).all.has_value());

  auto plain = RoundTrip(TextWithMentions("@b hi", MentionsPayload{{Range("acct-b", 0, 2)}, std::nullopt}));
  ASSERT_TRUE(static_cast<bool>(plain));
  EXPECT_EQ(plain->payload_json.find("\"all\""), std::string::npos);
}

TEST(TextMentionsCodecTest, TypeZeroIsUnchanged) {
  ThreadMessage message;
  message.content_type = ChatContentType::Text;
  message.text = "hi @bob";
  message.payload_json = R"({"text":"hi @bob"})";
  auto bytes = ChatPayloadCodec::EncodeToRow(message);
  ASSERT_TRUE(static_cast<bool>(bytes));
  auto expected = ChatPayloadCodec::EncodeText("hi @bob");
  ASSERT_TRUE(static_cast<bool>(expected));
  EXPECT_EQ(*bytes, *expected);
  EXPECT_FALSE(ChatPayloadCodec::HasMentions(message));
}

TEST(TextMentionsCodecTest, StructureErrorsRejectTheWholeMessage) {
  const std::string text = "@bob hi";
  const std::vector<RawMention> one{{"acct-b", 0, 4}};
  ASSERT_TRUE(static_cast<bool>(ChatPayloadValidator::DecodeValidated(RawTextMentions(text, one, 0)))) << "baseline";

  std::vector<RawMention> many(51, RawMention{"acct-b", 0, 4});
  EXPECT_FALSE(static_cast<bool>(ChatPayloadValidator::DecodeValidated(RawTextMentions(text, many, 0)))) << "count 51";
  EXPECT_FALSE(static_cast<bool>(ChatPayloadValidator::DecodeValidated(RawTextMentions(text, one, 0, std::nullopt, 1))))
      << "trailing byte";
  EXPECT_FALSE(static_cast<bool>(ChatPayloadValidator::DecodeValidated(RawTextMentions(text, one, 0x02)))) << "flag bit";
  EXPECT_FALSE(static_cast<bool>(ChatPayloadValidator::DecodeValidated(RawTextMentions(text, one, 0x81, std::pair{0u, 4u}))))
      << "unknown bit next to all";
  EXPECT_FALSE(static_cast<bool>(
      ChatPayloadValidator::DecodeValidated(RawTextMentions(text, one, 0, std::nullopt, 0, 2)))) << "count beyond data";

  std::vector<uint8_t> truncated = RawTextMentions(text, one, 0);
  truncated.resize(truncated.size() - 9);  // cuts into the last range
  EXPECT_FALSE(static_cast<bool>(ChatPayloadValidator::DecodeValidated(truncated)));

  // A LenUtf8 whose length points past the end (the account id of the first mention).
  std::vector<uint8_t> bad_len = RawTextMentions(text, one, 0);
  const size_t id_len_at = 1 + 1 + 8 + text.size() + 1 + 2;  // version, type, len+text, sub, count
  bad_len[id_len_at + 7] = 0xFF;
  EXPECT_FALSE(static_cast<bool>(ChatPayloadValidator::DecodeValidated(bad_len)));

  EXPECT_FALSE(static_cast<bool>(ChatPayloadValidator::DecodeValidated(RawTextMentions(text, one, 1))))
      << "all flag without its range";
}

TEST(TextMentionsCodecTest, BadContentDropsOnlyThatMention) {
  const std::string text = "@bob \xE4\xBD\xA0\xE5\xA5\xBD \xF0\x9F\x98\x80 end";  // @bob, 你好, 😀
  // byte offsets: "@bob"=0..4, ' '=4, 你=5..8, 好=8..11, ' '=11, 😀=12..16
  const auto kept_ids = [&](const std::vector<RawMention>& mentions, const uint8_t flags = 0,
                            const std::optional<std::pair<uint32_t, uint32_t>> all = std::nullopt) {
    auto decoded = ChatPayloadValidator::DecodeValidated(RawTextMentions(text, mentions, flags, all));
    EXPECT_TRUE(static_cast<bool>(decoded));
    std::vector<std::string> ids;
    if (decoded) {
      EXPECT_EQ(decoded->text, text);
      EXPECT_EQ(decoded->content_type, ChatContentType::Text);
      for (const MentionRange& range : ParseMentionsPayload(decoded->payload_json).mentions) {
        ids.push_back(range.account_id);
      }
    }
    return ids;
  };
  const RawMention good{"good", 0, 4};
  EXPECT_EQ(kept_ids({good}), (std::vector<std::string>{"good"}));
  EXPECT_EQ(kept_ids({good, {"oob", 12, 99}}), (std::vector<std::string>{"good"})) << "past the end";
  EXPECT_EQ(kept_ids({good, {"empty", 6, 6}}), (std::vector<std::string>{"good"})) << "empty range";
  EXPECT_EQ(kept_ids({good, {"reversed", 4, 0}}), (std::vector<std::string>{"good"})) << "end before start";
  EXPECT_EQ(kept_ids({good, {"overlap", 2, 4}}), (std::vector<std::string>{"good"})) << "overlap";
  EXPECT_EQ(kept_ids({good, {"cjk", 6, 8}}), (std::vector<std::string>{"good"})) << "inside a Chinese character";
  EXPECT_EQ(kept_ids({good, {"emoji", 13, 16}}), (std::vector<std::string>{"good"})) << "inside an emoji";
  EXPECT_EQ(kept_ids({good, {"noat", 5, 8}}), (std::vector<std::string>{"good"})) << "slice does not start with @";
  EXPECT_EQ(kept_ids({good, {"", 0, 0}}), (std::vector<std::string>{"good"})) << "empty id";
  EXPECT_TRUE(kept_ids({{"oob", 12, 99}}).empty()) << "nothing left";
}

TEST(TextMentionsCodecTest, BadAllRangeDropsOnlyAll) {
  const std::string text = "@Everyone @b hi";
  const std::vector<RawMention> one{{"acct-b", 10, 12}};
  auto bad = ChatPayloadValidator::DecodeValidated(RawTextMentions(text, one, 1, std::pair{1u, 9u}));  // not on '@'
  ASSERT_TRUE(static_cast<bool>(bad));
  const MentionsPayload bad_back = ParseMentionsPayload(bad->payload_json);
  EXPECT_FALSE(bad_back.all.has_value());
  EXPECT_EQ(bad_back.mentions.size(), 1u);

  auto oob = ChatPayloadValidator::DecodeValidated(RawTextMentions(text, one, 1, std::pair{0u, 99u}));
  ASSERT_TRUE(static_cast<bool>(oob));
  EXPECT_FALSE(ParseMentionsPayload(oob->payload_json).all.has_value());

  auto clash = ChatPayloadValidator::DecodeValidated(RawTextMentions(text, one, 1, std::pair{0u, 12u}));  // overlaps @b
  ASSERT_TRUE(static_cast<bool>(clash));
  EXPECT_FALSE(ParseMentionsPayload(clash->payload_json).all.has_value());
  EXPECT_EQ(ParseMentionsPayload(clash->payload_json).mentions.size(), 1u);
}

TEST(TextMentionsCodecTest, EncodeDropsUnfitRangesAndFallsBackToTypeZero) {
  MentionsPayload bad;
  bad.mentions.push_back(Range("acct-b", 3, 99));
  auto bytes = ChatPayloadCodec::EncodeToRow(TextWithMentions("hi there", bad));
  ASSERT_TRUE(static_cast<bool>(bytes));
  EXPECT_EQ((*bytes)[kWireTypeOffset], 0);

  MentionsPayload too_many;
  for (int i = 0; i < 51; ++i) {
    too_many.mentions.push_back(Range("a", 0, 2));
  }
  EXPECT_FALSE(static_cast<bool>(ChatPayloadCodec::EncodeToRow(TextWithMentions("@a", too_many))));
}

TEST(TextMentionsCodecTest, TextLivesInTheTextField) {
  MentionsPayload mentions;
  mentions.mentions.push_back(Range("acct-b", 0, 2));
  auto decoded = RoundTrip(TextWithMentions("@b hello", mentions));
  ASSERT_TRUE(static_cast<bool>(decoded));
  auto json = TryParseObject(decoded->payload_json);
  ASSERT_TRUE(json.has_value());
  EXPECT_EQ(json->getString("text").value_or(""), "@b hello");
}
