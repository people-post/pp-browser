#include "feature/presenters/ChatPresenter.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// PP助手 (AI conversations) and Chats (people) keep separate archive views that never mix.
ArchiveItem Human(const bool archived) { return {.is_ai = false, .listed = true, .archived = archived}; }
ArchiveItem Ai(const bool archived, const bool listed = true) {
  return {.is_ai = true, .listed = listed, .archived = archived};
}

ChatListBucket Where(const ArchiveItem& item, const bool showing, const bool showing_ai) {
  return ClassifyForList(item.is_ai, item.listed, item.archived, showing, showing_ai);
}

TEST(ArchiveViewsTest, ArchivedAiThreadShowsOnlyInTheAiArchivedView) {
  EXPECT_EQ(Where(Ai(false), false, false), ChatListBucket::Ai);
  EXPECT_EQ(Where(Ai(true), false, false), ChatListBucket::Hidden);
  EXPECT_EQ(Where(Ai(true), false, true), ChatListBucket::Ai);
  EXPECT_EQ(Where(Ai(false), false, true), ChatListBucket::Hidden);
  // The Chats archive view being open never lets an AI thread into the human rows.
  EXPECT_EQ(Where(Ai(true), true, false), ChatListBucket::Hidden);
  EXPECT_NE(Where(Ai(true), true, true), ChatListBucket::Human);
  EXPECT_NE(Where(Ai(false), true, false), ChatListBucket::Human);
}

TEST(ArchiveViewsTest, ArchivedHumanThreadShowsOnlyInTheChatsArchivedView) {
  EXPECT_EQ(Where(Human(false), false, false), ChatListBucket::Human);
  EXPECT_EQ(Where(Human(true), false, false), ChatListBucket::Hidden);
  EXPECT_EQ(Where(Human(true), true, false), ChatListBucket::Human);
  // The AI archive view being open never moves a human thread, and never lists it as an AI row.
  EXPECT_EQ(Where(Human(true), false, true), ChatListBucket::Hidden);
  EXPECT_EQ(Where(Human(false), false, true), ChatListBucket::Human);
  EXPECT_NE(Where(Human(true), true, true), ChatListBucket::Ai);
  EXPECT_NE(Where(Human(false), true, true), ChatListBucket::Ai);
}

TEST(ArchiveViewsTest, AnUnlistedAiThreadShowsNowhere) {
  EXPECT_EQ(Where(Ai(false, false), false, false), ChatListBucket::Hidden);
  EXPECT_EQ(Where(Ai(true, false), false, true), ChatListBucket::Hidden);
}

TEST(ArchiveViewsTest, EachCountOnlyCountsItsOwnKind) {
  const ArchiveViews views =
      ResolveArchiveViews({Human(true), Human(true), Human(false), Ai(true), Ai(false), Ai(true, false)}, false, false);
  EXPECT_EQ(views.archived_count, 2);
  EXPECT_EQ(views.archived_count_ai, 1);
}

TEST(ArchiveViewsTest, EachViewFlagResetsWhenItsOwnCountDropsToZero) {
  // Only AI archived: the Chats view (if it was open) resets, the AI view stays.
  ArchiveViews views = ResolveArchiveViews({Human(false), Ai(true)}, true, true);
  EXPECT_FALSE(views.showing_archived);
  EXPECT_TRUE(views.showing_archived_ai);
  // Only human archived: the opposite.
  views = ResolveArchiveViews({Human(true), Ai(false)}, true, true);
  EXPECT_TRUE(views.showing_archived);
  EXPECT_FALSE(views.showing_archived_ai);
  // Nothing archived: both reset.
  views = ResolveArchiveViews({Human(false), Ai(false)}, true, true);
  EXPECT_FALSE(views.showing_archived);
  EXPECT_FALSE(views.showing_archived_ai);
}

TEST(ArchiveViewsTest, UnarchivingFromTheAiViewReturnsTheThreadToTheNormalAiList) {
  // Archived and the AI view open: listed there; the view is on.
  ArchiveViews views = ResolveArchiveViews({Ai(true)}, false, true);
  EXPECT_TRUE(views.showing_archived_ai);
  EXPECT_EQ(Where(Ai(true), views.showing_archived, views.showing_archived_ai), ChatListBucket::Ai);
  // Flipped back (the last one): the view resets and the thread is in the normal AI list.
  views = ResolveArchiveViews({Ai(false)}, false, true);
  EXPECT_FALSE(views.showing_archived_ai);
  EXPECT_EQ(views.archived_count_ai, 0);
  EXPECT_EQ(Where(Ai(false), views.showing_archived, views.showing_archived_ai), ChatListBucket::Ai);
}

} // namespace
} // namespace pbr
