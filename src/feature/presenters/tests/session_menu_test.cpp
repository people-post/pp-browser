#include "feature/presenters/SessionMenu.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
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

TEST(SessionMenuTest, DirectChatWithAStrangerOffersBlockButNoViewContact) {
  const auto items = Items(SessionKind::Direct);
  EXPECT_TRUE(Has(items, Item::Block));
  EXPECT_FALSE(Has(items, Item::Unblock));
  EXPECT_FALSE(Has(items, Item::ViewContact));
}

TEST(SessionMenuTest, NoBlockForAStrangerWithoutAnIdentity) {
  const auto items = SessionMenuItems(SessionMenuState{.kind = SessionKind::Direct, .has_peer_identity = false});
  EXPECT_FALSE(Has(items, Item::Block));
}

TEST(SessionMenuTest, DirectChatWithAContact) {
  EXPECT_EQ(Items(SessionKind::Direct, false, true),
            (std::vector<Item>{Item::MarkUnread, Item::Pin, Item::Mute, Item::Archive, Item::ViewContact,
                               Item::Block, Item::ClearChat, Item::Delete}));
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
            (std::vector<Item>{Item::MarkUnread, Item::Pin, Item::Rename, Item::Archive, Item::ClearChat,
                               Item::Delete}));
}

TEST(SessionMenuTest, PinnedAiThreadSortsBeforeNewerUnpinnedOnes) {
  EXPECT_TRUE(SessionSortsBefore(/*a_pinned_at=*/100, /*a_updated_at=*/1, /*b_pinned_at=*/0, /*b_updated_at=*/5));
  EXPECT_FALSE(SessionSortsBefore(0, 5, 100, 1));
  EXPECT_TRUE(SessionSortsBefore(0, 5, 0, 1)) << "unpinned: newest first";
  EXPECT_EQ(Items(SessionKind::Ai, false, true).at(1), Item::Pin);
  SessionMenuState pinned{.kind = SessionKind::Ai};
  pinned.pinned = true;
  EXPECT_TRUE(Has(SessionMenuItems(pinned), Item::Unpin));
}

TEST(SessionMenuTest, RenameIsOnlyForAiThreads) {
  EXPECT_TRUE(Has(SessionMenuItems(SessionMenuState{.kind = SessionKind::Ai}), Item::Rename));
  EXPECT_FALSE(Has(SessionMenuItems(SessionMenuState{.kind = SessionKind::Direct}), Item::Rename));
  EXPECT_FALSE(Has(SessionMenuItems(SessionMenuState{.kind = SessionKind::Group}), Item::Rename));
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

TEST(SessionMenuTest, AMutedGroupOffersToMuteItsMentionsToo) {
  // G015: a mention alerts in a muted group unless its mentions are muted too.
  SessionMenuState group{.kind = SessionKind::Group};
  EXPECT_FALSE(Has(SessionMenuItems(group), Item::MuteMentions)) << "not muted: nothing to add";
  group.muted = true;
  EXPECT_TRUE(Has(SessionMenuItems(group), Item::MuteMentions));
  group.mute_mentions = true;
  EXPECT_TRUE(Has(SessionMenuItems(group), Item::AllowMentions));
  EXPECT_FALSE(Has(SessionMenuItems(group), Item::MuteMentions));
  SessionMenuState direct{.kind = SessionKind::Direct, .muted = true};
  EXPECT_FALSE(Has(SessionMenuItems(direct), Item::MuteMentions)) << "a direct chat has no mentions";
}

TEST(SessionMenuTest, AskBeforeSharingWithAiIsOfferedOnlyWhenTheChatWasTold) {
  for (const SessionKind kind : {SessionKind::Direct, SessionKind::Group}) {
    SessionMenuState state{.kind = kind};
    EXPECT_FALSE(Has(SessionMenuItems(state), Item::AskBeforeSharingAi)) << "nothing to undo";
    state.share_ai_confirmed = true;
    EXPECT_TRUE(Has(SessionMenuItems(state), Item::AskBeforeSharingAi));
  }
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

TEST(SessionMenuTest, MarkedUnreadShowsADotUntilThereIsARealCount) {
  // A thread marked unread by hand has no message count to show: it gets a dot. Once real messages
  // arrive the number takes over.
  EXPECT_TRUE(SessionShowsUnreadDot(0, true));
  EXPECT_FALSE(SessionShowsUnreadDot(3, true));
  EXPECT_FALSE(SessionShowsUnreadDot(0, false));
  // Either kind of unread offers "Mark as read".
  EXPECT_TRUE(SessionIsUnread(0, true));
  EXPECT_TRUE(SessionIsUnread(2, false));
  EXPECT_FALSE(SessionIsUnread(0, false));
}

// Blocking lives where the chat is: a direct chat with a saved contact offers Block (or Unblock when they are
// already blocked), next to View contact; strangers and groups do not.
TEST(SessionMenuTest, DirectChatWithAContactOffersBlockOrUnblock) {
  const auto items = SessionMenuItems(SessionMenuState{.kind = SessionKind::Direct, .has_contact = true});
  EXPECT_TRUE(Has(items, Item::Block));
  EXPECT_FALSE(Has(items, Item::Unblock));
  const auto blocked =
      SessionMenuItems(SessionMenuState{.kind = SessionKind::Direct, .has_contact = true, .blocked = true});
  EXPECT_TRUE(Has(blocked, Item::Unblock));
  EXPECT_FALSE(Has(blocked, Item::Block));
  EXPECT_TRUE(Has(Items(SessionKind::Direct), Item::Block)) << "a stranger can be blocked too";
  EXPECT_FALSE(Has(Items(SessionKind::Group), Item::Block));
  EXPECT_TRUE(SessionMenuItemIsDestructive(Item::Block));
  EXPECT_FALSE(SessionMenuItemIsDestructive(Item::Unblock));
}

TEST(SessionMenuTest, EveryItemHasAnIconThatExists) {
  const SessionKind kinds[] = {SessionKind::Ai, SessionKind::Direct, SessionKind::Group};
  for (const SessionKind kind : kinds) {
    for (const bool flip : {false, true}) {
      SessionMenuState state{.kind = kind, .unread = flip, .has_contact = true, .blocked = flip, .pinned = flip,
                             .muted = flip, .mute_mentions = flip, .share_ai_confirmed = true, .archived = flip};
      for (const Item item : SessionMenuItems(state)) {
        const std::string icon = SessionMenuIcon(item);
        ASSERT_FALSE(icon.empty());
        // "../icons/v2/x.svg" is relative to assets/views/.
        EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / "views" / icon)) << icon;
      }
    }
  }
}
