#include "lib/messaging/GroupRosterSync.h"
#include "lib/messaging/GroupMembershipCodec.h"
#include "lib/messaging/GroupMembershipApply.h"
#include "lib/messaging/GroupRosterSyncFamily.h"
#include "lib/messaging/GroupRosterStore.h"
#include "lib/messaging/SqliteThreadStore.h"
#include "lib/messaging/tests/TestRosterSigner.h"

#include "common/Utilities.h"

#include <filesystem>
#include <sqlite3.h>
#include <memory>
#include <gtest/gtest.h>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

using group_roster_sync::ReplyOutcome;
using group_roster_sync_codec::RosterReply;
using test::TestSigner;

constexpr const char kGroup[] = "group:hike";

/** One device's roster store: the group of Alice (owner), Bob, Carol and Dave, as a restore from the account locker left it. */
class GroupRosterSyncTest : public ::testing::Test {
protected:
  void SetUp() override {
    data_dir_ = std::filesystem::temp_directory_path() / ("pp_group_roster_sync_" + util::GenerateUuid());
    std::filesystem::remove_all(data_dir_);
    store_ = std::make_unique<SqliteThreadStore>(data_dir_.string());
    ASSERT_TRUE(store_->ListThreads());
    roster_ = std::make_unique<GroupRosterStore>(store_->ProfileDbPath());
    bob_ = "account:bob";
    carol_ = "account:carol";
    dave_ = "account:dave";
  }

  void TearDown() override {
    roster_.reset();
    store_.reset();
    std::filesystem::remove_all(data_dir_);
  }

  GroupRosterStatement Statement(uint64_t epoch, std::vector<std::string> members, TestSigner* owner = nullptr,
                                 std::vector<GroupOwnerTransfer> chain = {}) {
    TestSigner& signer = owner ? *owner : alice_;
    std::vector<GroupRosterStatementMember> rows{{signer.Id(), MemberRole::Owner, 1000}};
    for (const std::string& m : members) {
      rows.push_back({m, MemberRole::Member, 2000});
    }
    return group_roster_statement::Sign(signer, signer.Key(), kGroup, epoch, rows, std::move(chain)).value();
  }

  /** The roster as the account locker had it, holding `held`. */
  void Seed(const GroupRosterStatement& held) {
    GroupMetadata metadata;
    metadata.group_id = kGroup;
    metadata.owner_identity = held.owner_identity;
    metadata.title = "Weekend hike";
    metadata.roster_epoch = held.roster_epoch;
    ASSERT_TRUE(roster_->UpsertMetadata(metadata));
    ASSERT_TRUE(group_roster_sync::Adopt(*roster_, held));
  }

  GroupMetadata Metadata() { return roster_->LoadMetadata(kGroup).value().value(); }

  std::vector<std::string> Members() {
    std::vector<std::string> out;
    const std::vector<GroupRosterMember> members = roster_->ListMembers(kGroup).value();
    for (const GroupRosterMember& m : members) {
      out.push_back(m.member_identity);
    }
    std::sort(out.begin(), out.end());
    return out;
  }

  RosterReply ReplyWith(const std::string& request_id, std::optional<GroupRosterStatement> statement,
                        bool removed = false) {
    RosterReply reply;
    reply.group_id = kGroup;
    reply.request_id = request_id;
    reply.statement = std::move(statement);
    reply.removed = removed;
    return reply;
  }

  std::filesystem::path data_dir_;
  std::unique_ptr<SqliteThreadStore> store_;
  std::unique_ptr<GroupRosterStore> roster_;
  TestSigner alice_;
  std::string bob_, carol_, dave_;
};

