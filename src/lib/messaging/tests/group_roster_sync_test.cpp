#include "lib/messaging/GroupRosterSync.h"
#include "lib/messaging/GroupMembershipCodec.h"
#include "lib/messaging/GroupRosterSyncFamily.h"
#include "lib/messaging/GroupRosterStore.h"
#include "lib/messaging/SqliteThreadStore.h"
#include "lib/messaging/tests/TestRosterSigner.h"

#include "common/Utilities.h"

#include <filesystem>
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

} // namespace
} // namespace pbr
