#include "lib/ui/MentionDraft.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

std::vector<MentionCandidate> Members() {
  return {{"acc-bob", "Bob"}, {"acc-li", "Li Wei"}, {"acc-zh", "小明"}, {"acc-emo", "😀Jo"}, {"acc-ai", "ai"}};
}

MentionDraft Group(const bool owner = false, std::vector<MentionCandidate> members = Members()) {
  MentionDraft draft;
  draft.Enable([members] { return members; }, owner, "Everyone");
  return draft;
}

/** Types `typed` into `text` at the end, one textinput at a time, like the box does. */
std::string Type(MentionDraft& d, std::string text, const std::string& typed) {
  for (const char c : typed) {
    text += c;
    d.OnDraftChanged(text, ByteToCursor(text, text.size()), /*committed_input=*/true);
  }
  return text;
}

TEST(MentionDraftTest, AtAtStartOrAfterSpaceOpens) {
  MentionDraft d = Group();
  Type(d, "", "@");
  EXPECT_TRUE(d.PopupOpen());
  EXPECT_EQ(d.Rows().size(), 5u);

  MentionDraft e = Group();
  Type(e, "hi ", "@");
  EXPECT_TRUE(e.PopupOpen());

  MentionDraft f = Group();
  Type(f, "hi\n", "@");
  EXPECT_TRUE(f.PopupOpen());
}

TEST(MentionDraftTest, EmailDoesNotTrigger) {
  MentionDraft d = Group();
  Type(d, "", "me@x");
  EXPECT_FALSE(d.PopupOpen());
  Type(d, "mail me@", "b");
  EXPECT_FALSE(d.PopupOpen());
}

TEST(MentionDraftTest, NotInAGroupNothingOpens) {
  MentionDraft d;
  Type(d, "", "@");
  EXPECT_FALSE(d.PopupOpen());
}

TEST(MentionDraftTest, CompositionNeverTriggersOrMovesRanges) {
  MentionDraft d = Group();
  d.OnDraftChanged("@", 1, true, /*composing=*/true);
  EXPECT_FALSE(d.PopupOpen());
  // The same value arriving without a textinput (no composition state, value changed only) does not open either.
  d.OnDraftChanged("@", 1, /*committed_input=*/false);
  EXPECT_FALSE(d.PopupOpen());
  // Committed by a textinput afterwards: opens.
  d.OnDraftChanged("@b", 2, true);
  EXPECT_TRUE(d.PopupOpen());
}

TEST(MentionDraftTest, CompositionKeepsExistingRecordRanges) {
  MentionDraft d = Group();
  std::string t = Type(d, "", "@Bo");
  const auto picked = d.Pick(0);
  ASSERT_TRUE(picked);
  t = picked->text; // "@Bob "
  // A composition in the middle of typing "ni" in front: ignored entirely, ranges stay as they were.
  d.OnDraftChanged("ni" + t, 2, false, /*composing=*/true);
  ASSERT_EQ(d.Mentions().size(), 1u);
  EXPECT_EQ(d.Mentions()[0].start, 0u);
  // Committed afterwards: the edit is seen once, the range shifts.
  d.OnDraftChanged("ni" + t, 2, true);
  EXPECT_EQ(d.Mentions()[0].start, 2u);
}

TEST(MentionDraftTest, QueryFiltersAndIsCaseInsensitive) {
  MentionDraft d = Group();
  Type(d, "", "@bo");
  ASSERT_EQ(d.Rows().size(), 1u);
  EXPECT_EQ(d.Rows()[0].label, "Bob");
  Type(d, "", "@小");
  ASSERT_EQ(d.Rows().size(), 1u);
  EXPECT_EQ(d.Rows()[0].label, "小明");
}

TEST(MentionDraftTest, RowsSortedByName) {
  MentionDraft d = Group();
  Type(d, "", "@");
  ASSERT_EQ(d.Rows().size(), 5u);
  EXPECT_EQ(d.Rows()[0].label, "ai");
  EXPECT_EQ(d.Rows()[1].label, "Bob");
}

TEST(MentionDraftTest, DuplicateNamesGetAShortId) {
  MentionDraft d = Group(false, {{"relay:AAAAAAAAAAAAAAAA", "Sam"}, {"relay:BBBBBBBBBBBBBBBB", "Sam"}, {"acc-x", "Tom"}});
  Type(d, "", "@");
  ASSERT_EQ(d.Rows().size(), 3u);
  EXPECT_NE(d.Rows()[0].label, d.Rows()[1].label);
  EXPECT_NE(d.Rows()[0].label.find("Sam · "), std::string::npos);
  EXPECT_EQ(d.Rows()[2].label, "Tom");
  // The text that goes in the message is the nickname alone.
  const auto edit = d.Pick(0);
  ASSERT_TRUE(edit);
  EXPECT_EQ(edit->text, "@Sam ");
}

