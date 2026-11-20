#include "gui/chat/ThreadDrafts.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>

namespace pbr {
namespace {

TEST(ThreadDraftsTest, EachConversationKeepsItsOwnUnsentText) {
  ThreadDrafts drafts;
  EXPECT_EQ(drafts.SwitchTo("A", ""), "");           // first owner
  EXPECT_EQ(drafts.SwitchTo("B", "for A only"), ""); // A's text stays with A; B starts empty
  EXPECT_EQ(drafts.SwitchTo("A", "for B"), "for A only");
  EXPECT_EQ(drafts.SwitchTo("B", "for A only"), "for B");
}

TEST(ThreadDraftsTest, HomeHasItsOwnDraft) {
  ThreadDrafts drafts;
  (void)drafts.SwitchTo(ThreadDrafts::kHome, "");
  EXPECT_EQ(drafts.SwitchTo("A", "a home question"), "");
  EXPECT_EQ(drafts.SwitchTo(ThreadDrafts::kHome, ""), "a home question");
}

TEST(ThreadDraftsTest, StayingInTheSameConversationKeepsTheText) {
  ThreadDrafts drafts;
  (void)drafts.SwitchTo("A", "");
  EXPECT_EQ(drafts.SwitchTo("A", "typing"), "typing");
}

TEST(ThreadDraftsTest, ASentDraftLeavesNothingBehind) {
  ThreadDrafts drafts;
  (void)drafts.SwitchTo("A", "");
  EXPECT_EQ(drafts.SwitchTo("B", ""), ""); // A was sent (composer empty): nothing kept
  EXPECT_EQ(drafts.Peek("A"), "");
}

TEST(ThreadDraftsTest, AClosedConversationForgetsItsText) {
  ThreadDrafts drafts;
  (void)drafts.SwitchTo("A", "");
  (void)drafts.SwitchTo("B", "unsent in A");
  drafts.Forget("A");
  EXPECT_EQ(drafts.SwitchTo("A", ""), "");
}

TEST(ThreadDraftsTest, PutBackGoesInFrontOfWhatWasThere) {
  ThreadDrafts drafts;
  (void)drafts.SwitchTo("A", "");
  (void)drafts.SwitchTo("B", "later text");
  drafts.Prepend("A", "held question");
  EXPECT_EQ(drafts.Peek("A"), "held question\nlater text");
  drafts.Prepend("C", "only this");
  EXPECT_EQ(drafts.Peek("C"), "only this");
}

// The line typed beside pictures that were not sent (DEV/PLAN-picture-send-preview.md "文字回哪"): after what the
// conversation already had, on a new line; plainly when it had nothing.
TEST(ThreadDraftsTest, AppendGoesAfterWhatWasThere) {
  ThreadDrafts drafts;
  (void)drafts.SwitchTo("A", "");
  (void)drafts.SwitchTo("B", "typed later in A");
  drafts.Append("A", "beside the pictures");
  EXPECT_EQ(drafts.Peek("A"), "typed later in A\nbeside the pictures");
  drafts.Append("C", "only this");
  EXPECT_EQ(drafts.Peek("C"), "only this");
}

// A chat that is not on screen keeps the text for when it is opened; the conversation on screen is not touched.
TEST(ThreadDraftsTest, AppendToAChatNotOnScreenLeavesTheShownOneAlone) {
  ThreadDrafts drafts;
  (void)drafts.SwitchTo("A", "");
  (void)drafts.SwitchTo("B", "kept in A"); // B is on screen now
  drafts.Append("A", "beside the pictures");
  EXPECT_EQ(drafts.Peek("B"), "") << "the open chat's text lives in the composer, not here";
  EXPECT_EQ(drafts.SwitchTo("A", "typing in B"), "kept in A\nbeside the pictures");
  EXPECT_EQ(drafts.Peek("B"), "typing in B");
}

TEST(ThreadDraftsTest, MergeDraftTextByMode) {
  EXPECT_EQ(MergeDraftText("", "new", DraftPutBack::IfEmpty), "new");
  EXPECT_EQ(MergeDraftText("old", "new", DraftPutBack::IfEmpty), "old");
  EXPECT_EQ(MergeDraftText("", "new", DraftPutBack::Prepend), "new");
  EXPECT_EQ(MergeDraftText("old", "new", DraftPutBack::Prepend), "new\nold");
  EXPECT_EQ(MergeDraftText("", "new", DraftPutBack::Append), "new");
  EXPECT_EQ(MergeDraftText("old", "new", DraftPutBack::Append), "old\nnew");
}

std::string ControllerSource() {
  std::ifstream in(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatController.cpp", std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  std::string text = out.str();
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  return text;
}

// ChatController cannot be built in a unit test: PutTextBack is pinned to merge both ways through the one rule above
// (the chat on screen through its composer text, any other one through ThreadDrafts), and the callers keep theirs.
TEST(ThreadDraftsSourcePinTest, PutTextBackMergesBothBranchesThroughTheOneRule) {
  const std::string source = ControllerSource();
  const size_t start = source.find(
      "void ChatController::PutTextBack(const std::string& key, const std::string& text, "
      "const DraftPutBack mode) {");
  ASSERT_NE(start, std::string::npos);
  const std::string body = source.substr(start, source.find("\n}\n", start) - start);
  const size_t off_screen = body.find("drafts_.PutBack(key, text, mode);");
  const size_t on_screen = body.find("MergeDraftText(draft, text, mode)");
  ASSERT_NE(off_screen, std::string::npos);
  ASSERT_NE(on_screen, std::string::npos);
  EXPECT_LT(body.find("key != CurrentDraftKey()"), off_screen) << "never the chat shown";
  EXPECT_NE(source.find("PutTextBack(thread_id, text, DraftPutBack::IfEmpty);"), std::string::npos);
  EXPECT_NE(source.find("PutTextBack(thread_id, text, DraftPutBack::Prepend);"), std::string::npos);
}

} // namespace
} // namespace pbr
