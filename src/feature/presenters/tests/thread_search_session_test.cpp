#include "feature/presenters/ThreadSearchSession.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

using namespace pbr;

namespace {

std::vector<ThreadSearchEntry> Index(const std::vector<std::pair<std::string, std::string>>& messages) {
  // Oldest first, as the store returns them.
  std::vector<ThreadSearchEntry> index;
  int64_t order = 1;
  for (const auto& [id, text] : messages) {
    index.push_back({id, order++, FoldForSearch(text)});
  }
  return index;
}

/** Five messages saying "tea": t1 (oldest) … t5 (newest), and one that does not. */
std::vector<ThreadSearchEntry> FiveTeas() {
  return Index({{"t1", "tea"}, {"x", "coffee"}, {"t2", "tea"}, {"t3", "tea"}, {"t4", "tea"}, {"t5", "tea"}});
}

ThreadSearchSession OpenWith(std::vector<ThreadSearchEntry> index, const std::string& query = "tea") {
  ThreadSearchSession session;
  const uint64_t generation = session.Open();
  EXPECT_TRUE(session.IndexReady(generation, std::move(index), 0));
  session.SetQuery(query);
  return session;
}

} // namespace

TEST(ThreadSearchSessionTest, OpenIsIndexingUntilTheFirstIndex) {
  ThreadSearchSession session;
  const uint64_t generation = session.Open();
  EXPECT_TRUE(session.view().open);
  EXPECT_TRUE(session.view().indexing);
  EXPECT_TRUE(session.IndexReady(generation, FiveTeas(), 0));
  EXPECT_FALSE(session.view().indexing);
}

TEST(ThreadSearchSessionTest, IndexingStaysFalseDuringARebuildAndOldHitsStayUsable) {
  ThreadSearchSession session = OpenWith(FiveTeas());
  ASSERT_EQ(session.view().hit_count, 5U);
  const std::optional<uint64_t> rebuild = session.MessagesChanged(5000);
  ASSERT_TRUE(rebuild.has_value());
  EXPECT_FALSE(session.view().indexing);
  EXPECT_EQ(session.view().hit_count, 5U);
  session.Older();
  EXPECT_EQ(session.CurrentMessageId(), "t4");
  EXPECT_TRUE(session.IndexReady(*rebuild, FiveTeas(), 5100));
  EXPECT_FALSE(session.view().indexing);
}

TEST(ThreadSearchSessionTest, ALateIndexAfterCloseIsDropped) {
  ThreadSearchSession session;
  const uint64_t generation = session.Open();
  ASSERT_TRUE(session.IndexReady(generation, FiveTeas(), 0));
  session.SetQuery("tea");
  session.Close();
  EXPECT_FALSE(session.IndexReady(generation, FiveTeas(), 10));
  EXPECT_FALSE(session.HasIndex());
  EXPECT_TRUE(session.hits().empty());
  EXPECT_FALSE(session.view().open);
  EXPECT_EQ(session.view().query, "");
}

TEST(ThreadSearchSessionTest, OpenThenCloseBeforeTheIndexDropsIt) {
  ThreadSearchSession session;
  const uint64_t generation = session.Open();
  session.Close();
  EXPECT_FALSE(session.IndexReady(generation, FiveTeas(), 0));
  EXPECT_FALSE(session.HasIndex());
  // Reopened: the first open's build is still stale.
  const uint64_t again = session.Open();
  EXPECT_NE(again, generation);
  EXPECT_FALSE(session.IndexReady(generation, FiveTeas(), 0));
  EXPECT_TRUE(session.view().indexing);
}

TEST(ThreadSearchSessionTest, SetQueryCountsAndJumpsToTheNewest) {
  ThreadSearchSession session = OpenWith(FiveTeas());
  EXPECT_EQ(session.view().current, 0U);
  EXPECT_EQ(session.view().hit_count, 5U);
  EXPECT_EQ(session.TakeJump(), std::optional<std::string>("t5"));
  EXPECT_FALSE(session.TakeJump().has_value()); // once
  session.Older();
  session.SetQuery("TEA ");
  EXPECT_EQ(session.view().current, 0U);
  EXPECT_EQ(session.TakeJump(), std::optional<std::string>("t5"));
  session.SetQuery("milk");
  EXPECT_EQ(session.view().hit_count, 0U);
  EXPECT_FALSE(session.TakeJump().has_value());
}