TEST_F(GroupRosterSyncTest, RestoredMemberTakesTheNewerRosterAndStopsSendingToTheRemovedMember) {
  Seed(Statement(3, {bob_, carol_, dave_}));  // Carol's locker snapshot
  auto check = group_roster_sync::BeginCheck(*roster_, kGroup, carol_, 1);
  ASSERT_TRUE(check);
  std::vector<std::string> ask = check->ask;
  std::sort(ask.begin(), ask.end());
  std::vector<std::string> expected{alice_.Id(), bob_, dave_};
  std::sort(expected.begin(), expected.end());
  EXPECT_EQ(ask, expected) << "the owner and every member but us";
  EXPECT_FALSE(group_roster_sync::MaySend(Metadata())) << "no send while checking (a stale roster would leak)";

  // Alice removed Dave after the locker snapshot; Bob holds her newer statement.
  auto outcome = group_roster_sync::ApplyReply(*roster_, ReplyWith(check->request.request_id,
                                                                   Statement(4, {bob_, carol_})),
                                               carol_, bob_);
  ASSERT_TRUE(outcome);
  EXPECT_EQ(*outcome, ReplyOutcome::Active);
  std::vector<std::string> now{alice_.Id(), bob_, carol_};
  std::sort(now.begin(), now.end());
  EXPECT_EQ(Members(), now);
  EXPECT_EQ(Metadata().roster_epoch, 4u);
  EXPECT_TRUE(group_roster_sync::MaySend(Metadata()));
}

TEST_F(GroupRosterSyncTest, AnUnchangedRosterEndsTheCheck) {
  const GroupRosterStatement s3 = Statement(3, {bob_, carol_});
  Seed(s3);
  auto check = group_roster_sync::BeginCheck(*roster_, kGroup, carol_, 1).value();
  EXPECT_EQ(*group_roster_sync::ApplyReply(*roster_, ReplyWith(check.request.request_id, s3), carol_, bob_),
            ReplyOutcome::Active)
      << "same epoch: not dropped as stale";
}

TEST_F(GroupRosterSyncTest, AMemberRemovedSinceItsLockerSnapshotEndsRemoved) {
  Seed(Statement(3, {bob_, carol_, dave_}));  // Dave's locker snapshot
  auto check = group_roster_sync::BeginCheck(*roster_, kGroup, dave_, 1).value();
  EXPECT_EQ(*group_roster_sync::ApplyReply(*roster_, ReplyWith(check.request.request_id, Statement(4, {bob_, carol_})),
                                           dave_, bob_),
            ReplyOutcome::Removed);
  EXPECT_EQ(Metadata().sync_state, GroupSyncState::Removed);
  EXPECT_FALSE(group_roster_sync::MaySend(Metadata()));
}

TEST_F(GroupRosterSyncTest, RepliesToAnotherCheckOrUnverifiableStatementsAreIgnored) {
  Seed(Statement(3, {bob_, carol_}));
  auto check = group_roster_sync::BeginCheck(*roster_, kGroup, carol_, 1).value();
  EXPECT_EQ(*group_roster_sync::ApplyReply(*roster_, ReplyWith("an-old-request", Statement(4, {bob_, carol_})), carol_,
                                           bob_),
            ReplyOutcome::Ignored);
  TestSigner mallory;
  EXPECT_EQ(*group_roster_sync::ApplyReply(*roster_,
                                           ReplyWith(check.request.request_id, Statement(9, {carol_}, &mallory)),
                                           carol_, bob_),
            ReplyOutcome::Ignored)
      << "signed by someone who is not the owner";
  EXPECT_EQ(Metadata().sync_state, GroupSyncState::Checking);
  EXPECT_EQ(Members().size(), 3u);
}