TEST(MentionDraftTest, EmptyResultShowsEmptyState) {
  MentionDraft d = Group();
  Type(d, "", "@zzz");
  EXPECT_TRUE(d.PopupOpen());
  EXPECT_TRUE(d.PopupEmpty());
  EXPECT_FALSE(d.Pick(0));
}

TEST(MentionDraftTest, EveryoneRowOnlyForTheOwner) {
  MentionDraft member = Group(false);
  Type(member, "", "@");
  for (const MentionRow& row : member.Rows()) {
    EXPECT_FALSE(row.everyone);
  }
  MentionDraft owner = Group(true);
  Type(owner, "", "@");
  ASSERT_EQ(owner.Rows().size(), 6u);
  EXPECT_TRUE(owner.Rows()[0].everyone);
  EXPECT_EQ(owner.Rows()[0].label, "Everyone");
}

TEST(MentionDraftTest, PickInsertsNameAndRecordsRange) {
  MentionDraft d = Group();
  std::string t = Type(d, "", "hi @bo");
  ASSERT_TRUE(d.PopupOpen());
  const auto edit = d.Pick(0);
  ASSERT_TRUE(edit);
  EXPECT_EQ(edit->text, "hi @Bob ");
  EXPECT_EQ(edit->cursor_chars, 8);
  EXPECT_FALSE(d.PopupOpen());
  ASSERT_EQ(d.Mentions().size(), 1u);
  EXPECT_EQ(d.Mentions()[0].account_id, "acc-bob");
  EXPECT_EQ(d.Mentions()[0].start, 3u);
  EXPECT_EQ(d.Mentions()[0].end, 7u);
  // The controller writes the text back: the echo changes nothing.
  d.OnDraftChanged(edit->text, edit->cursor_chars, false);
  EXPECT_EQ(d.Mentions().size(), 1u);
}

TEST(MentionDraftTest, ChineseAndEmojiNamesAndPrecedingText) {
  MentionDraft d = Group();
  std::string t = Type(d, "", "你好😀 @小");
  const auto edit = d.Pick(0);
  ASSERT_TRUE(edit);
  EXPECT_EQ(edit->text, "你好😀 @小明 ");
  ASSERT_EQ(d.Mentions().size(), 1u);
  const std::string prefix = "你好😀 ";
  EXPECT_EQ(d.Mentions()[0].start, prefix.size());
  EXPECT_EQ(d.Mentions()[0].end, prefix.size() + std::string("@小明").size());
  EXPECT_EQ(edit->text.substr(d.Mentions()[0].start, d.Mentions()[0].end - d.Mentions()[0].start), "@小明");
  // cursor in code points: 你 好 😀 space @ 小 明 space
  EXPECT_EQ(edit->cursor_chars, 8);

  MentionDraft e = Group();
  Type(e, "", "@😀");
  const auto emoji = e.Pick(0);
  ASSERT_TRUE(emoji);
  EXPECT_EQ(emoji->text, "@😀Jo ");
  EXPECT_EQ(e.Mentions()[0].end, std::string("@😀Jo").size());
}

TEST(MentionDraftTest, NameWithSpaceIsNotANewQuery) {
  MentionDraft d = Group();
  Type(d, "", "@li");
  const auto edit = d.Pick(0);
  ASSERT_TRUE(edit);
  EXPECT_EQ(edit->text, "@Li Wei ");
  // Typing on after the inserted name: no popup for "Wei".
  d.OnDraftChanged(edit->text + "h", 9, true);
  EXPECT_FALSE(d.PopupOpen());
  // The cursor back inside the name does not reopen either.
  d.OnDraftChanged(edit->text + "h", 4, false);
  EXPECT_FALSE(d.PopupOpen());
}

TEST(MentionDraftTest, EveryoneInsertsLocalizedNameAndAllRecord) {
  MentionDraft d = Group(true);
  Type(d, "", "@");
  const auto edit = d.Pick(0);
  ASSERT_TRUE(edit);
  EXPECT_EQ(edit->text, "@Everyone ");
  ASSERT_EQ(d.Mentions().size(), 1u);
  EXPECT_TRUE(d.Mentions()[0].all);
  EXPECT_TRUE(d.Mentions()[0].account_id.empty());
  EXPECT_EQ(d.Mentions()[0].end, 9u);
}

