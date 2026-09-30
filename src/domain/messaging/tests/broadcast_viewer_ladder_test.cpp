#include "domain/messaging/BroadcastMedia.h"
#include "domain/messaging/BroadcastViewerLadder.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

using Action = BroadcastViewerLadder::Action;

// B009: the viewer takes the published level nearest its preference, below first.
TEST(BroadcastWatchVideoLevelTest, NearestPublishedLevelBelowFirst) {
  EXPECT_EQ(ChooseWatchVideoLevel({1, 2}, 2), 2);
  EXPECT_EQ(ChooseWatchVideoLevel({1, 2}, 1), 1);
  EXPECT_EQ(ChooseWatchVideoLevel({1}, 2), 1) << "below the preference";
  EXPECT_EQ(ChooseWatchVideoLevel({2}, 1), 2) << "nothing below: the lowest above";
  EXPECT_EQ(ChooseWatchVideoLevel({1, 3}, 2), 1);
  EXPECT_EQ(ChooseWatchVideoLevel({}, 2), 0) << "audio-only program";
}

TEST(BroadcastViewerLadderTest, AdmitAttachesToTheAdmittingHop) {
  BroadcastViewerLadder ladder({"h1", "h2"});
  auto step = ladder.Start();
  ASSERT_EQ(step.action, Action::Ask);
  EXPECT_EQ(step.hop, "h1");
  step = ladder.OnAdmitted("h1", "h1");
  EXPECT_EQ(step.action, Action::Attach);
  EXPECT_EQ(step.hop, "h1");
}

TEST(BroadcastViewerLadderTest, RedirectAsksHintsBeforeRemainingCandidatesAndStampsThePath) {
  BroadcastViewerLadder ladder({"l1a", "l1b"}, /*redirect_budget=*/4);
  ASSERT_EQ(ladder.Start().hop, "l1a");
  auto step = ladder.OnRedirect("l1a", {"child1", "child2"}, 3);
  ASSERT_EQ(step.action, Action::Ask);
  EXPECT_EQ(step.hop, "child1");
  EXPECT_EQ(ladder.PathStamp(), std::vector<std::string>{"l1a"});
  EXPECT_EQ(ladder.RedirectBudget(), 3);
  step = ladder.OnRefused("child1", "full");
  EXPECT_EQ(step.hop, "child2");
  step = ladder.OnRefused("child2", "full");
  EXPECT_EQ(step.hop, "l1b") << "then the remaining L1 candidates";
}

TEST(BroadcastViewerLadderTest, RedirectLoopsAndBudgetAreBounded) {
  BroadcastViewerLadder ladder({"a"}, /*redirect_budget=*/1);
  ASSERT_EQ(ladder.Start().hop, "a");
  auto step = ladder.OnRedirect("a", {"b", "a"}, 0);
  ASSERT_EQ(step.action, Action::Ask);
  EXPECT_EQ(step.hop, "b") << "the redirecting hop is never asked again";
  step = ladder.OnRedirect("b", {"c"}, 0);
  EXPECT_EQ(step.action, Action::GiveUp);
  EXPECT_NE(step.reason.find("budget"), std::string::npos) << step.reason;
}

TEST(BroadcastViewerLadderTest, HopWithoutAdmissionIsAttachedDirectly) {
  BroadcastViewerLadder ladder({"relay"});
  ASSERT_EQ(ladder.Start().hop, "relay");
  auto step = ladder.OnNoAdmissionService("relay");
  EXPECT_EQ(step.action, Action::Attach);
  EXPECT_EQ(step.hop, "relay");
}

TEST(BroadcastViewerLadderTest, AttachFailureMovesOnAndGivesUpWithTheLastReason) {
  BroadcastViewerLadder ladder({"h1", "h2", "h1", ""});
  ASSERT_EQ(ladder.Start().hop, "h1");
  auto step = ladder.OnAttachFailed("h1", "hop not dialable");
  ASSERT_EQ(step.hop, "h2") << "duplicates / empty candidates dropped";
  step = ladder.OnRefused("h2", "banned");
  EXPECT_EQ(step.action, Action::GiveUp);
  EXPECT_EQ(step.reason, "refused by h2: banned");
}

TEST(BroadcastViewerLadderTest, NoCandidatesGivesUp) {
  BroadcastViewerLadder ladder({});
  EXPECT_EQ(ladder.Start().action, Action::GiveUp);
}

TEST(BroadcastViewerLadderTest, TotalAsksAreCapped) {
  BroadcastViewerLadder ladder({"h0"}, /*redirect_budget=*/100, /*max_asks=*/3);
  auto step = ladder.Start();
  for (int i = 1; step.action == Action::Ask; ++i) {
    step = ladder.OnRedirect(step.hop, {"h" + std::to_string(i)}, 99);
  }
  EXPECT_EQ(step.action, Action::GiveUp);
}

TEST(BroadcastMediaTest, StreamIdComesFromThePublisherPeerIdAndContextIsBroadcastOwned) {
  EXPECT_EQ(BroadcastPublisherStreamId("12D3KooWPub"), PublisherStreamIdForIdentity("12D3KooWPub"));
  EXPECT_EQ(BroadcastMediaFrameContext("show", "live:1"), "broadcast-media|show|live:1");
}

TEST(BroadcastMediaTest, EveryShowGetsAFreshKeyAndJoinHandle) {
  const auto a = NewBroadcastMediaKey();
  const auto b = NewBroadcastMediaKey();
  EXPECT_EQ(a.size(), 32u);
  EXPECT_NE(a, b);
  const auto j1 = NewBroadcastJoinHandle("show");
  EXPECT_EQ(j1.rfind("live:show:", 0), 0u) << j1;
  EXPECT_NE(j1, NewBroadcastJoinHandle("show"));
}

} // namespace
} // namespace pbr