TEST_F(GroupRosterSyncTest, TheOwnersLaterReplyWins) {
  const GroupRosterStatement s3 = Statement(3, {bob_, carol_, dave_});
  Seed(s3);
  auto check = group_roster_sync::BeginCheck(*roster_, kGroup, carol_, 1).value();
  // A member that is itself behind answers first.
  EXPECT_EQ(*group_roster_sync::ApplyReply(*roster_, ReplyWith(check.request.request_id, s3), carol_, dave_),
            ReplyOutcome::Active);
  // Then the owner, with the roster after Dave's removal.
  EXPECT_EQ(*group_roster_sync::ApplyReply(*roster_, ReplyWith(check.request.request_id, Statement(4, {bob_, carol_})),
                                           carol_, alice_.Id()),
            ReplyOutcome::Active);
  EXPECT_EQ(Members().size(), 3u) << "Dave is gone";
  EXPECT_EQ(Metadata().roster_epoch, 4u);
}

TEST_F(GroupRosterSyncTest, WithoutAStatementOnlyTheOwnerCanSayWeAreOut) {
  Seed(Statement(3, {bob_, carol_}));
  auto check = group_roster_sync::BeginCheck(*roster_, kGroup, carol_, 1).value();
  EXPECT_EQ(*group_roster_sync::ApplyReply(*roster_, ReplyWith(check.request.request_id, std::nullopt, true), carol_,
                                           bob_),
            ReplyOutcome::Ignored);
  EXPECT_EQ(*group_roster_sync::ApplyReply(*roster_, ReplyWith(check.request.request_id, std::nullopt, true), carol_,
                                           alice_.Id()),
            ReplyOutcome::Removed);
}

TEST_F(GroupRosterSyncTest, AStatementAfterAMissedLeaveCatchesTheRosterUp) {
  Seed(Statement(3, {bob_, carol_, dave_}));
  // Dave left; we missed his member_left; Alice re-signs.
  EXPECT_TRUE(*group_roster_sync::KeepAndCatchUp(*roster_, Statement(4, {bob_, carol_})));
  EXPECT_EQ(Members().size(), 3u);
  EXPECT_EQ(Metadata().roster_epoch, 4u);
  // An older statement afterwards changes nothing.
  EXPECT_FALSE(*group_roster_sync::KeepAndCatchUp(*roster_, Statement(3, {bob_, carol_, dave_})));
  EXPECT_EQ(Members().size(), 3u);
}

TEST_F(GroupRosterSyncTest, AStatementFromTheNewOwnerIsKeptThroughTheChain) {
  Seed(Statement(3, {bob_, carol_}));
  TestSigner bob;
  // Alice hands the group to Bob; as members do, we apply the transfer (Bob owns it now).
  auto handed = group_roster_statement::SignTransfer(alice_, alice_.Key(), kGroup, bob.Id(), 4).value();
  GroupMetadata metadata = Metadata();
  metadata.owner_identity = bob.Id();
  ASSERT_TRUE(roster_->UpsertMetadata(metadata));
  EXPECT_TRUE(*group_roster_sync::KeepAndCatchUp(*roster_, Statement(5, {carol_}, &bob, {handed})));
  EXPECT_EQ(Metadata().owner_identity, bob.Id());
}

TEST_F(GroupRosterSyncTest, MembersAnswerWithTheirStatementAndWhetherTheAskerIsStillIn) {
  const GroupRosterStatement s4 = Statement(4, {bob_, carol_});
  Seed(s4);
  group_roster_sync_codec::RosterRequest request{kGroup, "r1", 3};
  auto for_carol = group_roster_sync::Answer(*roster_, request, carol_).value();
  ASSERT_TRUE(for_carol);
  EXPECT_FALSE(for_carol->removed);
  ASSERT_TRUE(for_carol->statement);
  EXPECT_EQ(for_carol->statement->roster_epoch, 4u);
  EXPECT_TRUE(group_roster_sync::Answer(*roster_, request, dave_).value()->removed) << "no longer on our roster";

  // A member still checking its own roster does not tell anyone they are out.
  ASSERT_TRUE(group_roster_sync::BeginCheck(*roster_, kGroup, bob_, 1));
  EXPECT_FALSE(group_roster_sync::Answer(*roster_, request, dave_).value()->removed);

  group_roster_sync_codec::RosterRequest unknown{"group:other", "r2", 1};
  EXPECT_FALSE(group_roster_sync::Answer(*roster_, unknown, carol_).value()) << "not a group we know";
}

