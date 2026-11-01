#include "feature/conversations/GroupMentions.h"

#include "feature/conversations/ChatPresenter.h"
#include "lib/messaging/AtAiParser.h"
#include "lib/messaging/QuoteReply.h"

#include <gtest/gtest.h>

#include <functional>
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
  return {TextSpanKind::Everyone, {}, "@everyone"};
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

TEST(GroupMentionsTest, ADeletedPickDoesNotTakeThePlaceOfALaterOneWithTheSameName) {
  std::vector<ComposerMention> recorded = {Mention("acct-bob-aaa", "Bob")};
  // The user deletes the "@Bob" and picks another Bob: the draft is re-read on each change.
  PruneRecordedMentions("hi ", recorded);
  EXPECT_TRUE(recorded.empty());
  recorded.push_back(Mention("acct-bob-bbb", "Bob"));
  const std::string text = "hi @Bob ";
  PruneRecordedMentions(text, recorded);
  const auto spans = MentionSpansForDraft(text, recorded);
  ASSERT_EQ(spans.size(), 1u);
  EXPECT_EQ(spans[0].account_id, "acct-bob-bbb");

  // Two picks, one token left: the oldest goes. Two tokens: both stay.
  recorded = {Mention("acct-bob-aaa", "Bob"), Mention("acct-bob-bbb", "Bob")};
  PruneRecordedMentions("only @Bob here", recorded);
  ASSERT_EQ(recorded.size(), 1u);
  EXPECT_EQ(recorded[0].account_id, "acct-bob-bbb");
  recorded = {Mention("acct-bob-aaa", "Bob"), Mention("acct-bob-bbb", "Bob"), Mention(kAlice, "Alice")};
  PruneRecordedMentions("@Bob @Bob", recorded);
  ASSERT_EQ(recorded.size(), 2u) << "Alice is gone, the two Bobs stay";
  EXPECT_EQ(recorded[0].account_id, "acct-bob-aaa");
  EXPECT_EQ(recorded[1].account_id, "acct-bob-bbb");
}

TEST(GroupMentionsTest, PicksAreMatchedWhateverOrderTheyWereMadeIn) {
  // "@Bob hi" picked first, then "@Alice " inserted in front of it.
  const std::string text = "@Alice @Bob hi";
  const auto spans = MentionSpansForDraft(text, {Mention(kBob, "Bob"), Mention(kAlice, "Alice")});
  ASSERT_EQ(spans.size(), 2u);
  EXPECT_EQ(spans[0].account_id, kAlice);
  EXPECT_EQ(Covered(text, spans[0]), "@Alice");
  EXPECT_EQ(spans[1].account_id, kBob);
  EXPECT_EQ(Covered(text, spans[1]), "@Bob");
  EXPECT_TRUE(text_spans::CheckStructure(spans));
}

TEST(GroupMentionsTest, TwoSameNameMembersInTextOrderKeepTheirPickOrder) {
  const std::string text = "@Bob and @Bob";
  const auto spans = MentionSpansForDraft(text, {Mention("acct-bob-aaa", "Bob"), Mention("acct-bob-bbb", "Bob")});
  ASSERT_EQ(spans.size(), 2u);
  EXPECT_EQ(spans[0].account_id, "acct-bob-aaa");
  EXPECT_EQ(spans[0].start, 0u);
  EXPECT_EQ(spans[1].account_id, "acct-bob-bbb");
  EXPECT_EQ(spans[1].start, 9u);
  // The twins can still swap when the user moves a token past the other one: the first pick takes the first token.
  const auto moved = MentionSpansForDraft("@Bob and @Bob", {Mention("acct-bob-bbb", "Bob"), Mention("acct-bob-aaa", "Bob")});
  EXPECT_EQ(moved[0].account_id, "acct-bob-bbb");
}