TEST(ThreadSearchSessionTest, QueryTypedBeforeTheIndexJumpsWhenItArrives) {
  ThreadSearchSession session;
  const uint64_t generation = session.Open();
  session.SetQuery("tea");
  EXPECT_EQ(session.view().hit_count, 0U);
  EXPECT_FALSE(session.TakeJump().has_value());
  ASSERT_TRUE(session.IndexReady(generation, FiveTeas(), 0));
  EXPECT_EQ(session.view().hit_count, 5U);
  EXPECT_EQ(session.TakeJump(), std::optional<std::string>("t5"));
}

TEST(ThreadSearchSessionTest, OlderAndNewerStopAtTheEnds) {
  ThreadSearchSession session = OpenWith(FiveTeas());
  (void)session.TakeJump();
  EXPECT_FALSE(session.view().can_newer);
  EXPECT_TRUE(session.view().can_older);
  session.Newer();
  EXPECT_EQ(session.view().current, 0U);
  EXPECT_FALSE(session.TakeJump().has_value());
  for (int i = 0; i < 10; ++i) {
    session.Older();
  }
  EXPECT_EQ(session.view().current, 4U);
  EXPECT_EQ(session.CurrentMessageId(), "t1");
  EXPECT_FALSE(session.view().can_older);
  EXPECT_TRUE(session.view().can_newer);
  session.Newer();
  EXPECT_EQ(session.TakeJump(), std::optional<std::string>("t2"));
}

TEST(ThreadSearchSessionTest, MessagesChangedWhileBuildingOnlyMarksARebuild) {
  ThreadSearchSession session;
  const uint64_t generation = session.Open();
  EXPECT_FALSE(session.MessagesChanged(100).has_value()); // the first build still runs
  EXPECT_FALSE(session.Tick(200).has_value());
  ASSERT_TRUE(session.IndexReady(generation, FiveTeas(), 300));
  EXPECT_FALSE(session.Tick(1000).has_value()); // too soon after the build
  const std::optional<uint64_t> rebuild = session.Tick(2300);
  ASSERT_TRUE(rebuild.has_value());
  EXPECT_FALSE(session.Tick(5000).has_value());            // one at a time, and nothing more pending
  EXPECT_FALSE(session.MessagesChanged(5000).has_value()); // the rebuild is still running
  ASSERT_TRUE(session.IndexReady(*rebuild, FiveTeas(), 5100));
  EXPECT_TRUE(session.Tick(7100).has_value());
}

TEST(ThreadSearchSessionTest, ClosedSearchNeverAsksForABuild) {
  ThreadSearchSession session;
  EXPECT_FALSE(session.MessagesChanged(0).has_value());
  EXPECT_FALSE(session.Tick(100000).has_value());
}

TEST(ThreadSearchSessionTest, ThirtySecondsOfChangesEveryTenthOfASecondBuildAtMostSixteenTimes) {
  ThreadSearchSession session;
  int builds = 1;
  ASSERT_TRUE(session.IndexReady(session.Open(), FiveTeas(), 0));
  for (int64_t now = 100; now <= 30000; now += 100) {
    for (std::optional<uint64_t> generation : {session.MessagesChanged(now), session.Tick(now)}) {
      if (generation) {
        ++builds;
        ASSERT_TRUE(session.IndexReady(*generation, FiveTeas(), now)); // built at once
      }
    }
  }
  EXPECT_LE(builds, 16);
  EXPECT_GE(builds, 10); // and it does keep up
}

TEST(ThreadSearchSessionTest, RebuildKeepsTheCursorOnTheSameMessage) {
  ThreadSearchSession session = OpenWith(FiveTeas());
  session.Older();
  session.Older(); // on t3
  (void)session.TakeJump();
  ASSERT_EQ(session.CurrentMessageId(), "t3");
  const std::optional<uint64_t> rebuild = session.MessagesChanged(5000);
  ASSERT_TRUE(rebuild.has_value());
  // A new message with the word arrived: the count grows, the cursor stays on t3, the view does not move.
  ASSERT_TRUE(session.IndexReady(*rebuild,
                                 Index({{"t1", "tea"},
                                        {"x", "coffee"},
                                        {"t2", "tea"},
                                        {"t3", "tea"},
                                        {"t4", "tea"},
                                        {"t5", "tea"},
                                        {"t6", "more tea"}}),
                                 5100));
  EXPECT_EQ(session.view().hit_count, 6U);
  EXPECT_EQ(session.CurrentMessageId(), "t3");
  EXPECT_EQ(session.view().current, 3U);
  EXPECT_FALSE(session.TakeJump().has_value());
}