TEST_F(GroupRosterSyncTest, CodecRoundTripsTheControlsAndTheEventFields) {
  const GroupRosterStatement s4 = Statement(4, {bob_, carol_});
  auto request = group_roster_sync_codec::DecodeRequest(
      group_roster_sync_codec::EncodeRequest({kGroup, "r1", 3}));
  ASSERT_TRUE(request);
  EXPECT_EQ(request->request_id, "r1");
  EXPECT_EQ(request->roster_epoch, 3u);

  auto reply = group_roster_sync_codec::DecodeReply(group_roster_sync_codec::EncodeReply(ReplyWith("r1", s4, true)));
  ASSERT_TRUE(reply);
  EXPECT_TRUE(reply->removed);
  ASSERT_TRUE(reply->statement);
  EXPECT_TRUE(group_roster_statement::Verify(*reply->statement, alice_.Id()));

  auto statement = group_roster_sync_codec::DecodeStatement(group_roster_sync_codec::EncodeStatement(s4));
  ASSERT_TRUE(statement);
  EXPECT_EQ(statement->roster_epoch, 4u);

  // A roster event carries the owner's statement (and a transfer its signed hand-over) next to its own fields.
  const std::string removed = R"({"group_id":"group:hike","member_identity":"account:dave","roster_epoch":4})";
  auto with = group_roster_sync_codec::WithStatement(removed, s4);
  ASSERT_TRUE(with);
  ASSERT_TRUE(group_roster_sync_codec::StatementIn(*with));
  EXPECT_EQ(group_roster_sync_codec::StatementIn(*with)->roster_epoch, 4u);
  EXPECT_FALSE(group_roster_sync_codec::StatementIn(removed)) << "an event from an app before G012";
  TestSigner bob;
  auto handed = group_roster_statement::SignTransfer(alice_, alice_.Key(), kGroup, bob.Id(), 5).value();
  auto transferred = group_roster_sync_codec::WithTransfer(removed, handed);
  ASSERT_TRUE(transferred);
  ASSERT_TRUE(group_roster_sync_codec::TransferIn(*transferred));
  EXPECT_EQ(group_roster_sync_codec::TransferIn(*transferred)->to_identity, bob.Id());
}

TEST_F(GroupRosterSyncTest, TheCheckRidesTheControlLane) {
  Seed(Statement(3, {bob_, carol_, dave_}));
  std::vector<std::pair<std::string, std::string>> answered;  // request id, asker
  SystemControlFamily family = MakeGroupRosterSyncFamily(
      *roster_, [this]() -> std::optional<std::string> { return carol_; },
      [&answered](group_roster_sync_codec::RosterRequest request, std::string asker) {
        answered.emplace_back(request.request_id, asker);
      });
  auto control = [](GroupMembershipControlType type, const std::string& detail) {
    return GroupMembershipCodec::BuildSystemMessage(std::string(), type, "", detail, "").value();
  };

  // Bookkeeping between two devices: on the lane (an account copy with the sender's card, never stored).
  ThreadMessage request = control(GroupMembershipControlType::GroupRosterRequest,
                                  group_roster_sync_codec::EncodeRequest({kGroup, "r1", 3}));
  ASSERT_TRUE(family.owns(request));
  EXPECT_TRUE(family.on_lane(request));
  ThreadMessage statement = control(GroupMembershipControlType::GroupRosterStatement,
                                    group_roster_sync_codec::EncodeStatement(Statement(4, {bob_, carol_})));
  EXPECT_FALSE(family.owns(statement)) << "the owner's statement stays a roster event in the chat stream";

  // A member's check is handed on to be answered; a stranger's is not ours to answer.
  ASSERT_TRUE(family.apply(request, SystemControlContext{.sender_identity = bob_}));
  ASSERT_TRUE(family.apply(request, SystemControlContext{.sender_identity = carol_}));
  ASSERT_EQ(answered.size(), 1u);
  EXPECT_EQ(answered[0], std::make_pair(std::string("r1"), bob_));

  // A reply to our open check settles it.
  auto check = group_roster_sync::BeginCheck(*roster_, kGroup, carol_, 1).value();
  ThreadMessage reply = control(GroupMembershipControlType::GroupRosterReply,
                                group_roster_sync_codec::EncodeReply(
                                    ReplyWith(check.request.request_id, Statement(4, {bob_, carol_}))));
  EXPECT_TRUE(family.on_lane(reply));
  ASSERT_TRUE(family.apply(reply, SystemControlContext{.sender_identity = bob_}));
  EXPECT_EQ(Metadata().sync_state, GroupSyncState::Active);
  EXPECT_EQ(Metadata().roster_epoch, 4u);
}

