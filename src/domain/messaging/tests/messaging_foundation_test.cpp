#include "domain/messaging/AtAiParser.h"
#include "domain/messaging/PlainTextLinks.h"
#include "domain/messaging/QuoteReply.h"
#include "common/chat/MessagingJson.h"
#include "common/chat/PeopleDiscoveryBlocks.h"
#include "domain/messaging/RelayWirePayload.h"
#include "domain/people/Ed25519Signer.h"

#include <gtest/gtest.h>
#include "common/ValueJson.h"
#include "common/PbrCompat.h"

TEST(MessagingFoundationTest, CoreMessagingUtilitiesRoundTrip) {
  using namespace pbr;

  const AtAiParseResult parsed = ParseAtAiPrefix("  @ai summarize this thread  ");
  EXPECT_TRUE(parsed.is_ai_invoke);
  EXPECT_EQ(parsed.prompt, "summarize this thread");

  Thread thread;
  thread.id = "t1";
  thread.kind = ThreadKind::Direct;
  thread.channel = ThreadChannel::E2e;
  thread.title = "Alice";
  thread.participant_contact_ids = {"c1"};
  thread.encrypted = true;
  const Object thread_json = ThreadToJson(thread);
  const Thread restored = ThreadFromJson(thread_json);
  EXPECT_EQ(restored.id, "t1");
  EXPECT_EQ(restored.kind, ThreadKind::Direct);
  EXPECT_EQ(restored.channel, ThreadChannel::E2e);

  auto payload_b64 = RelayWirePayload::EncodePlaintextText("hi");
  ASSERT_TRUE(static_cast<bool>(payload_b64));

  RelayEnvelope envelope;
  envelope.envelope_version = kRelayEnvelopeVersion;
  envelope.message_id = "m1";
  envelope.sender_relay_id = "relay:x";
  envelope.sender_contact_id = "relay:x";
  envelope.route.kind = "direct";
  envelope.route.channel = ThreadChannel::E2e;
  envelope.body.e2e.payload_b64 = *payload_b64;
  envelope.timestamp = 1;
  const auto roundtrip = ParseRelayEnvelope(RelayEnvelopeToJson(envelope));
  ASSERT_TRUE(static_cast<bool>(roundtrip));
  EXPECT_EQ(roundtrip.value().message_id, "m1");

  const Object legacy = TryParseObject(R"({"thread_id":"t1","message_id":"m2","body":{"text":"nope"}})").value_or(Object{});
  EXPECT_FALSE(static_cast<bool>(ParseRelayEnvelope(legacy)));

  auto keys = Ed25519Signer::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(keys));
  auto signature = Ed25519Signer::Sign("test payload", keys->private_key);
  ASSERT_TRUE(static_cast<bool>(signature));
  auto verified = Ed25519Signer::Verify("test payload", *signature, keys->public_key);
  ASSERT_TRUE(static_cast<bool>(verified));
  EXPECT_TRUE(*verified);

  DirectoryHit hit;
  hit.hit_id = "hit_alice";
  hit.display_name = "Alice Example";
  hit.nickname = "alice";
  hit.ids = {{ContactIdKind::RelayUser, "relay:alice123", true}};
  const std::string blocks = BuildPeopleDiscoveryBlocksJson({hit}, std::vector<PeopleDiscoveryContactView>{});
  EXPECT_NE(blocks.find("long_list"), std::string::npos);
  EXPECT_NE(blocks.find("Alice Example"), std::string::npos);
  EXPECT_NE(blocks.find("~alice"), std::string::npos);
  EXPECT_NE(blocks.find("add_contact"), std::string::npos);
  EXPECT_NE(blocks.find("start_conversation"), std::string::npos);
}

// "Ask AI" on a message puts the question and the quoted message on separate lines.
TEST(AtAiParserTest, PromptMaySpanLines) {
  using namespace pbr;

  const AtAiParseResult parsed = ParseAtAiPrefix("@ai is this true?\n\n> line one\n> line two");
  EXPECT_TRUE(parsed.is_ai_invoke);
  EXPECT_EQ(parsed.mode, AtAiMode::Local);
  EXPECT_EQ(parsed.prompt, "is this true?\n\n> line one\n> line two");

  const AtAiParseResult quote_only = ParseAtAiPrefix("@ai \n\n> line one");
  EXPECT_TRUE(quote_only.is_ai_invoke);
  EXPECT_EQ(quote_only.prompt, "> line one");

  EXPECT_FALSE(ParseAtAiPrefix("@ai").is_ai_invoke);
}

// A quoting reply is plain text that splits back into the reply and the quote.
TEST(QuoteReplyTest, ComposeAndSplitRoundTrip) {
  using namespace pbr;

  const std::string composed = ComposeQuoteReply("ok", "line one\nline two");
  EXPECT_EQ(composed, "ok\n\n> line one\n> line two");
  const auto parts = SplitQuoteReply(composed);
  ASSERT_TRUE(parts.has_value());
  EXPECT_EQ(parts->reply, "ok");
  EXPECT_EQ(parts->quote, "line one\nline two");

  // A long message is quoted by its beginning, counted in characters, not bytes.
  const std::string cut = ComposeQuoteReply("ok", "\xE4\xBD\xA0\xE5\xA5\xBD\xE5\x90\x97", 2);
  EXPECT_EQ(cut, "ok\n\n> \xE4\xBD\xA0\xE5\xA5\xBD\xE2\x80\xA6");

  EXPECT_FALSE(SplitQuoteReply("just text").has_value());
  EXPECT_FALSE(SplitQuoteReply("\n\n> quote without a reply").has_value());
  EXPECT_FALSE(SplitQuoteReply("ok\n\n> quote\nnot a quote line").has_value());
}

// https links in a plain message become tappable; a shared article shows its label instead of the URL.
TEST(PlainTextLinksTest, FindsHttpsLinksAndViewDetailsLabels) {
  using namespace pbr;

  const std::string shared = "G7 to release oil reserves.\n\xE3\x80\x90\xE6\x9F\xA5\xE7\x9C\x8B\xE8\xAF\xA6\xE6\x83\x85\xE3\x80\x91https://www.aa.com.tr/en/x?id=1";
  auto links = FindPlainTextLinks(shared);
  ASSERT_EQ(links.size(), 1u);
  EXPECT_EQ(links[0].url, "https://www.aa.com.tr/en/x?id=1");
  EXPECT_EQ(links[0].display, std::string(kViewDetailsLabels[0]));
  EXPECT_EQ(shared.substr(0, links[0].begin), "G7 to release oil reserves.\n"); // the label is part of the span
  EXPECT_EQ(links[0].end, shared.size());

  links = FindPlainTextLinks("see https://example.com/a, then [View details]https://example.org/b.");
  ASSERT_EQ(links.size(), 2u);
  EXPECT_EQ(links[0].url, "https://example.com/a"); // trailing comma is not part of the link
  EXPECT_EQ(links[0].display, links[0].url);
  EXPECT_EQ(links[1].url, "https://example.org/b");
  EXPECT_EQ(links[1].display, "[View details]");

  // A link ends at CJK text; http, bare "https://" and "xhttps://" are not links.
  links = FindPlainTextLinks("\xE7\x9C\x8Bhttps://example.com/a\xE8\xBF\x99\xE9\x87\x8C http://plain.example https:// xhttps://no.example");
  ASSERT_EQ(links.size(), 1u);
  EXPECT_EQ(links[0].url, "https://example.com/a");
  EXPECT_TRUE(FindPlainTextLinks("no links here").empty());
}
