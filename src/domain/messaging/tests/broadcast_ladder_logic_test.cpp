#include "domain/messaging/BroadcastLadderLogic.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(BroadcastLadderLogicTest, AdmitWhenFreeViewerSlot) {
  BroadcastLadderViewerInput in;
  in.free_viewer_slots = 2;
  in.whitelist_online_children = {"child-a"};
  const auto d = DecideBroadcastViewerAdmit(in);
  EXPECT_EQ(d.action, BroadcastLadderViewerAction::Admit);
  EXPECT_TRUE(d.redirect_peer_ids.empty());
}

TEST(BroadcastLadderLogicTest, RedirectWhenFullWithChildren) {
  BroadcastLadderViewerInput in;
  in.free_viewer_slots = 0;
  in.redirect_budget = 3;
  in.self_peer_id = "parent";
  in.whitelist_online_children = {"c1", "c2", "c3"};
  in.max_redirect_hints = 2;
  const auto d = DecideBroadcastViewerAdmit(in);
  EXPECT_EQ(d.action, BroadcastLadderViewerAction::Redirect);
  ASSERT_EQ(d.redirect_peer_ids.size(), 2u);
  EXPECT_EQ(d.redirect_peer_ids[0], "c1");
  EXPECT_EQ(d.redirect_peer_ids[1], "c2");
  EXPECT_EQ(d.redirect_budget_remaining, 2);
}

TEST(BroadcastLadderLogicTest, JitterRotatesRedirectOrder) {
  BroadcastLadderViewerInput in;
  in.free_viewer_slots = 0;
  in.redirect_budget = 2;
  in.whitelist_online_children = {"a", "b", "c"};
  in.jitter_unit = 0.5; // start at index 1 of 3
  in.max_redirect_hints = 3;
  const auto d = DecideBroadcastViewerAdmit(in);
  ASSERT_EQ(d.action, BroadcastLadderViewerAction::Redirect);
  ASSERT_EQ(d.redirect_peer_ids.size(), 3u);
  EXPECT_EQ(d.redirect_peer_ids[0], "b");
  EXPECT_EQ(d.redirect_peer_ids[1], "c");
  EXPECT_EQ(d.redirect_peer_ids[2], "a");
}

TEST(BroadcastLadderLogicTest, RefuseOnBudgetExhaustedOrLoopOrNoChildren) {
  BroadcastLadderViewerInput full;
  full.free_viewer_slots = 0;
  full.redirect_budget = 0;
  full.whitelist_online_children = {"c1"};
  EXPECT_EQ(DecideBroadcastViewerAdmit(full).action, BroadcastLadderViewerAction::Refuse);

  BroadcastLadderViewerInput no_kids;
  no_kids.free_viewer_slots = 0;
  no_kids.redirect_budget = 2;
  EXPECT_EQ(DecideBroadcastViewerAdmit(no_kids).action, BroadcastLadderViewerAction::Refuse);

  BroadcastLadderViewerInput loop;
  loop.free_viewer_slots = 0;
  loop.redirect_budget = 2;
  loop.self_peer_id = "me";
  loop.path_stamp = {"me"};
  loop.whitelist_online_children = {"c1"};
  EXPECT_EQ(DecideBroadcastViewerAdmit(loop).action, BroadcastLadderViewerAction::Refuse);
  EXPECT_EQ(DecideBroadcastViewerAdmit(loop).refuse_reason, "redirect path loop");
}

TEST(BroadcastLadderLogicTest, SlotWinAdmitsWhenFreeOrDemotesViewer) {
  BroadcastLadderSlotWinInput free_slot;
  free_slot.free_child_slots = 1;
  free_slot.candidate_on_whitelist = true;
  free_slot.new_relay_peer_id = "relay-1";
  auto d = DecideBroadcastSlotWin(free_slot);
  EXPECT_EQ(d.action, BroadcastLadderSlotWinAction::AdmitRelay);

  BroadcastLadderSlotWinInput demote;
  demote.free_child_slots = 0;
  demote.candidate_on_whitelist = true;
  demote.new_relay_peer_id = "relay-1";
  demote.demotable_viewer_peer_ids = {"viewer-a", "viewer-b"};
  demote.max_demotions = 1;
  d = DecideBroadcastSlotWin(demote);
  EXPECT_EQ(d.action, BroadcastLadderSlotWinAction::DemoteViewersAndAdmitRelay);
  ASSERT_EQ(d.demote_viewer_peer_ids.size(), 1u);
  EXPECT_EQ(d.demote_viewer_peer_ids[0], "viewer-a");
  EXPECT_EQ(d.demotion_redirect_target, "relay-1");
}

TEST(BroadcastLadderLogicTest, SlotWinRefusesOffWhitelistOrRateLimit) {
  BroadcastLadderSlotWinInput in;
  in.free_child_slots = 1;
  in.candidate_on_whitelist = false;
  in.new_relay_peer_id = "relay-1";
  EXPECT_EQ(DecideBroadcastSlotWin(in).action, BroadcastLadderSlotWinAction::Refuse);

  in.candidate_on_whitelist = true;
  in.slot_win_rate_limited = true;
  EXPECT_EQ(DecideBroadcastSlotWin(in).action, BroadcastLadderSlotWinAction::Refuse);
}

TEST(BroadcastLadderLogicTest, FilterOnlineWhitelistAndPrimaryHop) {
  const auto filtered = FilterOnlineWhitelist({"a", "b", "a", "", "c"}, {"c", "a", "z"}, 2);
  ASSERT_EQ(filtered.size(), 2u);
  EXPECT_EQ(filtered[0], "a");
  EXPECT_EQ(filtered[1], "c");

  EXPECT_EQ(PrimaryBroadcastHopPeerId("hop", {"l1a"}), "hop");
  EXPECT_EQ(PrimaryBroadcastHopPeerId("", {"", "l1b"}), "l1b");
  EXPECT_TRUE(PrimaryBroadcastHopPeerId("", {}).empty());
}

} // namespace
} // namespace pbr