// G016: a group the owner ended is over for every roster rule.
TEST_F(GroupRosterSyncTest, ASoloOwnerEndsItsGroupWithNothingToSend) {
  Seed(Statement(2, {}));
  ASSERT_EQ(Members(), (std::vector<std::string>{alice_.Id()}));
  auto ended = EndGroupInRoster(*roster_, kGroup, alice_.Id(), 3);
  ASSERT_TRUE(ended);
  EXPECT_TRUE(ended->applied);
  EXPECT_TRUE(Members().empty());
  EXPECT_EQ(Metadata().sync_state, GroupSyncState::Dissolved);
  EXPECT_EQ(GroupSyncStateFromString(GroupSyncStateToString(GroupSyncState::Dissolved)), GroupSyncState::Dissolved);
  EXPECT_EQ(Metadata().roster_epoch, 3u);
  EXPECT_TRUE(Metadata().roster_statement_json.empty()) << "an ended group hands no roster on";
  EXPECT_FALSE(group_roster_sync::MaySend(Metadata()));

  auto again = EndGroupInRoster(*roster_, kGroup, alice_.Id(), 9);
  ASSERT_TRUE(again);
  EXPECT_FALSE(again->applied) << "a repeat is a no-op";
  EXPECT_EQ(Metadata().roster_epoch, 3u);
}

TEST_F(GroupRosterSyncTest, ADissolvedGroupChecksNothingAnswersNothingAndKeepsNoRoster) {
  Seed(Statement(3, {bob_, carol_}));
  ASSERT_TRUE(EndGroupInRoster(*roster_, kGroup, alice_.Id(), 4));

  auto check = group_roster_sync::BeginCheck(*roster_, kGroup, carol_, 1);
  ASSERT_TRUE(check);
  EXPECT_TRUE(check->ask.empty()) << "asks no one";
  EXPECT_EQ(Metadata().sync_state, GroupSyncState::Dissolved) << "and does not reopen the group";
  EXPECT_TRUE(Metadata().sync_request_id.empty());

  group_roster_sync_codec::RosterRequest request{kGroup, "r1", 3};
  auto answer = group_roster_sync::Answer(*roster_, request, bob_);
  ASSERT_TRUE(answer);
  ASSERT_TRUE(answer->has_value());
  EXPECT_FALSE((*answer)->statement) << "no dissolve was held here, so there is none to hand on";
  EXPECT_FALSE((*answer)->removed);

  // A reply to a check from before it ended changes nothing; neither does a stale roster.
  Seed(Statement(3, {bob_, carol_}));
  GroupMetadata open = Metadata();
  open.sync_state = GroupSyncState::Dissolved;
  open.sync_request_id = "r-old";
  ASSERT_TRUE(roster_->UpsertMetadata(open));
  auto outcome = group_roster_sync::ApplyReply(*roster_, ReplyWith("r-old", Statement(5, {bob_, carol_})), carol_, bob_);
  ASSERT_TRUE(outcome);
  EXPECT_EQ(*outcome, ReplyOutcome::Ignored);
  EXPECT_EQ(Metadata().sync_state, GroupSyncState::Dissolved);
}