TEST(MentionDraftTest, SeveralMentionsInOneMessage) {
  MentionDraft d = Group();
  std::string t = Type(d, "", "@bo");
  t = d.Pick(0)->text;
  t = Type(d, t, "@小");
  t = d.Pick(0)->text;
  EXPECT_EQ(t, "@Bob @小明 ");
  ASSERT_EQ(d.Mentions().size(), 2u);
  EXPECT_EQ(d.Mentions()[1].start, 5u);
}

TEST(MentionDraftTest, InsertingBeforeShiftsRanges) {
  MentionDraft d = Group();
  Type(d, "", "x @bo");
  const std::string t = d.Pick(0)->text; // "x @Bob "
  d.OnDraftChanged("hello " + t, 8, true);
  ASSERT_EQ(d.Mentions().size(), 1u);
  EXPECT_EQ(d.Mentions()[0].start, 8u);
  EXPECT_EQ(d.Mentions()[0].end, 12u);
  // Typing right at its start (before the '@') also shifts.
  d.OnDraftChanged("hello z" + t, 7, true);
  EXPECT_EQ(d.Mentions()[0].start, 9u);
}

TEST(MentionDraftTest, BackspaceAtTheEndDeletesTheWholeMention) {
  MentionDraft d = Group();
  Type(d, "", "hey @bo");
  std::string t = d.Pick(0)->text; // "hey @Bob "
  // The user removes the trailing space first: the record stays.
  d.OnDraftChanged("hey @Bob", 8, false);
  ASSERT_EQ(d.Mentions().size(), 1u);
  // Backspace at the end of the mention: the last letter goes, so the whole mention does.
  const auto edit = d.OnDraftChanged("hey @Bo", 7, false);
  ASSERT_TRUE(edit);
  EXPECT_EQ(edit->text, "hey ");
  EXPECT_EQ(edit->cursor_chars, 4);
  EXPECT_TRUE(d.Mentions().empty());
}

TEST(MentionDraftTest, BackspaceWholeDeleteShiftsLaterMentions) {
  MentionDraft d = Group();
  Type(d, "", "@bo");
  std::string t = d.Pick(0)->text; // "@Bob "
  t = Type(d, t, "@小");
  t = d.Pick(0)->text; // "@Bob @小明 "
  ASSERT_EQ(d.Mentions().size(), 2u);
  // Backspace at the end of "@Bob" (cursor after 'b'): text "@Bo @小明 ".
  const auto edit = d.OnDraftChanged("@Bo @小明 ", 3, false);
  ASSERT_TRUE(edit);
  EXPECT_EQ(edit->text, " @小明 ");
  ASSERT_EQ(d.Mentions().size(), 1u);
  EXPECT_EQ(d.Mentions()[0].account_id, "acc-zh");
  EXPECT_EQ(d.Mentions()[0].start, 1u);
}

TEST(MentionDraftTest, TypingInsideVoidsTheRecordButKeepsTheText) {
  MentionDraft d = Group();
  Type(d, "", "@bo");
  d.Pick(0); // "@Bob "
  d.OnDraftChanged("@BoXb ", 3, true);
  EXPECT_TRUE(d.Mentions().empty());
  EXPECT_TRUE(d.FinalizeForSend("@BoXb ", "@BoXb").empty());
}

TEST(MentionDraftTest, PastingInsideVoidsTheRecord) {
  MentionDraft d = Group();
  Type(d, "", "@bo");
  d.Pick(0);
  d.OnDraftChanged("@Bpasted ob ", 8, false);
  EXPECT_TRUE(d.Mentions().empty());
}

TEST(MentionDraftTest, TypingAfterTheMentionKeepsIt) {
  MentionDraft d = Group();
  Type(d, "", "@bo");
  std::string t = d.Pick(0)->text;
  Type(d, t, "hello");
  ASSERT_EQ(d.Mentions().size(), 1u);
  EXPECT_EQ(d.Mentions()[0].end, 4u);
}

TEST(MentionDraftTest, SpaceDeletedAtAndCursorAwayClosePopup) {
  MentionDraft d = Group();
  Type(d, "", "hi @b");
  ASSERT_TRUE(d.PopupOpen());
  d.OnDraftChanged("hi @b ", 6, true); // space
  EXPECT_FALSE(d.PopupOpen());

  MentionDraft e = Group();
  Type(e, "", "hi @b");
  e.OnDraftChanged("hi b", 3, false); // '@' deleted
  EXPECT_FALSE(e.PopupOpen());

  MentionDraft f = Group();
  Type(f, "", "hi @b");
  f.OnDraftChanged("hi @b", 1, false); // cursor moved away
  EXPECT_FALSE(f.PopupOpen());

  MentionDraft g = Group();
  Type(g, "", "hi @b");
  g.ClosePopup(); // tap outside
  EXPECT_FALSE(g.PopupOpen());
}

