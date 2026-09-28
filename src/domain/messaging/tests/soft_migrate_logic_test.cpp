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

// V050: after the initiator leaves, every participant must name the same next owner from its own
// rows — equal stamps or missing stamps must not fall back to row order.
TEST(SoftMigrateLogicTest, SelectCallInitiatorIsIndependentOfRowOrder) {
  const std::vector<SoftMigrateJoinedPeer> tie_a = {{"account:C", 2000}, {"account:B", 2000}};
  const std::vector<SoftMigrateJoinedPeer> tie_b = {{"account:B", 2000}, {"account:C", 2000}};
  EXPECT_EQ(SelectCallInitiator(tie_a), "account:B");
  EXPECT_EQ(SelectCallInitiator(tie_b), "account:B");

  const std::vector<SoftMigrateJoinedPeer> unstamped_a = {{"account:C", std::nullopt}, {"account:B", std::nullopt}};
  const std::vector<SoftMigrateJoinedPeer> unstamped_b = {{"account:B", std::nullopt}, {"account:C", std::nullopt}};
  EXPECT_EQ(SelectCallInitiator(unstamped_a), "account:B");
  EXPECT_EQ(SelectCallInitiator(unstamped_b), "account:B");

  const std::vector<SoftMigrateJoinedPeer> mixed = {{"account:A", std::nullopt}, {"account:C", 3000}};
  EXPECT_EQ(SelectCallInitiator(mixed), "account:C") << "a stamped peer outranks an unstamped one";
}

// V038/V050: the join that triggered a SoftMigrate may be refused before the decision runs — a
// call back at 2 joined is a 1:1 and never picks a hop, whatever queued the migrate.
TEST(SoftMigrateLogicTest, NeverPicksAHopForTwoJoined) {
  SoftMigrateDecisionInput in;
  in.local_identity = "account:A";
  in.initiator_identity = "account:A";
  in.joined_identities = {"account:A", "account:B"};
  in.sfu_hint_empty = true;
  for (SoftMigrateTrigger trigger : {SoftMigrateTrigger::JoinedCountObserved, SoftMigrateTrigger::RemoteAcceptObserved}) {
    in.trigger = trigger;
    EXPECT_EQ(DecideSoftMigrate(in), SoftMigrateAction::NoOp) << static_cast<int>(trigger);
  }
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

} // namespace
} // namespace pbr
