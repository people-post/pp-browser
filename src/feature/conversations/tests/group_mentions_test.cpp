#include "feature/conversations/GroupMentions.h"

#include "lib/messaging/QuoteReply.h"

#include <gtest/gtest.h>

#include <map>

namespace pbr {
namespace {

// Group-chat G015: what the composer sends and what a transcript highlights.

constexpr const char* kAlice = "acct-alice";
constexpr const char* kBob = "acct-bob";
constexpr const char* kBob2 = "acct-bob-2";
constexpr const char* kZhang = "acct-zhang";
constexpr const char* kOwner = "acct-owner";
constexpr const char* kMe = "acct-me";

ComposerMention Mention(const std::string& account, const std::string& nickname) {
  return {TextSpanKind::Mention, account, "@" + nickname};
}

ComposerMention Everyone() {
  return {TextSpanKind::Everyone, {}, kEveryoneMentionToken};
}

TextSpan Span(TextSpanKind kind, size_t start, size_t end, const std::string& account = {}) {
  TextSpan span;
  span.kind = static_cast<uint8_t>(kind);
  span.start = static_cast<uint32_t>(start);
  span.end = static_cast<uint32_t>(end);
  span.account_id = account;
  return span;
}

std::string Covered(const std::string& text, const TextSpan& span) {
  return text.substr(span.start, span.end - span.start);
}

// --- the spans a draft is sent with -----------------------------------------------------------

TEST(GroupMentionsTest, SpansCoverExactlyEachInsertedToken) {
  const std::string text = "@Alice and @Bob lunch?";
  const auto spans = MentionSpansForDraft(text, {Mention(kAlice, "Alice"), Mention(kBob, "Bob")});
  ASSERT_EQ(spans.size(), 2u);
  EXPECT_EQ(Covered(text, spans[0]), "@Alice");
  EXPECT_EQ(spans[0].account_id, kAlice);
  EXPECT_TRUE(spans[0].Is(TextSpanKind::Mention));
  EXPECT_EQ(Covered(text, spans[1]), "@Bob");
  EXPECT_EQ(spans[1].account_id, kBob);
  EXPECT_TRUE(text_spans::CheckStructure(spans));
  EXPECT_EQ(text_spans::KeepValidRanges(text, spans).size(), 2u);
}

TEST(GroupMentionsTest, TokensMovedByEditsAroundThemAreFoundAgain) {
  // Inserted into "@Alice @Bob ", then words typed before, between and after.
  const std::string text = "hey @Alice, and you @Bob: see this";
  const auto spans = MentionSpansForDraft(text, {Mention(kAlice, "Alice"), Mention(kBob, "Bob")});
  ASSERT_EQ(spans.size(), 2u);
  EXPECT_EQ(spans[0].start, 4u);
  EXPECT_EQ(Covered(text, spans[0]), "@Alice");
  EXPECT_EQ(Covered(text, spans[1]), "@Bob");
}

TEST(GroupMentionsTest, ATokenEditedAwayIsDroppedTheRestStay) {
  const std::string text = "@Alce and @Bob";  // "@Alice" mistyped afterwards
  const auto spans = MentionSpansForDraft(text, {Mention(kAlice, "Alice"), Mention(kBob, "Bob")});
  ASSERT_EQ(spans.size(), 1u);
  EXPECT_EQ(spans[0].account_id, kBob);
  EXPECT_EQ(Covered(text, spans[0]), "@Bob");

  EXPECT_TRUE(MentionSpansForDraft("nobody here", {Mention(kAlice, "Alice")}).empty());
  // Run on into a longer name, or glued to a word before it: no longer the token.
  EXPECT_TRUE(MentionSpansForDraft("@Bobby hi", {Mention(kBob, "Bob")}).empty());
  EXPECT_TRUE(MentionSpansForDraft("mail x@Bob now", {Mention(kBob, "Bob")}).empty());
  // Punctuation after it is fine.
  EXPECT_EQ(MentionSpansForDraft("@Bob, hi", {Mention(kBob, "Bob")}).size(), 1u);
}

TEST(GroupMentionsTest, EachTokenIsSearchedAfterThePreviousInInsertionOrder) {
  // Two members with the same public nickname: the first inserted takes the first "@Bob".
  const std::string text = "@Bob and @Bob";
  const auto spans = MentionSpansForDraft(text, {Mention(kBob, "Bob"), Mention(kBob2, "Bob")});
  ASSERT_EQ(spans.size(), 2u);
  EXPECT_EQ(spans[0].account_id, kBob);
  EXPECT_EQ(spans[0].start, 0u);
  EXPECT_EQ(spans[1].account_id, kBob2);
  EXPECT_EQ(spans[1].start, 9u);
  EXPECT_TRUE(text_spans::CheckStructure(spans));

  // One of the two deleted: one span is left (insertion order decides which account it names).
  const auto one = MentionSpansForDraft("hi @Bob", {Mention(kBob, "Bob"), Mention(kBob2, "Bob")});
  ASSERT_EQ(one.size(), 1u);
  EXPECT_EQ(one[0].account_id, kBob);
}

TEST(GroupMentionsTest, AnAccountOrEveryoneTwiceIsMarkedOnce) {
  // The receiver rejects a message naming an account twice or holding two everyone spans (G015).
  const std::string text = "@Bob @Bob @everyone @everyone";
  const auto spans =
      MentionSpansForDraft(text, {Mention(kBob, "Bob"), Mention(kBob, "Bob"), Everyone(), Everyone()});
  ASSERT_EQ(spans.size(), 2u);
  EXPECT_EQ(spans[0].account_id, kBob);
  EXPECT_EQ(spans[0].start, 0u);
  EXPECT_TRUE(spans[1].Is(TextSpanKind::Everyone));
  EXPECT_EQ(Covered(text, spans[1]), "@everyone");
  EXPECT_TRUE(spans[1].account_id.empty());
  EXPECT_TRUE(text_spans::CheckStructure(spans));
}

TEST(GroupMentionsTest, MultibyteNicknamesAreUtf8ByteRanges) {
  const std::string name = "\xE5\xBC\xA0\xE4\xB8\x89";  // 张三
  const std::string text = "\xE4\xBD\xA0\xE5\xA5\xBD @" + name + " \xE5\x90\x83\xE9\xA5\xAD";  // 你好 @张三 吃饭
  const auto spans = MentionSpansForDraft(text, {Mention(kZhang, name)});
  ASSERT_EQ(spans.size(), 1u);
  EXPECT_EQ(spans[0].start, 7u);  // "你好 " is 7 bytes
  EXPECT_EQ(spans[0].end, 7u + 1 + name.size());
  EXPECT_EQ(Covered(text, spans[0]), "@" + name);
  EXPECT_EQ(text_spans::KeepValidRanges(text, spans).size(), 1u);
  // Chinese text right after the name, no space: still found (no word boundary in that script).
  EXPECT_EQ(MentionSpansForDraft("@" + name + "\xE4\xBD\xA0\xE5\xA5\xBD", {Mention(kZhang, name)}).size(), 1u);
}

TEST(GroupMentionsTest, ComposedWithAQuoteTheSpansStillHold) {
  // The spans are found in the typed reply; the quote goes under it, so their offsets are unchanged.
  const std::string reply = "@Bob look";
  const auto spans = MentionSpansForDraft(reply, {Mention(kBob, "Bob")});
  const std::string out = ComposeQuoteReply(reply, "an old @Alice message");
  ASSERT_EQ(spans.size(), 1u);
  EXPECT_EQ(Covered(out, spans[0]), "@Bob");
  EXPECT_EQ(MentionTextLimit(out), reply.size());
  EXPECT_EQ(MentionTextLimit(reply), reply.size());
}

// --- the picker ----------------------------------------------------------------------------------

std::vector<GroupRosterMember> Roster() {
  std::vector<GroupRosterMember> roster;
  for (const char* id : {kOwner, kMe, kZhang, kAlice, kBob}) {
    GroupRosterMember member;
    member.member_identity = id;
    member.role = std::string(id) == kOwner ? MemberRole::Owner : MemberRole::Member;
    roster.push_back(member);
  }
  return roster;
}

std::string NameOf(const std::string& account) {
  static const std::map<std::string, std::string> names = {
      {kOwner, "Olga"}, {kMe, "Me"}, {kZhang, "\xE5\xBC\xA0\xE4\xB8\x89"}, {kAlice, "alice"}, {kBob, "Bob"}};
  return names.at(account);
}

TEST(GroupMentionsTest, PickerListsOtherMembersByNameAndEveryoneForTheOwnerOnly) {
  const auto lines = MentionCandidates(Roster(), kMe, /*local_is_owner=*/false, NameOf, "");
  ASSERT_EQ(lines.size(), 4u);
  EXPECT_EQ(lines[0].name, "alice");  // ASCII case does not split the order
  EXPECT_EQ(lines[1].name, "Bob");
  EXPECT_EQ(lines[2].name, "Olga");
  EXPECT_EQ(lines[3].account_id, kZhang);
  for (const MentionCandidate& line : lines) {
    EXPECT_NE(line.account_id, kMe) << "never ourselves";
    EXPECT_TRUE(line.kind == TextSpanKind::Mention);
  }
  EXPECT_EQ(lines[1].Token(), "@Bob");

  const auto owner_lines = MentionCandidates(Roster(), kOwner, /*local_is_owner=*/true, NameOf, "");
  ASSERT_FALSE(owner_lines.empty());
  EXPECT_TRUE(owner_lines[0].kind == TextSpanKind::Everyone);
  EXPECT_EQ(owner_lines[0].Token(), "@everyone");
  EXPECT_EQ(owner_lines.size(), 5u);  // everyone + the four others
}

TEST(GroupMentionsTest, PickerFiltersByTheTypedQuery) {
  const auto bo = MentionCandidates(Roster(), kOwner, true, NameOf, "BO");
  ASSERT_EQ(bo.size(), 1u);
  EXPECT_EQ(bo[0].account_id, kBob);
  const auto ev = MentionCandidates(Roster(), kOwner, true, NameOf, "ev");
  ASSERT_EQ(ev.size(), 1u);
  EXPECT_TRUE(ev[0].kind == TextSpanKind::Everyone);
  EXPECT_TRUE(MentionCandidates(Roster(), kMe, false, NameOf, "ev").empty()) << "not the owner: no @everyone";
  const auto zh = MentionCandidates(Roster(), kMe, false, NameOf, "\xE5\xBC\xA0");
  ASSERT_EQ(zh.size(), 1u);
  EXPECT_EQ(zh[0].account_id, kZhang);
  EXPECT_TRUE(MentionCandidates(Roster(), kMe, false, NameOf, "ai").empty());
}

TEST(GroupMentionsTest, ThePublicNameNeverTheSendersPrivateRemark) {
  Contact contact;
  contact.local.display_name = "my secret remark";
  contact.remote.nickname = "Robert";
  EXPECT_EQ(MemberPublicName(kBob, "Bob", contact), "Bob") << "the account card's nickname first";
  EXPECT_EQ(MemberPublicName(kBob, "", contact), "Robert") << "then the directory's";
  Contact remark_only;
  remark_only.local.display_name = "my secret remark";
  EXPECT_EQ(MemberPublicName("acct-0123456789abcdef", "  ", remark_only), "acct-0123456…");
  EXPECT_EQ(MemberPublicName(kBob, "", std::nullopt), kBob);
}

TEST(GroupMentionsTest, TypingAtOpensThePickerAtTheStartOrAfterASpace) {
  EXPECT_EQ(TypedMentionTrigger("", "@"), std::optional<size_t>(0));
  EXPECT_EQ(TypedMentionTrigger("hi ", "hi @"), std::optional<size_t>(3));
  EXPECT_EQ(TypedMentionTrigger("hi  there", "hi @ there"), std::optional<size_t>(3));
  EXPECT_EQ(TypedMentionTrigger("a\n", "a\n@"), std::optional<size_t>(2));
  // After Chinese text (no spaces in it).
  EXPECT_EQ(TypedMentionTrigger("\xE5\xA5\xBD", "\xE5\xA5\xBD@"), std::optional<size_t>(3));
  EXPECT_FALSE(TypedMentionTrigger("mail", "mail@")) << "an e-mail address";
  EXPECT_FALSE(TypedMentionTrigger("", "x")) << "another character";
  EXPECT_FALSE(TypedMentionTrigger("", "@@")) << "a paste, not one key";
  EXPECT_FALSE(TypedMentionTrigger("@b", "@")) << "a deletion";
}

TEST(GroupMentionsTest, TheQueryFollowsTheAtUntilTheWordIsFinished) {
  EXPECT_EQ(MentionQueryAt("hi @", 3), std::optional<std::string>(""));
  EXPECT_EQ(MentionQueryAt("hi @bo", 3), std::optional<std::string>("bo"));
  EXPECT_EQ(MentionQueryAt("hi @ there", 3), std::optional<std::string>(""));
  EXPECT_FALSE(MentionQueryAt("hi @bo ", 3)) << "a space after the word: the user typed on";
  EXPECT_FALSE(MentionQueryAt("hi bo", 3)) << "the @ was deleted";
  EXPECT_FALSE(MentionQueryAt("hi", 3));
}

TEST(GroupMentionsTest, ChoosingReplacesTheQueryWithTheTokenAndASpace) {
  MentionInsert inserted = InsertMention("hi @bo", 3, "@Bob");
  EXPECT_EQ(inserted.text, "hi @Bob ");
  EXPECT_EQ(inserted.caret, inserted.text.size());
  inserted = InsertMention("@ there", 0, "@everyone");
  EXPECT_EQ(inserted.text, "@everyone there");
  EXPECT_EQ(inserted.caret, 10u);
  const std::string name = "\xE5\xBC\xA0\xE4\xB8\x89";
  inserted = InsertMention("@\xE5\xBC\xA0", 0, "@" + name);
  EXPECT_EQ(inserted.text, "@" + name + " ");
  EXPECT_EQ(inserted.caret, 1 + name.size() + 1);
}

// --- what a transcript highlights -----------------------------------------------------------

MentionRoster RosterNow() {
  MentionRoster roster;
  roster.members = {kOwner, kMe, kAlice, kBob};
  roster.owner = kOwner;
  roster.local_account = kMe;
  return roster;
}

TEST(GroupMentionsTest, AMentionOfACurrentMemberIsHighlightedANonMemberIsNot) {
  const std::string text = "@Alice @Zed hi";
  const std::vector<TextSpan> spans = {Span(TextSpanKind::Mention, 0, 6, kAlice),
                                       Span(TextSpanKind::Mention, 7, 11, "acct-zed")};
  const auto runs = EligibleMentionRuns(text, text.size(), spans, RosterNow(), kBob);
  ASSERT_EQ(runs.size(), 1u);
  EXPECT_EQ(runs[0].begin, 0u);
  EXPECT_EQ(runs[0].end, 6u);
  EXPECT_EQ(runs[0].account_id, kAlice);

  // A late roster that holds Zed restores the highlight (checked at display, not at decode).
  MentionRoster later = RosterNow();
  later.members.insert("acct-zed");
  EXPECT_EQ(EligibleMentionRuns(text, text.size(), spans, later, kBob).size(), 2u);
}

TEST(GroupMentionsTest, EveryoneIsHighlightedOnlyFromTheRostersOwner) {
  const std::string text = "@everyone meeting";
  const std::vector<TextSpan> spans = {Span(TextSpanKind::Everyone, 0, 9)};
  ASSERT_EQ(EligibleMentionRuns(text, text.size(), spans, RosterNow(), kOwner).size(), 1u);
  EXPECT_TRUE(EligibleMentionRuns(text, text.size(), spans, RosterNow(), kBob).empty()) << "not the owner";
  MentionRoster me_owner = RosterNow();
  me_owner.owner = kMe;
  EXPECT_EQ(EligibleMentionRuns(text, text.size(), spans, me_owner, kMe).size(), 1u) << "our own, as owner";
  MentionRoster unknown = RosterNow();
  unknown.owner.clear();
  EXPECT_TRUE(EligibleMentionRuns(text, text.size(), spans, unknown, kOwner).empty());
}

TEST(GroupMentionsTest, BadRangesAndTheQuotedPartAreNotHighlighted) {
  const std::string name = "\xE5\xBC\xA0\xE4\xB8\x89";
  const std::string text = "@" + name + " hi";
  MentionRoster roster = RosterNow();
  roster.members.insert(kZhang);
  // Ends inside a character, or past the text, or not on an '@'.
  EXPECT_TRUE(EligibleMentionRuns(text, text.size(), {Span(TextSpanKind::Mention, 0, 3, kZhang)}, roster, kBob).empty());
  EXPECT_TRUE(EligibleMentionRuns(text, text.size(), {Span(TextSpanKind::Mention, 0, 99, kZhang)}, roster, kBob).empty());
  EXPECT_TRUE(EligibleMentionRuns(text, text.size(), {Span(TextSpanKind::Mention, 1, 7, kZhang)}, roster, kBob).empty());
  const auto ok = EligibleMentionRuns(text, text.size(), {Span(TextSpanKind::Mention, 0, 7, kZhang)}, roster, kBob);
  ASSERT_EQ(ok.size(), 1u);
  EXPECT_EQ(text.substr(ok[0].begin, ok[0].end - ok[0].begin), "@" + name);

  // A quote reply: a span in the quote under it is not the reply's (a copy drops mentions).
  const std::string out = ComposeQuoteReply("ok", "@Alice said");
  const size_t in_quote = out.find("@Alice");
  EXPECT_TRUE(EligibleMentionRuns(out, MentionTextLimit(out),
                                  {Span(TextSpanKind::Mention, in_quote, in_quote + 6, kAlice)}, RosterNow(), kBob)
                  .empty());
}

TEST(GroupMentionsTest, RunsComeInTextOrderUnknownKindsAreNotHighlighted) {
  const std::string text = "@Bob @Alice x";
  TextSpan unknown = Span(TextSpanKind::Mention, 12, 13);
  unknown.kind = 9;
  const auto runs = EligibleMentionRuns(
      text, text.size(), {Span(TextSpanKind::Mention, 5, 11, kAlice), Span(TextSpanKind::Mention, 0, 4, kBob), unknown},
      RosterNow(), kOwner);
  ASSERT_EQ(runs.size(), 2u);
  EXPECT_EQ(runs[0].account_id, kBob);
  EXPECT_EQ(runs[1].account_id, kAlice);
}

} // namespace
} // namespace pbr