TEST(ThreadSearchSessionTest, RebuildWithoutTheCurrentMessageGoesToTheNewest) {
  ThreadSearchSession session = OpenWith(FiveTeas());
  session.Older(); // on t4
  (void)session.TakeJump();
  const std::optional<uint64_t> rebuild = session.MessagesChanged(5000);
  ASSERT_TRUE(rebuild.has_value());
  ASSERT_TRUE(session.IndexReady(*rebuild, Index({{"t1", "tea"}, {"t2", "tea"}, {"t3", "tea"}, {"t5", "tea"}}), 5100));
  EXPECT_EQ(session.view().current, 0U);
  EXPECT_EQ(session.TakeJump(), std::optional<std::string>("t5"));
}

TEST(ThreadSearchSessionTest, DropHitCountsDownAndKeepsTheCursorInRange) {
  ThreadSearchSession session = OpenWith(FiveTeas());
  (void)session.TakeJump();
  for (int i = 0; i < 4; ++i) {
    session.Older(); // on t1, the oldest
  }
  (void)session.TakeJump();
  EXPECT_TRUE(session.DropHit("t1"));
  EXPECT_EQ(session.view().hit_count, 4U);
  EXPECT_EQ(session.view().current, 3U);
  EXPECT_EQ(session.TakeJump(), std::optional<std::string>("t2"));
  // A hit newer than the cursor: the cursor stays on its message.
  EXPECT_TRUE(session.DropHit("t5"));
  EXPECT_EQ(session.CurrentMessageId(), "t2");
  EXPECT_FALSE(session.TakeJump().has_value());
  EXPECT_FALSE(session.DropHit("nope"));
  EXPECT_TRUE(session.DropHit("t2"));
  EXPECT_TRUE(session.DropHit("t3"));
  EXPECT_TRUE(session.DropHit("t4"));
  EXPECT_EQ(session.view().hit_count, 0U);
  EXPECT_EQ(session.view().current, 0U);
  EXPECT_FALSE(session.view().can_older);
  EXPECT_FALSE(session.view().can_newer);
}

TEST(ThreadSearchSessionTest, AFailedRebuildKeepsTheIndexAndHits) {
  ThreadSearchSession session = OpenWith(FiveTeas());
  session.Older(); // on t4
  (void)session.TakeJump();
  const std::optional<uint64_t> rebuild = session.MessagesChanged(5000);
  ASSERT_TRUE(rebuild.has_value());
  EXPECT_TRUE(session.IndexFailed(*rebuild, 5100));
  EXPECT_FALSE(session.view().indexing);
  EXPECT_TRUE(session.HasIndex());
  EXPECT_EQ(session.view().hit_count, 5U);
  EXPECT_EQ(session.CurrentMessageId(), "t4");
  EXPECT_FALSE(session.TakeJump().has_value());
  // Another change may rebuild again, spaced like any rebuild.
  EXPECT_FALSE(session.MessagesChanged(6000).has_value());
  EXPECT_TRUE(session.Tick(7100).has_value());
}

TEST(ThreadSearchSessionTest, AFailedFirstIndexEndsSearchingAndALateFailureIsIgnored) {
  ThreadSearchSession session;
  const uint64_t generation = session.Open();
  EXPECT_TRUE(session.IndexFailed(generation, 100));
  EXPECT_FALSE(session.view().indexing);
  EXPECT_TRUE(session.view().open);
  EXPECT_FALSE(session.HasIndex());
  session.Close();
  EXPECT_FALSE(session.IndexFailed(generation, 200));
  const uint64_t again = session.Open();
  EXPECT_FALSE(session.IndexFailed(generation, 300)); // the old generation
  EXPECT_TRUE(session.view().indexing);
  EXPECT_TRUE(session.IndexFailed(again, 300));
}
