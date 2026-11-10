#include "gui/chat/ReadWhenLooking.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

ShellChromeSnapshot Desktop(NavTab tab, const std::string& pane) {
  ShellChromeSnapshot chrome;
  chrome.layout_mode = LayoutMode::Expanded;
  chrome.nav_tab = tab;
  chrome.primary_pane_key = pane;
  return chrome;
}

ShellChromeSnapshot Phone(NavTab tab, bool chat_open) {
  ShellChromeSnapshot chrome;
  chrome.layout_mode = LayoutMode::Compact;
  chrome.nav_tab = tab;
  chrome.compact_chat_open = chat_open;
  return chrome;
}

TEST(ReadWhenLookingTest, ReadsOnlyWhenEveryConditionHolds) {
  EXPECT_TRUE(ShouldMarkOpenChatRead(true, true, true, true, false));
}

TEST(ReadWhenLookingTest, EveryOtherCombinationLeavesItUnread) {
  for (int bits = 0; bits < 31; ++bits) { // 31 = attentive, open, on screen, unread, no veto: the case above
    const bool attentive = (bits & 1) != 0;
    const bool open = (bits & 2) != 0;
    const bool shows = (bits & 4) != 0;
    const bool unread = (bits & 8) != 0;
    const bool veto = (bits & 16) == 0; // bit clear = a tap is pending elsewhere
    EXPECT_FALSE(ShouldMarkOpenChatRead(attentive, open, shows, unread, veto)) << "bits=" << bits;
  }
}

TEST(ChatTranscriptOnScreenTest, DesktopHomeTabWithTheChatPaneShows) {
  EXPECT_TRUE(ChatTranscriptOnScreen(Desktop(NavTab::Home, "chat"), "t1", "t1"));
  EXPECT_FALSE(ChatPageShows(Desktop(NavTab::Home, "chat"), "t1", "t1")) << "why ChatPageShows is not used here";
}

TEST(ChatTranscriptOnScreenTest, DesktopChatsTabWithTheChatPaneShows) {
  EXPECT_TRUE(ChatTranscriptOnScreen(Desktop(NavTab::Sessions, "chat"), "t1", "t1"));
}

TEST(ChatTranscriptOnScreenTest, DesktopWithAnotherPaneDoesNotShow) {
  EXPECT_FALSE(ChatTranscriptOnScreen(Desktop(NavTab::Sessions, "list"), "t1", "t1"));
  EXPECT_FALSE(ChatTranscriptOnScreen(Desktop(NavTab::Home, "home"), "t1", "t1"));
}

TEST(ChatTranscriptOnScreenTest, DesktopOtherThreadOrNoThreadDoesNotShow) {
  EXPECT_FALSE(ChatTranscriptOnScreen(Desktop(NavTab::Sessions, "chat"), "t1", "t2"));
  EXPECT_FALSE(ChatTranscriptOnScreen(Desktop(NavTab::Sessions, "chat"), "", ""));
}

TEST(ChatTranscriptOnScreenTest, PhoneNeedsTheChatPageOpenOnAListTab) {
  EXPECT_TRUE(ChatTranscriptOnScreen(Phone(NavTab::Sessions, true), "t1", "t1"));
  EXPECT_FALSE(ChatTranscriptOnScreen(Phone(NavTab::Sessions, false), "t1", "t1"));
  EXPECT_FALSE(ChatTranscriptOnScreen(Phone(NavTab::Home, true), "t1", "t1"));
}

TEST(TapPendingElsewhereTest, NoTapNoVeto) {
  EXPECT_FALSE(TapPendingElsewhere(std::nullopt, false, "t1"));
}

TEST(TapPendingElsewhereTest, ATapOfAnotherChatVetoes) {
  EXPECT_TRUE(TapPendingElsewhere(std::string("t2"), false, "t1"));
}

TEST(TapPendingElsewhereTest, ATapOfTheOpenChatDoesNot) {
  EXPECT_FALSE(TapPendingElsewhere(std::string("t1"), false, "t1"));
}

TEST(TapPendingElsewhereTest, AnAlertTapWithAnUnknownChatVetoes) {
  EXPECT_TRUE(TapPendingElsewhere(std::nullopt, true, "t1"));
  EXPECT_TRUE(TapPendingElsewhere(std::string(), true, "t1"));
}

} // namespace
} // namespace pbr
