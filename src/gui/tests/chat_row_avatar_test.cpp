// Phone chats list: a row carries the person's own photo when it is cached, else the letter avatar.
#include "gui/chat/ChatController.h"
#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace pbr {
namespace {

std::string ReadAsset(const std::string& relative) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

TEST(ChatRowAvatarTest, RowCarriesThePhotoSourceWhenTheIconIsCached) {
  ChatSessionRow from;
  from.id = "t1";
  from.title = "Alice";
  from.kind = "private";
  from.avatar_letter = "A";
  from.icon_path = "/profiles/x/icons/alice.jpg";
  const SessionDisplayRow row = ToSessionDisplayRow(from);
  EXPECT_TRUE(row.has_icon);
  EXPECT_EQ(std::string(row.icon_src.c_str()), "/profiles/x/icons/alice.jpg");
}

TEST(ChatRowAvatarTest, RowFallsBackToTheLetterWhenNothingIsCached) {
  ChatSessionRow from;
  from.id = "t2";
  from.title = "Bob";
  from.kind = "private";
  from.avatar_letter = "B";
  from.avatar_tone = 3;
  const SessionDisplayRow row = ToSessionDisplayRow(from);
  EXPECT_FALSE(row.has_icon);
  EXPECT_TRUE(row.icon_src.empty());
  EXPECT_EQ(std::string(row.avatar_letter.c_str()), "B");
  EXPECT_EQ(row.avatar_tone, 3);
}

// Kenneth 2026-11-04: a blocked person's chat is marked so they can be found again to unblock.
TEST(ChatRowAvatarTest, ABlockedChatSaysBlockedInsteadOfTheLastMessage) {
  ChatSessionRow from;
  from.id = "t3";
  from.title = "Carol";
  from.kind = "private";
  from.preview = "see you";
  from.blocked = true;
  const SessionDisplayRow row = ToSessionDisplayRow(from);
  EXPECT_TRUE(row.blocked);
  EXPECT_EQ(std::string(row.preview.c_str()), Tr("sidebar.blocked_preview"));
  from.blocked = false;
  EXPECT_EQ(std::string(ToSessionDisplayRow(from).preview.c_str()), "see you");
}

TEST(ChatRowAvatarTest, TheBlockMarkIsPhoneOnlyAndSitsBeforeThePreview) {
  const std::string rml = ReadAsset("views/sidebar.rml");
  const size_t mark = rml.find("class=\"chat-row-blocked\"");
  ASSERT_NE(mark, std::string::npos);
  EXPECT_LT(mark, rml.find("sidebar-session-preview"));
  EXPECT_NE(rml.find("data-if=\"session.blocked\"", mark), std::string::npos);
}

} // namespace
} // namespace pbr
