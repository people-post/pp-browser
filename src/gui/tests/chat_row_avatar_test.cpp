// Phone chats list: a row carries the person's own photo when it is cached, else the letter avatar.
#include "gui/chat/ChatController.h"

#include <gtest/gtest.h>

#include <string>

namespace pbr {
namespace {

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

} // namespace
} // namespace pbr