TEST_F(GroupRosterSyncTest, ADissolveIsNeverKeptAsTheRoster) {
  Seed(Statement(3, {bob_, carol_}));
  std::vector<GroupRosterStatementMember> rows{{alice_.Id(), MemberRole::Owner, 1000}, {bob_, MemberRole::Member, 2000}};
  const GroupRosterStatement dissolve =
      group_roster_statement::Sign(alice_, alice_.Key(), kGroup, 9, rows, {}, /*dissolved=*/true).value();
  ASSERT_TRUE(group_roster_statement::Verify(dissolve, alice_.Id())) << "it verifies as what it is";
  EXPECT_FALSE(group_roster_sync::KeepStatement(*roster_, dissolve));
  EXPECT_FALSE(group_roster_sync::KeepAndCatchUp(*roster_, dissolve));
  EXPECT_EQ(Metadata().roster_epoch, 3u);
  EXPECT_EQ(Members().size(), 3u);
}

GroupRosterStatement DissolveOf(TestSigner& owner, const std::string& group, uint64_t epoch,
                                const std::vector<std::string>& members, std::vector<GroupOwnerTransfer> chain = {}) {
  std::vector<GroupRosterStatementMember> rows{{owner.Id(), MemberRole::Owner, 1000}};
  for (const std::string& m : members) {
    rows.push_back({m, MemberRole::Member, 2000});
  }
  return group_roster_statement::Sign(owner, owner.Key(), group, epoch, rows, std::move(chain), true).value();
}

// G016: a dissolved group answers a member check with the owner's dissolve; the asker, a restored member, ends its group.
TEST_F(GroupRosterSyncTest, ADissolvedGroupAnswersWithTheDissolveAndTheAskerEndsToo) {
  Seed(Statement(3, {bob_, carol_}));
  const GroupRosterStatement dissolve = DissolveOf(alice_, kGroup, 4, {bob_, carol_});
  ASSERT_TRUE(EndGroupInRoster(*roster_, kGroup, alice_.Id(), 4, DumpJson(group_roster_statement::ToJson(dissolve))));

  auto answer = group_roster_sync::Answer(*roster_, {kGroup, "r1", 3}, carol_);
  ASSERT_TRUE(answer && answer->has_value());
  ASSERT_TRUE((*answer)->statement);
  EXPECT_TRUE((*answer)->statement->dissolved);
  EXPECT_FALSE((*answer)->removed);

  // The asker restored the group from an older locker snapshot and is checking its members.
  Seed(Statement(3, {bob_, carol_}));
  auto check = group_roster_sync::BeginCheck(*roster_, kGroup, carol_, 1);
  ASSERT_TRUE(check);
  ASSERT_EQ(Metadata().sync_state, GroupSyncState::Checking);
  GroupDissolvedApplied ended;
  auto outcome = group_roster_sync::ApplyReply(*roster_, ReplyWith(check->request.request_id, (*answer)->statement),
                                               carol_, bob_, &ended);
  ASSERT_TRUE(outcome);
  EXPECT_EQ(*outcome, ReplyOutcome::Dissolved) << "relayed by a member, it holds: the owner's signature carries it";
  EXPECT_TRUE(ended.applied);
  EXPECT_EQ(Metadata().sync_state, GroupSyncState::Dissolved) << "no longer checking";
  EXPECT_TRUE(Members().empty());

  // The owner itself answers with a fresh signature over what it held; the statement is the dissolve either way.
  auto again = group_roster_sync::ApplyReply(*roster_, ReplyWith(check->request.request_id, dissolve), carol_, alice_.Id());
  ASSERT_TRUE(again);
  EXPECT_EQ(*again, ReplyOutcome::Ignored) << "a group already over takes nothing more";
}

