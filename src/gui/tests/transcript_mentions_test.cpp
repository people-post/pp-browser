#include "gui/chat/TranscriptRml.h"

#include "lib/messaging/QuoteReply.h"
#include "lib/messaging/TextSpans.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// Group-chat G015: a group message's mentions drawn highlighted (eligible ones only), a direct chat's as plain text.

constexpr const char* kAlice = "acct-alice";
constexpr const char* kBob = "acct-bob";
constexpr const char* kOwner = "acct-owner";
constexpr const char* kMe = "acct-me";

TextSpan Span(TextSpanKind kind, size_t start, size_t end, const std::string& account = {}) {
  TextSpan span;
  span.kind = static_cast<uint8_t>(kind);
  span.start = static_cast<uint32_t>(start);
  span.end = static_cast<uint32_t>(end);
  span.account_id = account;
  return span;
}

TranscriptRow Row(const std::string& id, const std::string& sender, const std::string& text,
                  const std::vector<TextSpan>& spans) {
  TranscriptRow row;
  row.message.id = id;
  row.message.thread_id = "group-thread";
  row.message.sender_contact_id = sender;
  row.message.text = text;
  row.message.timestamp = 1;
  row.message.payload_json = text_spans::ToPayloadJson(text, spans);
  return row;
}

MentionRoster Roster() {
  MentionRoster roster;
  roster.members = {kOwner, kMe, kAlice, kBob};
  roster.owner = kOwner;
  roster.local_account = kMe;
  return roster;
}

std::string Rml(const TranscriptRow& row, const std::optional<MentionRoster>& roster) {
  const auto rows = RenderTranscript({row}, TranscriptFacts{}, roster);
  return rows.empty() ? std::string() : std::string(rows[0].content_rml.c_str());
}

TEST(TranscriptMentionsTest, AGroupMentionIsHighlightedAndTappable) {
  const std::string rml = Rml(Row("m1", kBob, "hi @Alice!", {Span(TextSpanKind::Mention, 3, 9, kAlice)}), Roster());
  EXPECT_NE(rml.find("hi <span class=\"chat-mention\" data-event-click=\"open_mention('m1', 0)\">@Alice</span>!"),
            std::string::npos)
      << rml;
  EXPECT_NE(rml.find("selectable=\"text\""), std::string::npos) << "the text stays selectable";
}

TEST(TranscriptMentionsTest, ADirectChatShowsSpansAsPlainText) {
  const std::string rml = Rml(Row("m1", kBob, "hi @Alice!", {Span(TextSpanKind::Mention, 3, 9, kAlice)}), std::nullopt);
  EXPECT_EQ(rml.find("chat-mention"), std::string::npos);
  EXPECT_NE(rml.find("hi @Alice!"), std::string::npos);
}

TEST(TranscriptMentionsTest, OnlyEligibleSpansAreHighlighted) {
  // A non-member's mention, and @everyone from someone who is not the owner: plain.
  const std::string text = "@Zed @everyone";
  const auto row = Row("m1", kBob, text,
                       {Span(TextSpanKind::Mention, 0, 4, "acct-zed"), Span(TextSpanKind::Everyone, 5, 14)});
  EXPECT_EQ(Rml(row, Roster()).find("chat-mention"), std::string::npos);
  // The owner's @everyone is, without a tap target; ours too once we are the owner.
  const auto from_owner = Row("m2", kOwner, "@everyone go", {Span(TextSpanKind::Everyone, 0, 9)});
  const std::string owner_rml = Rml(from_owner, Roster());
  EXPECT_NE(owner_rml.find("<span class=\"chat-mention chat-mention--everyone\">@everyone</span> go"), std::string::npos)
      << owner_rml;
  MentionRoster me_owner = Roster();
  me_owner.owner = kMe;
  const auto ours = Row("m3", kLocalSelfContactId, "@everyone go", {Span(TextSpanKind::Everyone, 0, 9)});
  EXPECT_NE(Rml(ours, me_owner).find("chat-mention--everyone"), std::string::npos);
  EXPECT_EQ(Rml(ours, Roster()).find("chat-mention"), std::string::npos);
}

TEST(TranscriptMentionsTest, Utf8TextIsSplitByBytesAndEscaped) {
  const std::string name = "\xE5\xBC\xA0<\xE4\xB8\x89>";  // 张<三> (a name with markup characters)
  const std::string text = "\xE4\xBD\xA0\xE5\xA5\xBD @" + name + " & bye";  // 你好 @张<三> & bye
  MentionRoster roster = Roster();
  roster.members.insert("acct-zhang");
  const std::string rml =
      Rml(Row("m1", kBob, text, {Span(TextSpanKind::Mention, 7, 7 + 1 + name.size(), "acct-zhang")}), roster);
  const std::string expected = "\xE4\xBD\xA0\xE5\xA5\xBD <span class=\"chat-mention\" data-event-click=\"open_mention('m1', "
                               "0)\">@\xE5\xBC\xA0&lt;\xE4\xB8\x89&gt;</span> &amp; bye";
  EXPECT_NE(rml.find(expected), std::string::npos) << rml;
}

TEST(TranscriptMentionsTest, MentionsAndLinksTogether) {
  const std::string text = "@Bob see https://example.com/a";
  const std::string rml = Rml(Row("m1", kAlice, text, {Span(TextSpanKind::Mention, 0, 4, kBob)}), Roster());
  EXPECT_NE(rml.find("<span class=\"chat-mention\" data-event-click=\"open_mention('m1', 0)\">@Bob</span> see "),
            std::string::npos)
      << rml;
  EXPECT_NE(rml.find("open_message_link('m1', 0)"), std::string::npos) << "the link keeps its index";

  // A span inside a link: the link wins, the mention is not drawn.
  const std::string inside = "https://example.com/@Bob";
  const size_t at = inside.find('@');
  const std::string link_rml = Rml(Row("m2", kAlice, inside, {Span(TextSpanKind::Mention, at, at + 4, kBob)}), Roster());
  EXPECT_EQ(link_rml.find("chat-mention"), std::string::npos);
  EXPECT_NE(link_rml.find("open_message_link('m2', 0)"), std::string::npos);
}

TEST(TranscriptMentionsTest, AQuoteReplyHighlightsItsOwnWordsNotTheQuote) {
  const std::string out = ComposeQuoteReply("@Bob ok", "@Alice said so");
  const size_t quoted_at = out.find("@Alice");
  const auto row = Row("m1", kOwner, out,
                       {Span(TextSpanKind::Mention, 0, 4, kBob), Span(TextSpanKind::Mention, quoted_at, quoted_at + 6, kAlice)});
  const std::string rml = Rml(row, Roster());
  EXPECT_NE(rml.find("chat-quote"), std::string::npos);
  EXPECT_NE(rml.find(">@Bob</span> ok"), std::string::npos) << rml;
  EXPECT_EQ(rml.find(">@Alice</span>"), std::string::npos);
}

} // namespace
} // namespace pbr
