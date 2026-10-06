#include "gui/chat/SessionMenu.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace pbr;

namespace {

using Item = SessionMenuItem;

std::vector<Item> Items(const SessionKind kind, const bool unread = false, const bool has_contact = false) {
  return SessionMenuItems(SessionMenuState{.kind = kind, .unread = unread, .has_contact = has_contact});
}

bool Has(const std::vector<Item>& items, const Item item) {
  return std::find(items.begin(), items.end(), item) != items.end();
}

} // namespace

TEST(SessionMenuTest, DirectChatWithAContact) {
  EXPECT_EQ(Items(SessionKind::Direct, false, true),
            (std::vector<Item>{Item::MarkUnread, Item::Pin, Item::Mute, Item::Archive, Item::ViewContact,
                               Item::ClearChat, Item::Delete}));
}

TEST(SessionMenuTest, DirectChatWithAStrangerHasNoContactToView) {
  EXPECT_FALSE(Has(Items(SessionKind::Direct), Item::ViewContact));
}

TEST(SessionMenuTest, UnreadChatOffersMarkRead) {
  EXPECT_EQ(Items(SessionKind::Direct, true).front(), Item::MarkRead);
  EXPECT_EQ(Items(SessionKind::Ai, true).front(), Item::MarkRead);
}

TEST(SessionMenuTest, GroupLeavesInsteadOfDeleting) {
  // Closing a group thread is leaving the group (the close flow asks and hands the group over when needed).
  EXPECT_EQ(Items(SessionKind::Group, false, true),
            (std::vector<Item>{Item::MarkUnread, Item::Pin, Item::Mute, Item::Archive, Item::ClearChat,
                               Item::LeaveGroup}));
}

TEST(SessionMenuTest, AiThreadHasNothingToMute) {
  // An AI thread never notifies, and has no contact or group.
  EXPECT_EQ(Items(SessionKind::Ai, false, true),
            (std::vector<Item>{Item::MarkUnread, Item::Pin, Item::Archive, Item::ClearChat, Item::Delete}));
}

TEST(SessionMenuTest, SetStatesOfferTheirOpposite) {
  SessionMenuState state{.kind = SessionKind::Direct};
  state.pinned = true;
  state.muted = true;
  state.archived = true;
  const std::vector<Item> items = SessionMenuItems(state);
  EXPECT_TRUE(Has(items, Item::Unpin) && !Has(items, Item::Pin));
  EXPECT_TRUE(Has(items, Item::Unmute) && !Has(items, Item::Mute));
  EXPECT_TRUE(Has(items, Item::Unarchive) && !Has(items, Item::Archive));
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

TEST(SessionMenuTest, MuteExpires) {
  const int64_t now = 1'700'000'000'000;
  EXPECT_FALSE(ThreadIsMuted(0, now));
  EXPECT_TRUE(ThreadIsMuted(now + 1, now));
  EXPECT_FALSE(ThreadIsMuted(now, now));
  EXPECT_FALSE(ThreadIsMuted(now - 1, now));
  EXPECT_TRUE(ThreadIsMuted(kSessionMutedForever, now));
}

TEST(SessionMenuTest, MuteChoicesAreEightHoursAWeekOrAlways) {
  const int64_t now = 1'700'000'000'000;
  EXPECT_EQ(MutedUntilFor(SessionMuteChoice::EightHours, now), now + 8LL * 60 * 60 * 1000);
  EXPECT_EQ(MutedUntilFor(SessionMuteChoice::OneWeek, now), now + 7LL * 24 * 60 * 60 * 1000);
  EXPECT_EQ(MutedUntilFor(SessionMuteChoice::Always, now), kSessionMutedForever);
}

TEST(SessionMenuTest, PinnedRowsSortFirstThenByActivity) {
  struct Row {
    const char* id;
    int64_t pinned_at;
    int64_t updated_at;
  };
  std::vector<Row> rows = {
      {"old", 0, 10}, {"new", 0, 30}, {"pinned-earlier", 100, 5}, {"mid", 0, 20}, {"pinned-later", 200, 1},
  };
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    return SessionSortsBefore(a.pinned_at, a.updated_at, b.pinned_at, b.updated_at);
  });
  std::vector<std::string> order;
  for (const Row& row : rows) {
    order.emplace_back(row.id);
  }
  // The most recently pinned is on top; unpinned rows keep the newest-first order.
  EXPECT_EQ(order, (std::vector<std::string>{"pinned-later", "pinned-earlier", "new", "mid", "old"}));
}
