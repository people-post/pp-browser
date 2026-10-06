#include "gui/chat/SessionMenu.h"

#include <gtest/gtest.h>

#include <vector>

using namespace pbr;

namespace {

using Item = SessionMenuItem;

std::vector<Item> Items(const SessionKind kind, const bool unread = false, const bool has_contact = false) {
  return SessionMenuItems(SessionMenuState{.kind = kind, .unread = unread, .has_contact = has_contact});
}

} // namespace

TEST(SessionMenuTest, DirectChatWithAContact) {
  EXPECT_EQ(Items(SessionKind::Direct, false, true),
            (std::vector<Item>{Item::MarkUnread, Item::ViewContact, Item::ClearChat, Item::Delete}));
}

TEST(SessionMenuTest, DirectChatWithAStrangerHasNoContactToView) {
  EXPECT_EQ(Items(SessionKind::Direct), (std::vector<Item>{Item::MarkUnread, Item::ClearChat, Item::Delete}));
}

TEST(SessionMenuTest, UnreadChatOffersMarkRead) {
  EXPECT_EQ(Items(SessionKind::Direct, true).front(), Item::MarkRead);
  EXPECT_EQ(Items(SessionKind::Ai, true).front(), Item::MarkRead);
}

TEST(SessionMenuTest, GroupLeavesInsteadOfDeleting) {
  // Closing a group thread is leaving the group (the close flow asks and hands the group over when needed).
  EXPECT_EQ(Items(SessionKind::Group, false, true),
            (std::vector<Item>{Item::MarkUnread, Item::ClearChat, Item::LeaveGroup}));
}

TEST(SessionMenuTest, AiThreadHasNoContactAndNoGroup) {
  EXPECT_EQ(Items(SessionKind::Ai, false, true), (std::vector<Item>{Item::MarkUnread, Item::ClearChat, Item::Delete}));
}

TEST(SessionMenuTest, DestructiveItemsComeLast) {
  for (const SessionKind kind : {SessionKind::Ai, SessionKind::Direct, SessionKind::Group}) {
    const std::vector<Item> items = Items(kind, false, true);
    bool seen_destructive = false;
    for (const Item item : items) {
      if (SessionMenuItemIsDestructive(item)) {
        seen_destructive = true;
      } else {
        EXPECT_FALSE(seen_destructive) << "a safe item after a destructive one";
      }
    }
    EXPECT_TRUE(seen_destructive);
  }
}