TEST(GroupMentionsTest, AShorterNameThatStartsAnotherDoesNotTakeItsFront) {
  const std::string zhang = "\xE5\xBC\xA0";            // 张
  const std::string zhang_san = "\xE5\xBC\xA0\xE4\xB8\x89";  // 张三
  const std::string text = "@" + zhang_san + " @" + zhang;
  const auto spans = MentionSpansForDraft(text, {Mention("acct-short", zhang), Mention(kZhang, zhang_san)});
  ASSERT_EQ(spans.size(), 2u);
  EXPECT_EQ(spans[0].account_id, kZhang);
  EXPECT_EQ(Covered(text, spans[0]), "@" + zhang_san);
  EXPECT_EQ(spans[1].account_id, "acct-short");
  EXPECT_EQ(Covered(text, spans[1]), "@" + zhang);
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
  const auto lines = MentionCandidates(Roster(), kMe, /*local_is_owner=*/false, NameOf, "", kEveryoneMentionWord);
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

  const auto owner_lines = MentionCandidates(Roster(), kOwner, /*local_is_owner=*/true, NameOf, "", kEveryoneMentionWord);
  ASSERT_FALSE(owner_lines.empty());
  EXPECT_TRUE(owner_lines[0].kind == TextSpanKind::Everyone);
  EXPECT_EQ(owner_lines[0].Token(), "@everyone");
  EXPECT_EQ(owner_lines.size(), 5u);  // everyone + the four others
}

