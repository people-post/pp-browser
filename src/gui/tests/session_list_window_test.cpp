#include "gui/chat/SessionListWindow.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace pbr {
namespace {

std::string ReadAsset(const char* relative) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

TEST(SessionListWindowTest, FirstWindowIsTwentyRows) {
  SessionListWindow window;
  EXPECT_EQ(window.Visible(500), 20u);
  EXPECT_TRUE(window.More(500));
}

TEST(SessionListWindowTest, ExtendsByTwentyNearTheBottomOnly) {
  SessionListWindow window;
  EXPECT_FALSE(window.ExtendIfNearBottom(500, /*near_bottom=*/false));
  EXPECT_EQ(window.Visible(500), 20u);
  EXPECT_TRUE(window.ExtendIfNearBottom(500, true));
  EXPECT_EQ(window.Visible(500), 40u);
  EXPECT_TRUE(window.ExtendIfNearBottom(500, true));
  EXPECT_EQ(window.Visible(500), 60u);
}

TEST(SessionListWindowTest, NeverExceedsTheRowCount) {
  SessionListWindow window;
  EXPECT_EQ(window.Visible(7), 7u);
  EXPECT_FALSE(window.More(7));
  EXPECT_FALSE(window.ExtendIfNearBottom(7, true));
  EXPECT_TRUE(window.ExtendIfNearBottom(25, true));
  EXPECT_EQ(window.Visible(25), 25u);
  EXPECT_EQ(window.Visible(10), 10u);  // the list shrank under the window
}

TEST(SessionListWindowTest, MoreIsFalseOnceEverythingIsShown) {
  SessionListWindow window;
  EXPECT_TRUE(window.More(45));
  window.ExtendIfNearBottom(45, true);
  EXPECT_TRUE(window.More(45));
  window.ExtendIfNearBottom(45, true);
  EXPECT_FALSE(window.More(45));
  EXPECT_FALSE(window.More(0));
}

TEST(SessionListWindowTest, ResetGoesBackToTwenty) {
  SessionListWindow window;
  window.ExtendIfNearBottom(500, true);
  window.ExtendIfNearBottom(500, true);
  window.Reset();
  EXPECT_EQ(window.Visible(500), 20u);
}

TEST(SessionListWindowTest, ARefreshDoesNotCollapseTheWindow) {
  SessionListWindow window;
  window.ExtendIfNearBottom(500, true);
  // A new message re-applies the list: nothing resets, the window stays at 40.
  EXPECT_EQ(window.Visible(501), 40u);
}

TEST(SessionListWindowTest, NearBottomNeedsAScrollableList) {
  EXPECT_FALSE(SessionListNearBottom(0.f, 400.f, 400.f));    // nothing to scroll
  EXPECT_FALSE(SessionListNearBottom(0.f, 2000.f, 500.f));   // at the top
  EXPECT_TRUE(SessionListNearBottom(1440.f, 2000.f, 500.f)); // 60dp from the end
  EXPECT_TRUE(SessionListNearBottom(1500.f, 2000.f, 500.f)); // at the end
}

// The sidebar shows the loading line only while rows remain, and both languages carry its text.
TEST(SessionListWindowTest, SidebarLoadingLineFollowsTheMoreFlag) {
  const std::string rml = ReadAsset("views/sidebar.rml");
  const size_t line = rml.find("{{i18n:sidebar.loading_more}}");
  ASSERT_NE(line, std::string::npos);
  const size_t start = rml.rfind("<p", line);
  ASSERT_NE(start, std::string::npos);
  EXPECT_NE(rml.substr(start, line - start).find("data-if=\"sessions_more\""), std::string::npos);
  EXPECT_NE(rml.find("data-event-scroll=\"sessions_scroll()\""), std::string::npos);
  EXPECT_NE(ReadAsset("locales/en.json").find("\"sidebar.loading_more\": \"Loading…\""), std::string::npos);
  EXPECT_NE(ReadAsset("locales/zh-Hans.json").find("\"sidebar.loading_more\": \"加载中…\""), std::string::npos);
}

} // namespace

TEST(SessionListWindowTest, IncludeReachesTheOpenChatInWholeSteps) {
  SessionListWindow window;
  EXPECT_FALSE(window.Include(5)) << "already inside the first window";
  EXPECT_EQ(window.Visible(100), 20u);
  EXPECT_TRUE(window.Include(20)) << "row 21 needs the second step";
  EXPECT_EQ(window.Visible(100), 40u);
  EXPECT_TRUE(window.Include(75));
  EXPECT_EQ(window.Visible(100), 80u);
  EXPECT_FALSE(window.Include(41)) << "never shrinks";
  EXPECT_EQ(window.Visible(100), 80u);
  window.Reset();
  EXPECT_EQ(window.Visible(100), 20u);
}

} // namespace pbr
