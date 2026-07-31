#include "base/messaging/SoftMigrateLogic.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(SoftMigrateLogicTest, SelectCallInitiatorEarliestJoinedAt) {
  std::vector<SoftMigrateJoinedPeer> peers = {
      {"relay:B", 2000},
      {"relay:A", 1000},
      {"relay:C", 3000},
  };
  EXPECT_EQ(SelectCallInitiator(peers), "relay:A");
}

TEST(SoftMigrateLogicTest, MidCallNonInitiatorInviterWaits) {
  // A is sticky initiator; B invites C and receives CallAccept — must not pick (V021/V022).
  SoftMigrateDecisionInput in;
  in.local_identity = "relay:B";
  in.initiator_identity = "relay:A";
  in.joined_identities = {"relay:A", "relay:B", "relay:C"};
  in.sfu_hint_empty = true;
  in.trigger = SoftMigrateTrigger::RemoteAcceptObserved;
  EXPECT_EQ(DecideSoftMigrate(in), SoftMigrateAction::WaitForAttach);
}

TEST(SoftMigrateLogicTest, MidCallInitiatorPicksOnRosterOrAccept) {
  SoftMigrateDecisionInput in;
  in.local_identity = "relay:A";
  in.initiator_identity = "relay:A";
  in.joined_identities = {"relay:A", "relay:B", "relay:C"};
  in.sfu_hint_empty = true;
  in.trigger = SoftMigrateTrigger::JoinedCountObserved;
  EXPECT_EQ(DecideSoftMigrate(in), SoftMigrateAction::PickHop);

  in.trigger = SoftMigrateTrigger::RemoteAcceptObserved;
  EXPECT_EQ(DecideSoftMigrate(in), SoftMigrateAction::PickHop);
}

TEST(SoftMigrateLogicTest, JoinerWithoutHintWaits) {
  SoftMigrateDecisionInput in;
  in.local_identity = "relay:C";
  in.initiator_identity = "relay:A";
  in.joined_identities = {"relay:A", "relay:B", "relay:C"};
  in.sfu_hint_empty = true;
  in.trigger = SoftMigrateTrigger::LocalJoinedWithoutHint;
  EXPECT_EQ(DecideSoftMigrate(in), SoftMigrateAction::WaitForAttach);
}

TEST(SoftMigrateLogicTest, HintAlreadySetInitiatorWaits) {
  SoftMigrateDecisionInput in;
  in.local_identity = "relay:A";
  in.initiator_identity = "relay:A";
  in.joined_identities = {"relay:A", "relay:B", "relay:C"};
  in.sfu_hint_empty = false;
  in.trigger = SoftMigrateTrigger::RemoteAcceptObserved;
  EXPECT_EQ(DecideSoftMigrate(in), SoftMigrateAction::WaitForAttach);
}

TEST(SoftMigrateLogicTest, IceRecoverOnlyCoordinatorPicks) {
  SoftMigrateDecisionInput in;
  in.joined_identities = {"relay:A", "relay:B", "relay:C"};
  in.initiator_identity = "relay:B";
  in.sfu_hint_empty = false;
  in.trigger = SoftMigrateTrigger::IceRecover;

  in.local_identity = "relay:A"; // lex-min coordinator
  EXPECT_EQ(DecideSoftMigrate(in), SoftMigrateAction::PickHop);

  in.local_identity = "relay:B";
  EXPECT_EQ(DecideSoftMigrate(in), SoftMigrateAction::WaitForAttach);
}

TEST(SoftMigrateLogicTest, AlreadyOnSfuIsNoOp) {
  SoftMigrateDecisionInput in;
  in.local_identity = "relay:A";
  in.initiator_identity = "relay:A";
  in.joined_identities = {"relay:A", "relay:B", "relay:C"};
  in.sfu_hint_empty = true;
  in.trigger = SoftMigrateTrigger::RemoteAcceptObserved;
  in.already_on_sfu = true;
  EXPECT_EQ(DecideSoftMigrate(in), SoftMigrateAction::NoOp);
}

TEST(SoftMigrateLogicTest, FreshGroupCallInitiatorPicks) {
  SoftMigrateDecisionInput in;
  in.local_identity = "relay:B";
  in.initiator_identity = "relay:B";
  in.joined_identities = {"relay:B", "relay:A", "relay:C"};
  in.sfu_hint_empty = true;
  in.trigger = SoftMigrateTrigger::RemoteAcceptObserved;
  EXPECT_EQ(DecideSoftMigrate(in), SoftMigrateAction::PickHop);
}

} // namespace
} // namespace pbr