// G016 authority: a dissolve is judged like any owner statement — the owner we know (or its successor), not older
// than our roster, and not by an owner who had already handed the group on.
TEST_F(GroupRosterSyncTest, ADissolveNeedsTheCurrentOwnerAtAnEpochNotOlderThanOurs) {
  TestSigner a, b;
  const GroupOwnerTransfer a_to_b = group_roster_statement::SignTransfer(a, a.Key(), kGroup, b.Id(), 5).value();
  const GroupOwnerTransfer b_to_a = group_roster_statement::SignTransfer(b, b.Key(), kGroup, a.Id(), 7).value();
  // We know B as the owner since epoch 5 (A handed the group over).
  Seed(Statement(5, {bob_, carol_}, &b, {a_to_b}));

  // A, a former owner, dissolves the group at epoch 6: on its own, or with the chain it signed — both rejected.
  EXPECT_FALSE(ApplyDissolvedStatement(*roster_, DissolveOf(a, kGroup, 6, {bob_}), bob_));
  EXPECT_FALSE(ApplyDissolvedStatement(*roster_, DissolveOf(a, kGroup, 6, {bob_}, {a_to_b}), bob_)) << "a stale chain";
  EXPECT_EQ(Metadata().sync_state, GroupSyncState::Active);

  // A copy older than the roster we hold is rejected, whoever signed it...
  EXPECT_FALSE(ApplyDissolvedStatement(*roster_, DissolveOf(b, kGroup, 4, {bob_}, {a_to_b}), bob_)) << "a lower epoch";
  EXPECT_EQ(Metadata().sync_state, GroupSyncState::Active);

  // ...and A, who got the group back from B at epoch 7 (A to B to A), may dissolve it at epoch 8.
  auto ended = ApplyDissolvedStatement(*roster_, DissolveOf(a, kGroup, 8, {bob_}, {a_to_b, b_to_a}), bob_);
  ASSERT_TRUE(ended) << ended.error().message;
  EXPECT_TRUE(ended->applied);
  EXPECT_EQ(Metadata().sync_state, GroupSyncState::Dissolved);
  EXPECT_EQ(Metadata().owner_identity, a.Id());
}

TEST_F(GroupRosterSyncTest, AFormerOwnersDissolveLosesToTheChainWeHold) {
  TestSigner a, b;
  const GroupOwnerTransfer a_to_b = group_roster_statement::SignTransfer(a, a.Key(), kGroup, b.Id(), 5).value();
  // This device still has A as the owner in its metadata but holds B's newer statement (kept, not yet adopted).
  Seed(Statement(3, {bob_, carol_}, &a));
  GroupMetadata metadata = Metadata();
  metadata.roster_statement_json = DumpJson(group_roster_statement::ToJson(Statement(5, {bob_, carol_}, &b, {a_to_b})));
  ASSERT_TRUE(roster_->UpsertMetadata(metadata));

  auto stale = ApplyDissolvedStatement(*roster_, DissolveOf(a, kGroup, 6, {bob_}), bob_);
  ASSERT_FALSE(stale) << "A verifies as the owner we know, but B's chain shows A handed the group on at 5";
  EXPECT_EQ(Metadata().sync_state, GroupSyncState::Active);
}