TEST(MentionDraftTest, ReopensOnlyByTyping) {
  MentionDraft d = Group();
  Type(d, "", "hi @b");
  d.ClosePopup();
  // The cursor wandering back into "@b" does not reopen.
  d.OnDraftChanged("hi @b", 1, false);
  d.OnDraftChanged("hi @b", 5, false);
  EXPECT_FALSE(d.PopupOpen());
}

TEST(MentionDraftTest, EnterIsNeverASelect) {
  // Enter is not an input to MentionDraft at all: a newline typed into the box ends the query.
  MentionDraft d = Group();
  Type(d, "", "@b");
  d.OnDraftChanged("@b\n", 3, true);
  EXPECT_FALSE(d.PopupOpen());
  EXPECT_TRUE(d.Mentions().empty());
}

TEST(MentionDraftTest, FinalizeKeepsGoodRecordsAndShiftsForTrim) {
  MentionDraft d = Group();
  Type(d, "", "  hi @bo");
  const std::string t = d.Pick(0)->text; // "  hi @Bob "
  const auto out = d.FinalizeForSend(t, "hi @Bob");
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].start, 3u);
  EXPECT_EQ(out[0].end, 7u);
  EXPECT_EQ(out[0].account_id, "acc-bob");
}

TEST(MentionDraftTest, FinalizeDropsRecordWhoseSliceChanged) {
  MentionDraft d = Group();
  Type(d, "", "@bo");
  const std::string t = d.Pick(0)->text;
  EXPECT_TRUE(d.FinalizeForSend(t, "@Bab").empty());       // text no longer equals the insert
  EXPECT_TRUE(d.FinalizeForSend(t, "@B").empty());         // out of range
  EXPECT_TRUE(d.FinalizeForSend("other draft", "@Bob").empty()); // draft not the one the records belong to
}

TEST(MentionDraftTest, FinalizeKeepsOneEveryoneAndCapsAt50) {
  MentionDraft d = Group(true);
  Type(d, "", "@");
  std::string t = d.Pick(0)->text;
  t = Type(d, t, "@");
  const auto second = d.Pick(0);
  ASSERT_TRUE(second);
  t = second->text;
  EXPECT_EQ(d.Mentions().size(), 1u); // the second @Everyone is plain text
  EXPECT_EQ(d.FinalizeForSend(t, t).size(), 1u);

  std::vector<MentionCandidate> many;
  for (int i = 0; i < 60; ++i) {
    many.push_back({"acc-" + std::to_string(1000 + i), "m" + std::to_string(1000 + i)});
  }
  MentionDraft big = Group(false, many);
  std::string text;
  for (int i = 0; i < 60; ++i) {
    text = Type(big, text, "@m" + std::to_string(1000 + i));
    text = big.Pick(0)->text;
  }
  EXPECT_EQ(big.FinalizeForSend(text, text).size(), 50u);
}

TEST(MentionDraftTest, ClearedDraftDropsRecords) {
  MentionDraft d = Group();
  Type(d, "", "@bo");
  d.Pick(0);
  d.OnDraftChanged("", 0, false); // sent: the box is empty
  EXPECT_TRUE(d.Mentions().empty());
}

TEST(MentionDraftTest, CursorUnitConversion) {
  const std::string s = "a你😀b"; // 1 + 3 + 4 + 1 bytes, 4 code points
  EXPECT_EQ(CursorToByte(s, 0), 0u);
  EXPECT_EQ(CursorToByte(s, 1), 1u);
  EXPECT_EQ(CursorToByte(s, 2), 4u);
  EXPECT_EQ(CursorToByte(s, 3), 8u);
  EXPECT_EQ(CursorToByte(s, 4), 9u);
  EXPECT_EQ(CursorToByte(s, 99), 9u);
  EXPECT_EQ(ByteToCursor(s, 0), 0);
  EXPECT_EQ(ByteToCursor(s, 4), 2);
  EXPECT_EQ(ByteToCursor(s, 8), 3);
  EXPECT_EQ(ByteToCursor(s, 9), 4);
  EXPECT_EQ(ByteToCursor(s, 99), 4);
}

TEST(MentionDraftTest, TriggerWithCursorInCodePoints) {
  MentionDraft d = Group();
  // "你好 @b" cursor at the end is 5 code points, 8 bytes + ...
  d.OnDraftChanged("你好 @b", 5, true);
  EXPECT_TRUE(d.PopupOpen());
}

} // namespace
} // namespace pbr
