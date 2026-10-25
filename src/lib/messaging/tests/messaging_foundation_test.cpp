#include "lib/messaging/AtAiParser.h"
#include "lib/messaging/PeerKemKeyStore.h"
#include "lib/messaging/PlainTextLinks.h"
#include "lib/messaging/QuoteReply.h"
#include "common/chat/MessagingJson.h"
#include "common/directory/DirectoryJson.h"
#include "common/chat/PeopleDiscoveryBlocks.h"
#include "lib/messaging/RelayWirePayload.h"

#include <functional>
#include "lib/messaging/tests/TestAccount.h"

#include <gtest/gtest.h>
#include "common/ValueJson.h"
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

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

  mesh::DirectoryHit hit;
  hit.hit_id = "hit_alice";
  hit.display_name = "Alice Example";
  hit.nickname = "alice";
  hit.ids = {{mesh::ContactIdKind::RelayUser, "relay:alice123", true}};
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

namespace pbr {

// The wire parser keeps one body shape per route and no unknown route kinds.
TEST(MessagingFoundationTest, RelayEnvelopeParserRefusesMixedShapes) {
  RelayEnvelope direct;
  direct.message_id = "m1";
  direct.sender_relay_id = "relay:a";
  direct.sender_contact_id = "account:a";
  direct.route.kind = kRelayRouteDirect;
  direct.route.channel = ThreadChannel::E2ePublic;
  direct.body.e2e.payload_b64 = "AQID";
  direct.timestamp = 1;
  ASSERT_TRUE(static_cast<bool>(ParseRelayEnvelope(RelayEnvelopeToJson(direct))));

  const auto with = [&](const std::function<void(Object&)>& edit) {
    Object json = RelayEnvelopeToJson(direct);
    edit(json);
    return static_cast<bool>(ParseRelayEnvelope(json));
  };
  EXPECT_FALSE(with([](Object& json) {
    Object body = *json.getObject("body");
    Object e2e = *body.getObject("e2e");
    Object members;
    members.set("x", "AQID");
    e2e.set("member_payloads", members);
    body.set("e2e", e2e);
    json.set("body", body);
  })) << "direct with member_payloads";
  EXPECT_FALSE(with([](Object& json) {
    Object route = *json.getObject("route");
    route.set("kind", "other");
    json.set("route", route);
  })) << "unknown route kind";
  EXPECT_FALSE(with([](Object& json) {
    Object route;
    route.set("kind", "group");
    route.set("group_id", "group:a");
    json.set("route", route);
  })) << "group with payload_b64";
}

// Numbers are range-checked before narrowing, and ids carry no control characters.
TEST(MessagingFoundationTest, RelayEnvelopeParserRejectsOutOfRangeFields) {
  RelayEnvelope direct;
  direct.message_id = "m1";
  direct.sender_relay_id = "relay:a";
  direct.sender_contact_id = "account:a";
  direct.route.kind = kRelayRouteDirect;
  direct.route.channel = ThreadChannel::E2ePublic;
  direct.body.e2e.payload_b64 = "AQID";
  direct.timestamp = 1;
  const auto with = [&](const std::function<void(Object&)>& edit) {
    Object json = RelayEnvelopeToJson(direct);
    edit(json);
    return static_cast<bool>(ParseRelayEnvelope(json));
  };
  constexpr int64_t kTwo32 = int64_t{1} << 32;
  EXPECT_TRUE(with([](Object&) {}));
  EXPECT_FALSE(with([](Object& json) { json.set("session_epoch", kTwo32 + 1); })) << "epoch past u32";
  EXPECT_FALSE(with([](Object& json) { json.set("envelope_version", kTwo32 + kRelayEnvelopeVersion); }))
      << "version that only matches once narrowed";
  EXPECT_FALSE(with([](Object& json) { json.set("sender_contact_id", std::string("account:a\nx")); }));
  EXPECT_FALSE(with([](Object& json) { json.set("sender_contact_id", std::string("account:a\0x", 11)); }));
  EXPECT_FALSE(with([](Object& json) { json.set("recipient_contact_id", std::string("account:b\tx")); }));
  EXPECT_TRUE(with([](Object& json) { json.set("sender_relay_id", std::string()); }))
      << "no Brief route yet is allowed";
  EXPECT_FALSE(with([](Object& json) { json.set("key_epoch", kTwo32); })) << "key_epoch past u32";
  EXPECT_FALSE(with([](Object& json) { json.set("key_epoch", int64_t{0}); })) << "no epoch 0";
}

// G011: a group envelope's pairwise key epoch travels next to the group's numbering epoch.
TEST(MessagingFoundationTest, AGroupEnvelopeKeepsItsKeyEpoch) {
  RelayEnvelope group;
  group.message_id = "m1";
  group.sender_relay_id = "relay:a";
  group.sender_contact_id = "account:a";
  group.route.kind = kRelayRouteGroup;
  group.route.group_id = "group:a";
  group.body.e2e.member_payloads = std::map<std::string, std::string>{{"account:b", "AQID"}};
  group.session_epoch = 3;
  group.key_epoch = 2;
  group.timestamp = 1;
  auto parsed = ParseRelayEnvelope(RelayEnvelopeToJson(group));
  ASSERT_TRUE(parsed) << parsed.error().message;
  EXPECT_EQ(parsed->session_epoch, 3u);
  EXPECT_EQ(parsed->key_epoch, std::optional<uint32_t>(2));
}

// The store keeps an account's KEM key only under that account's own binding.
TEST(MessagingFoundationTest, PeerKemKeyStoreKeepsOnlyKeysTheAccountBound) {
  const test::TestAccount alice = test::TestAccount::Make();
  const test::TestAccount mallory = test::TestAccount::Make();
  const std::string account = ContactIdKindToString(mesh::ContactIdKind::Account);
  const PeerKemKeyRecord bound{.kem_public_key_b64 = alice.KemPublicB64(),
                               .signing_public_key_b64 = alice.SigningPublicB64(),
                               .binding_signature_b64 = alice.KemBindingB64(),
                               .source = "test"};
  PeerKemKeyStore store;

  PeerKemKeyRecord unbound = bound;
  unbound.binding_signature_b64.clear();
  EXPECT_FALSE(store.Put(account, alice.account_id, unbound));
  PeerKemKeyRecord substituted = bound;
  substituted.kem_public_key_b64 = mallory.KemPublicB64();
  EXPECT_FALSE(store.Put(account, alice.account_id, substituted)) << "another key under Alice's binding";
  const PeerKemKeyRecord mallory_own{.kem_public_key_b64 = mallory.KemPublicB64(),
                                  .signing_public_key_b64 = mallory.SigningPublicB64(),
                                  .binding_signature_b64 = mallory.KemBindingB64(),
                                  .source = "test"};
  EXPECT_FALSE(store.Put(account, alice.account_id, mallory_own)) << "Mallory's own binding, claimed for Alice";
  EXPECT_FALSE(store.Put("relay_user", alice.account_id, bound)) << "accounts only";
  EXPECT_FALSE(store.Get(account, alice.account_id).has_value());

  EXPECT_TRUE(store.Put(account, alice.account_id, bound));
  ASSERT_TRUE(store.Get(account, alice.account_id).has_value());
  EXPECT_EQ(store.Get(account, alice.account_id)->kem_public_key_b64, alice.KemPublicB64());
}

} // namespace pbr