// G016: ending a group is one transaction — the roster, the invites and what is still to send land together.
TEST_F(GroupRosterSyncTest, EndingAGroupIsAllOrNothing) {
  Seed(Statement(3, {bob_, carol_}));
  PendingGroupSend send{kGroup, bob_, "group_dissolved", "{}", "The group owner dissolved this group"};

  // A failure part-way (here a trigger on the pending sends) leaves the group exactly as it was.
  sqlite3* db = nullptr;
  ASSERT_EQ(sqlite3_open(store_->ProfileDbPath().c_str(), &db), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(db,
                         "CREATE TRIGGER fail_send BEFORE INSERT ON group_pending_sends BEGIN "
                         "SELECT RAISE(ABORT, 'boom'); END;",
                         nullptr, nullptr, nullptr),
            SQLITE_OK);
  EXPECT_FALSE(EndGroupInRoster(*roster_, kGroup, alice_.Id(), 4, "stmt", {send}));
  EXPECT_EQ(Metadata().sync_state, GroupSyncState::Active);
  EXPECT_EQ(Members().size(), 3u);
  EXPECT_TRUE(roster_->ListPendingSends().value().empty());

  ASSERT_EQ(sqlite3_exec(db, "DROP TRIGGER fail_send;", nullptr, nullptr, nullptr), SQLITE_OK);
  sqlite3_close(db);
  auto ended = EndGroupInRoster(*roster_, kGroup, alice_.Id(), 4, "stmt", {send});
  ASSERT_TRUE(ended);
  EXPECT_TRUE(ended->applied);
  EXPECT_EQ(Metadata().sync_state, GroupSyncState::Dissolved);
  EXPECT_TRUE(Members().empty());
  EXPECT_EQ(Metadata().roster_statement_json, "stmt");

  // What is still to send survives (a restart reads it back) until its copy is in the outbox.
  GroupRosterStore reopened(store_->ProfileDbPath());
  auto pending = reopened.ListPendingSends(kGroup);
  ASSERT_TRUE(pending);
  ASSERT_EQ(pending->size(), 1u);
  EXPECT_EQ(pending->front().recipient, bob_);
  EXPECT_EQ(pending->front().control_type, "group_dissolved");
  ASSERT_TRUE(reopened.DeletePendingSend(pending->front()));
  EXPECT_TRUE(reopened.ListPendingSends().value().empty());

  // A second ending (a member's redelivery) changes nothing and records nothing.
  auto again = EndGroupInRoster(*roster_, kGroup, alice_.Id(), 9, "other", {send});
  ASSERT_TRUE(again);
  EXPECT_FALSE(again->applied);
  EXPECT_TRUE(roster_->ListPendingSends().value().empty());
}

// G016: the owner's other device says the group is dissolved; no one else's word counts.
TEST_F(GroupRosterSyncTest, AnOwnersOtherDeviceEndsTheGroupAndNoOneElseCan) {
  Seed(Statement(3, {bob_, carol_}));
  std::vector<GroupDissolvedEnd> heard;
  auto family = MakeGroupRosterSyncFamily(
      *roster_, [&]() -> std::optional<std::string> { return alice_.Id(); }, nullptr,
      [&](GroupDissolvedEnd end) { heard.push_back(std::move(end)); });
  const auto notice = [&](const GroupRosterStatement& statement) {
    return GroupMembershipCodec::BuildSystemMessage(std::string(), GroupMembershipControlType::GroupDissolvedSibling,
                                                    "Group dissolved", group_roster_sync_codec::EncodeStatement(statement),
                                                    alice_.Id())
        .value();
  };
  ThreadMessage message = notice(DissolveOf(alice_, kGroup, 4, {bob_, carol_}));
  ASSERT_TRUE(family.owns(message));
  EXPECT_TRUE(family.on_lane(message)) << "a lane notice, never a chat row";
  ASSERT_TRUE(family.apply(message, SystemControlContext{.sender_identity = bob_}));
  EXPECT_TRUE(heard.empty()) << "a member's word is not our own device's";
  EXPECT_EQ(Metadata().sync_state, GroupSyncState::Active);

  ASSERT_TRUE(family.apply(message, SystemControlContext{.sender_identity = alice_.Id()}));
  ASSERT_EQ(heard.size(), 1u);
  EXPECT_EQ(heard[0].group_id, kGroup);
  EXPECT_EQ(Metadata().sync_state, GroupSyncState::Dissolved);
  ASSERT_TRUE(family.apply(message, SystemControlContext{.sender_identity = alice_.Id()}));
  EXPECT_EQ(heard.size(), 1u) << "a repeat is heard once";
}

} // namespace
} // namespace pbr
