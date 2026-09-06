#include "domain/messaging/SoftMigrateLogic.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(SoftMigrateLogicTest, SelectCallInitiatorEarliestJoinedAt) {
  std::vector<SoftMigrateJoinedPeer> peers = {
      {"account:B", 2000},
      {"account:A", 1000},
      {"account:C", 3000},
  };
  EXPECT_EQ(SelectCallInitiator(peers), "account:A");
}

TEST(SoftMigrateLogicTest, MidCallNonInitiatorInviterWaits) {
  // A is sticky initiator; B invites C and receives CallAccept — must not pick (V021/V022).
  SoftMigrateDecisionInput in;
  in.local_identity = "account:B";
  in.initiator_identity = "account:A";
  in.joined_identities = {"account:A", "account:B", "account:C"};
  in.sfu_hint_empty = true;
  in.trigger = SoftMigrateTrigger::RemoteAcceptObserved;
  EXPECT_EQ(DecideSoftMigrate(in), SoftMigrateAction::WaitForAttach);
}

TEST(SoftMigrateLogicTest, MidCallInitiatorPicksOnRosterOrAccept) {
  SoftMigrateDecisionInput in;
  in.local_identity = "account:A";
  in.initiator_identity = "account:A";
  in.joined_identities = {"account:A", "account:B", "account:C"};
  in.sfu_hint_empty = true;
  in.trigger = SoftMigrateTrigger::JoinedCountObserved;
  EXPECT_EQ(DecideSoftMigrate(in), SoftMigrateAction::PickHop);

  in.trigger = SoftMigrateTrigger::RemoteAcceptObserved;
  EXPECT_EQ(DecideSoftMigrate(in), SoftMigrateAction::PickHop);
}

TEST(SoftMigrateLogicTest, JoinerWithoutHintWaits) {
  SoftMigrateDecisionInput in;
  in.local_identity = "account:C";
  in.initiator_identity = "account:A";
  in.joined_identities = {"account:A", "account:B", "account:C"};
  in.sfu_hint_empty = true;
  in.trigger = SoftMigrateTrigger::LocalJoinedWithoutHint;
  EXPECT_EQ(DecideSoftMigrate(in), SoftMigrateAction::WaitForAttach);
}

TEST(SoftMigrateLogicTest, HintAlreadySetInitiatorWaits) {
  SoftMigrateDecisionInput in;
  in.local_identity = "account:A";
  in.initiator_identity = "account:A";
  in.joined_identities = {"account:A", "account:B", "account:C"};
  in.sfu_hint_empty = false;
  in.trigger = SoftMigrateTrigger::RemoteAcceptObserved;
  EXPECT_EQ(DecideSoftMigrate(in), SoftMigrateAction::WaitForAttach);
}

TEST(SoftMigrateLogicTest, IceRecoverOnlyCoordinatorPicks) {
  SoftMigrateDecisionInput in;
  in.joined_identities = {"account:A", "account:B", "account:C"};
  in.initiator_identity = "account:B";
  in.sfu_hint_empty = false;
  in.trigger = SoftMigrateTrigger::IceRecover;

  in.local_identity = "account:A"; // lex-min coordinator
  EXPECT_EQ(DecideSoftMigrate(in), SoftMigrateAction::PickHop);

  in.local_identity = "account:B";
  EXPECT_EQ(DecideSoftMigrate(in), SoftMigrateAction::WaitForAttach);
}

TEST(SoftMigrateLogicTest, AlreadyOnSfuIsNoOp) {
  SoftMigrateDecisionInput in;
  in.local_identity = "account:A";
  in.initiator_identity = "account:A";
  in.joined_identities = {"account:A", "account:B", "account:C"};
  in.sfu_hint_empty = true;
  in.trigger = SoftMigrateTrigger::RemoteAcceptObserved;
  in.already_on_sfu = true;
  EXPECT_EQ(DecideSoftMigrate(in), SoftMigrateAction::NoOp);
}

TEST(SoftMigrateLogicTest, FreshGroupCallInitiatorPicks) {
  SoftMigrateDecisionInput in;
  in.local_identity = "account:B";
  in.initiator_identity = "account:B";
  in.joined_identities = {"account:B", "account:A", "account:C"};
  in.sfu_hint_empty = true;
  in.trigger = SoftMigrateTrigger::RemoteAcceptObserved;
  EXPECT_EQ(DecideSoftMigrate(in), SoftMigrateAction::PickHop);
}

TEST(SoftMigrateLogicTest, BroadcastIsNoOp) {
  SoftMigrateDecisionInput in;
  in.local_identity = "account:A";
  in.initiator_identity = "account:A";
  in.joined_identities = {"account:A", "account:B", "account:C"};
  in.sfu_hint_empty = true;
  in.trigger = SoftMigrateTrigger::RemoteAcceptObserved;
  in.is_broadcast = true;
  EXPECT_EQ(DecideSoftMigrate(in), SoftMigrateAction::NoOp);
}

} // namespace
} // namespace pbr
