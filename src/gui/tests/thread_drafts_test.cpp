#include "gui/chat/ThreadDrafts.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(ThreadDraftsTest, EachConversationKeepsItsOwnUnsentText) {
  ThreadDrafts drafts;
  EXPECT_EQ(drafts.SwitchTo("A", ""), "");             // first owner
  EXPECT_EQ(drafts.SwitchTo("B", "for A only"), "");   // A's text stays with A; B starts empty
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

} // namespace
} // namespace pbr