TEST(GroupMentionsTest, PickerFiltersByTheTypedQuery) {
  const auto bo = MentionCandidates(Roster(), kOwner, true, NameOf, "BO", kEveryoneMentionWord);
  ASSERT_EQ(bo.size(), 1u);
  EXPECT_EQ(bo[0].account_id, kBob);
  const auto ev = MentionCandidates(Roster(), kOwner, true, NameOf, "ev", kEveryoneMentionWord);
  ASSERT_EQ(ev.size(), 1u);
  EXPECT_TRUE(ev[0].kind == TextSpanKind::Everyone);
  EXPECT_TRUE(MentionCandidates(Roster(), kMe, false, NameOf, "ev", kEveryoneMentionWord).empty()) << "not the owner: no @everyone";
  const auto zh = MentionCandidates(Roster(), kMe, false, NameOf, "\xE5\xBC\xA0", kEveryoneMentionWord);
  ASSERT_EQ(zh.size(), 1u);
  EXPECT_EQ(zh[0].account_id, kZhang);
  EXPECT_TRUE(MentionCandidates(Roster(), kMe, false, NameOf, "ai", kEveryoneMentionWord).empty());
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

TEST(GroupMentionsTest, TheQueryRunsFromTheAtToTheCaret) {
  EXPECT_EQ(MentionQueryAt("hi @", 3, 4), std::optional<std::string>(""));
  EXPECT_EQ(MentionQueryAt("hi @bo", 3, 6), std::optional<std::string>("bo"));
  EXPECT_EQ(MentionQueryAt("hi @ there", 3, 4), std::optional<std::string>(""));
  EXPECT_EQ(MentionQueryAt("hi @bo there", 3, 6), std::optional<std::string>("bo")) << "words after the caret are not the query";
  EXPECT_FALSE(MentionQueryAt("hi @bo ", 3, 7)) << "a space after the word: the user typed on";
  EXPECT_FALSE(MentionQueryAt("hi bo", 3, 5)) << "the @ was deleted";
  EXPECT_FALSE(MentionQueryAt("hi", 3, 2));
  EXPECT_FALSE(MentionQueryAt("hi @bo", 3, 3)) << "the caret moved in front of the @";
  EXPECT_EQ(MentionQueryAt("hi @bo", 3, 99), std::optional<std::string>("bo")) << "a caret past the end is the end";
}

TEST(GroupMentionsTest, AnAtTypedInFrontOfAWordLeavesTheWordAlone) {
  // "hello" with "@" typed at the start: the caret is right after the @, so the query is empty ...
  EXPECT_EQ(MentionQueryAt("@hello", 0, 1), std::optional<std::string>(""));
  // ... and choosing replaces the @ only.
  MentionInsert inserted = InsertMention("@hello", 0, 1, "@Bob");
  EXPECT_EQ(inserted.text, "@Bob hello");
  EXPECT_EQ(inserted.caret, 5u);
  // In Chinese text (3 bytes a character), with the caret counted in bytes: "你好@世界" -> "你好@Bob 世界".
  const std::string hello = "\xE4\xBD\xA0\xE5\xA5\xBD";  // 你好
  const std::string world = "\xE4\xB8\x96\xE7\x95\x8C";  // 世界
  const std::string text = hello + "@" + world;
  EXPECT_EQ(MentionQueryAt(text, hello.size(), hello.size() + 1), std::optional<std::string>(""));
  inserted = InsertMention(text, hello.size(), hello.size() + 1, "@Bob");
  EXPECT_EQ(inserted.text, hello + "@Bob " + world);
  EXPECT_EQ(inserted.caret, hello.size() + 5);
  // Part of the word typed after the @: only that part is replaced.
  EXPECT_EQ(MentionQueryAt("@boring", 0, 3), std::optional<std::string>("bo"));
  EXPECT_EQ(InsertMention("@boring", 0, 3, "@Bob").text, "@Bob ring");
}

TEST(GroupMentionsTest, ChoosingReplacesTheQueryWithTheTokenAndASpace) {
  MentionInsert inserted = InsertMention("hi @bo", 3, 6, "@Bob");
  EXPECT_EQ(inserted.text, "hi @Bob ");
  EXPECT_EQ(inserted.caret, inserted.text.size());
  inserted = InsertMention("@ there", 0, 1, "@everyone");
  EXPECT_EQ(inserted.text, "@everyone there");
  EXPECT_EQ(inserted.caret, 10u);
  const std::string name = "\xE5\xBC\xA0\xE4\xB8\x89";
  inserted = InsertMention("@\xE5\xBC\xA0", 0, 4, "@" + name);
  EXPECT_EQ(inserted.text, "@" + name + " ");
  EXPECT_EQ(inserted.caret, 1 + name.size() + 1);
}

// --- members that share a name -------------------------------------------------------------------

TEST(GroupMentionsTest, MembersWithTheSameNameShowAShortIdTheOthersDoNot) {
  const auto twin_name = [](const std::string& account) { return account == kBob2 ? std::string("Bob") : NameOf(account); };
  auto roster = Roster();
  GroupRosterMember twin;
  twin.member_identity = kBob2;
  roster.push_back(twin);
  const auto lines = MentionCandidates(roster, kMe, false, twin_name, "", kEveryoneMentionWord);
  size_t bobs = 0;
  for (const MentionCandidate& line : lines) {
    if (line.name == "Bob") {
      ++bobs;
      EXPECT_FALSE(line.short_id.empty());
      EXPECT_EQ(line.Label(), "Bob \xC2\xB7 " + line.short_id);
      EXPECT_EQ(line.Token(), "@Bob") << "what is inserted stays the name";
    } else {
      EXPECT_TRUE(line.short_id.empty()) << line.name;
      EXPECT_EQ(line.Label(), line.name);
    }
  }
  EXPECT_EQ(bobs, 2u);
  // Narrowed by the query to one of them: still told apart (names collide in the group, not in the list).
  const auto bo = MentionCandidates(roster, kMe, false, twin_name, "bo", kEveryoneMentionWord);
  ASSERT_EQ(bo.size(), 2u);
  EXPECT_NE(bo[0].Label(), bo[1].Label());
  EXPECT_EQ(bo[0].account_id, kBob) << "same name: ordered by account id";
  // No twins: no short id at all.
  for (const MentionCandidate& line : MentionCandidates(Roster(), kMe, false, NameOf, "", kEveryoneMentionWord)) {
    EXPECT_TRUE(line.short_id.empty());
  }
}

TEST(GroupMentionsTest, NamesThatDifferOnlyInAsciiCaseCollide) {
  const auto names = [](const std::string& account) {
    return account == kBob ? std::string("Bob") : account == kAlice ? std::string("bob") : NameOf(account);
  };
  const auto lines = MentionCandidates(Roster(), kMe, false, names, "bob", kEveryoneMentionWord);
  ASSERT_EQ(lines.size(), 2u);
  for (const MentionCandidate& line : lines) {
    EXPECT_FALSE(line.short_id.empty()) << line.name;
    EXPECT_EQ(line.Token(), "@" + line.name) << "what is inserted stays the name, as typed";
  }
  EXPECT_NE(lines[0].Label(), lines[1].Label());
}

// --- a member named "ai" ------------------------------------------------------------------------

TEST(GroupMentionsTest, ATextThatStartsAtAiIsTheAgentsEvenAfterAPickedMemberNamedAi) {
  // A member called "ai" was picked once (the pick is still recorded); the user then typed "@ai question" by hand.
  const std::string text = "@ai question";
  const auto spans = MentionSpansForDraft(text, {Mention(kAlice, "ai")});
  ASSERT_EQ(spans.size(), 1u) << "the composer cannot tell the pick from the typing";
  EXPECT_TRUE(ParseAtAiPrefix(text).is_ai_invoke);
  EXPECT_FALSE(ChatPresenter::KeepsMentionSpans(text, spans)) << "the spans drop: the question goes to the AI only";
  EXPECT_FALSE(ChatPresenter::KeepsMentionSpans("  @ai+ question", spans));
  // Any other text keeps its spans, and no span means nothing to send.
  EXPECT_TRUE(ChatPresenter::KeepsMentionSpans("hi @ai", MentionSpansForDraft("hi @ai", {Mention(kAlice, "ai")})));
  EXPECT_FALSE(ChatPresenter::KeepsMentionSpans("hello", {}));
}

// --- the picker's lines are read ahead, once --------------------------------------------------------

struct PreloadFixture {
  int reads = 0;
  int loaded_notices = 0;
  uint64_t version = 1;
  int64_t now = 100000;
  std::vector<std::function<void()>> pending;  // loads waiting for their turn on the "worker"
  bool deferred = false;
  std::vector<GroupRosterMember> roster = Roster();
  MentionCandidatePreload preload{
      [this](std::function<MentionCandidatePreload::Lines()> read,
             std::function<void(MentionCandidatePreload::Lines)> done) {
        // The worker half runs `read` (which carries nothing of the fixture's picker); the UI half is `done`.
        auto run = [read = std::move(read), done = std::move(done)]() { done(read()); };
        if (deferred) {
          pending.push_back(std::move(run));
        } else {
          run();
        }
      },
      [this]() -> MentionCandidatePreload::Reader {
        return [this](const std::string&) {
          ++reads;  // the roster, the cards, the contacts: all behind this call
          return AllMentionCandidates(
              roster, kOwner, true,
              [](const std::string& account) { return account == "acct-dave" ? std::string("Dave") : NameOf(account); },
              "everyone");
        };
      },
      [this]() { return version; }, [this]() { return now; }};
  PreloadFixture() {
    preload.SetOnLoaded([this]() { ++loaded_notices; });
  }
  void RunFirst() {
    ASSERT_FALSE(pending.empty());
    auto job = std::move(pending.front());
    pending.erase(pending.begin());
    job();
  }
  void RunPending() {
    auto jobs = std::move(pending);
    pending.clear();
    for (auto& job : jobs) {
      job();
    }
  }
};

TEST(GroupMentionsTest, TheGroupsLinesAreReadWhenItsChatOpensAndTypingOnlyFilters) {
  PreloadFixture f;
  f.preload.Open("thread-hike");
  ASSERT_EQ(f.reads, 1);
  ASSERT_TRUE(f.preload.Ready());
  // `@` and the keystrokes after it: no read at all.
  f.preload.Typed();
  EXPECT_EQ(f.preload.Filter("").size(), 5u);
  EXPECT_EQ(f.preload.Filter("b").size(), 1u);
  EXPECT_EQ(f.preload.Filter("bo").size(), 1u);
  EXPECT_EQ(f.preload.Filter("ev").size(), 1u);
  f.preload.Open("thread-hike");  // the chat refreshing itself is not a new open
  f.preload.Changed();            // nor is a refresh with no membership change
  EXPECT_EQ(f.reads, 1);
}

TEST(GroupMentionsTest, AMembershipChangeReloadsOnceNotEveryRefresh) {
  PreloadFixture f;
  f.preload.Open("thread-hike");
  for (int refresh = 0; refresh < 10; ++refresh) {
    f.now += 1000;
    f.preload.Changed();  // ten refreshes, nothing changed in the group
  }
  EXPECT_EQ(f.reads, 1);
  GroupRosterMember dave;
  dave.member_identity = "acct-dave";
  f.roster.push_back(dave);
  ++f.version;  // a membership event was applied
  f.preload.Changed();
  f.preload.Changed();
  EXPECT_EQ(f.reads, 2) << "one reload for the change";
  EXPECT_EQ(f.preload.Filter("dave").size(), 1u);
}

TEST(GroupMentionsTest, AChangeDuringALoadIsReadAfterIt) {
  PreloadFixture f;
  f.deferred = true;
  f.preload.Open("thread-hike");
  ++f.version;
  f.preload.Changed();  // arrives while the first read is out
  f.RunPending();       // first read lands; a second starts at once
  EXPECT_EQ(f.reads, 1);
  f.RunPending();
  EXPECT_EQ(f.reads, 2);
  EXPECT_TRUE(f.preload.Ready());
}

TEST(GroupMentionsTest, OldLinesAreRefreshedOnTheNextAtAndTheOpenPickerIsTold) {
  PreloadFixture f;
  f.preload.Open("thread-hike");
  const int told = f.loaded_notices;
  f.now += MentionCandidatePreload::kStaleMs - 1;
  f.preload.Typed();
  EXPECT_EQ(f.reads, 1) << "not yet old";
  f.now += 2;
  f.deferred = true;
  f.preload.Typed();
  EXPECT_EQ(f.preload.Filter("").size(), 5u) << "answers from what it holds meanwhile";
  EXPECT_EQ(f.reads, 1);
  f.RunPending();
  EXPECT_EQ(f.reads, 2);
  EXPECT_EQ(f.loaded_notices, told + 1) << "the picker open on the old lines refreshes";
  f.preload.Typed();
  EXPECT_EQ(f.reads, 2) << "and is fresh again";
}

TEST(GroupMentionsTest, AnotherChatDropsTheLinesAndAReopenReadsAgainAndTellsThePicker) {
  PreloadFixture f;
  f.deferred = true;
  f.preload.Open("thread-hike");
  f.preload.Typed();  // `@` typed while the first read is out: nothing to show yet
  EXPECT_FALSE(f.preload.Ready());
  f.preload.Drop();   // the user went to another chat and came back
  f.preload.Open("thread-hike");
  f.RunFirst();       // the first read lands after the drop: ignored; the second is the live one
  EXPECT_FALSE(f.preload.Ready());
  EXPECT_EQ(f.loaded_notices, 0);
  f.RunPending();
  EXPECT_TRUE(f.preload.Ready());
  EXPECT_EQ(f.loaded_notices, 1) << "the picker waiting for its lines is told once, for the live load";
  EXPECT_EQ(f.preload.Filter("").size(), 5u);
  f.preload.Drop();
  EXPECT_TRUE(f.preload.Filter("").empty());
}

TEST(GroupMentionsTest, AFirstAtBeforeTheLoadIsDoneWaitsForItWithoutBlocking) {
  PreloadFixture f;
  f.deferred = true;
  f.preload.Open("thread-hike");
  EXPECT_FALSE(f.preload.Ready()) << "the read has not run: the UI thread is not waiting for it";
  f.preload.Typed();  // does not start a second load
  EXPECT_EQ(f.pending.size(), 1u);
  f.RunPending();
  EXPECT_TRUE(f.preload.Ready());
  EXPECT_EQ(f.loaded_notices, 1);
  EXPECT_EQ(f.reads, 1);
}

TEST(GroupMentionsTest, APickOfSomeoneWhoLeftIsDroppedAtSend) {
  MentionRoster roster;
  roster.members = {kMe, kAlice};
  roster.owner = kOwner;
  roster.local_account = kMe;
  const std::vector<TextSpan> spans = {Span(TextSpanKind::Mention, 0, 6, kAlice), Span(TextSpanKind::Mention, 7, 11, kBob),
                                       Span(TextSpanKind::Everyone, 12, 21)};
  const auto kept = KeepMentionsOnRoster(spans, roster);
  ASSERT_EQ(kept.size(), 1u) << "Bob left; we are not the owner, so no everyone";
  EXPECT_EQ(kept[0].account_id, kAlice);
  roster.owner = kMe;
  EXPECT_EQ(KeepMentionsOnRoster(spans, roster).size(), 2u) << "the owner keeps its everyone";
}

// --- the everyone line in the user's language --------------------------------------------------

TEST(GroupMentionsTest, EveryoneInChineseIsFoundByAPrefixOfItsWordOrByEveryone) {
  const std::string word = "\xE6\x89\x80\xE6\x9C\x89\xE4\xBA\xBA";  // 所有人
  for (const char* query : {"", "\xE6\x89\x80", "\xE6\x89\x80\xE6\x9C\x89", "\xE6\x9C\x89\xE4\xBA\xBA", "every"}) {
    const auto lines = MentionCandidates(Roster(), kOwner, true, NameOf, query, word);
    ASSERT_FALSE(lines.empty()) << query;
    EXPECT_TRUE(lines[0].kind == TextSpanKind::Everyone) << query;
    EXPECT_EQ(lines[0].Token(), "@" + word) << "what is inserted is the Chinese word";
  }
  // Not the owner, or a query that matches neither word: no line.
  EXPECT_TRUE(MentionCandidates(Roster(), kMe, false, NameOf, "\xE6\x89\x80", word).empty());
  const auto other = MentionCandidates(Roster(), kOwner, true, NameOf, "\xE5\xBC\xA0", word);
  ASSERT_EQ(other.size(), 1u);
  EXPECT_EQ(other[0].account_id, kZhang);
}

TEST(GroupMentionsTest, EveryoneInEnglishStaysEveryone) {
  for (const char* query : {"", "every", "EVERY", "one"}) {
    const auto lines = MentionCandidates(Roster(), kOwner, true, NameOf, query, kEveryoneMentionWord);
    ASSERT_FALSE(lines.empty()) << query;
    EXPECT_EQ(lines[0].Token(), "@everyone");
  }
  EXPECT_TRUE(MentionCandidates(Roster(), kOwner, true, NameOf, "\xE6\x89\x80", kEveryoneMentionWord).empty()) << "no Chinese word in English";
}

TEST(GroupMentionsTest, TheInsertedChineseEveryoneBecomesTheEveryoneSpan) {
  const std::string word = "\xE6\x89\x80\xE6\x9C\x89\xE4\xBA\xBA";  // 所有人
  const auto lines = MentionCandidates(Roster(), kOwner, true, NameOf, "", word);
  ASSERT_FALSE(lines.empty());
  const ComposerMention recorded{lines[0].kind, lines[0].account_id, lines[0].Token()};
  // What the picker leaves in the draft: "@所有人 " then the typed words; also directly followed by Chinese text.
  for (const std::string& text : {"@" + word + " \xE5\x90\x83\xE9\xA5\xAD", "\xE5\xA4\xA7\xE5\xAE\xB6 @" + word + "\xE5\x90\x83\xE9\xA5\xAD"}) {
    const auto spans = MentionSpansForDraft(text, {recorded});
    ASSERT_EQ(spans.size(), 1u) << text;
    EXPECT_TRUE(spans[0].Is(TextSpanKind::Everyone));
    EXPECT_EQ(Covered(text, spans[0]), "@" + word);
    EXPECT_TRUE(spans[0].account_id.empty()) << "the Everyone span has an empty body";
    EXPECT_TRUE(text_spans::CheckStructure(spans));
  }
  // The English line in an English UI still works the same way.
  EXPECT_EQ(MentionSpansForDraft("@everyone hi", {Everyone()}).size(), 1u);
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
