#include "domain/messaging/GroupMembershipApply.h"
#include "domain/messaging/GroupRosterStore.h"
#include "domain/messaging/SqliteThreadStore.h"

#include "common/Utilities.h"

#include <filesystem>
#include <gtest/gtest.h>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

class GroupRosterInviteApplyTest : public ::testing::Test {
protected:
  void SetUp() override {
    data_dir_ = std::filesystem::temp_directory_path() /
                ("pp_group_roster_apply_" + util::GenerateUuid());
    std::filesystem::remove_all(data_dir_);
    store_ = std::make_unique<SqliteThreadStore>(data_dir_.string());
    // Ensure threads/ + profile.db exist before GroupRosterStore opens the path.
    ASSERT_TRUE(store_->ListThreads());
    roster_ = std::make_unique<GroupRosterStore>(store_->ProfileDbPath());
  }

  void TearDown() override {
    roster_.reset();
    store_.reset();
    std::filesystem::remove_all(data_dir_);
  }

  PendingGroupInvite MakePending() {
    PendingGroupInvite pending;
    pending.invite_nonce = "nonce-1";
    pending.group_id = "group:hike";
    pending.group_title = "Weekend hike";
    pending.inviter_identity = "relay:alice";
    pending.invitee_identity = "relay:bob";
    pending.roster_epoch = 1;
    pending.status = InviteStatus::Pending;
    pending.created_at = util::NowUnixMs();
    pending.outbound = true;  // alice (this device) issued it
    return pending;
  }

  void SeedOwnedGroup(uint64_t epoch = 1) {
    GroupMetadata metadata;
    metadata.group_id = "group:hike";
    metadata.owner_identity = "relay:alice";
    metadata.title = "Weekend hike";
    metadata.roster_epoch = epoch;
    ASSERT_TRUE(roster_->UpsertMetadata(metadata));
  }

  Roe<bool> Accept(const std::string& nonce, const std::string& sender, const std::string& group = "group:hike") {
    return ApplyInviteAcceptToRoster(*roster_, nonce, sender, group, "relay:alice", util::NowUnixMs());
  }

  bool IsMember(const std::string& identity) {
    auto member = roster_->IsMember("group:hike", identity);
    return member && *member;
  }

  std::filesystem::path data_dir_;
  std::unique_ptr<SqliteThreadStore> store_;
  std::unique_ptr<GroupRosterStore> roster_;
};

TEST_F(GroupRosterInviteApplyTest, AcceptAddsMemberAndBumpsEpoch) {
  GroupMetadata metadata;
  metadata.group_id = "group:hike";
  metadata.owner_identity = "relay:alice";
  metadata.title = "Weekend hike";
  metadata.roster_epoch = 1;
  ASSERT_TRUE(roster_->UpsertMetadata(metadata));

  GroupRosterMember owner;
  owner.member_identity = "relay:alice";
  owner.role = MemberRole::Owner;
  owner.joined_at = util::NowUnixMs();
  ASSERT_TRUE(roster_->UpsertMember("group:hike", owner));
  ASSERT_TRUE(roster_->UpsertPendingInvite(MakePending()));

  auto accepted = Accept("nonce-1", "relay:bob");
  ASSERT_TRUE(accepted) << accepted.error().message;
  EXPECT_TRUE(*accepted);

  auto members = roster_->ListMembers("group:hike");
  ASSERT_TRUE(members);
  ASSERT_EQ(members->size(), 2u);

  auto loaded = roster_->LoadPendingInvite("nonce-1");
  ASSERT_TRUE(loaded && loaded->has_value());
  EXPECT_EQ((*loaded)->status, InviteStatus::Accepted);
  EXPECT_EQ((*loaded)->group_title, "Weekend hike");

  auto meta = roster_->LoadMetadata("group:hike");
  ASSERT_TRUE(meta && meta->has_value());
  EXPECT_EQ((*meta)->roster_epoch, 2u);
}

TEST_F(GroupRosterInviteApplyTest, AcceptRejectsWrongSender) {
  SeedOwnedGroup();
  ASSERT_TRUE(roster_->UpsertPendingInvite(MakePending()));
  auto result = Accept("nonce-1", "relay:eve");
  ASSERT_FALSE(result);
}

TEST_F(GroupRosterInviteApplyTest, DeclineUpdatesStatusWithoutRemovingMembers) {
  GroupMetadata metadata;
  metadata.group_id = "group:hike";
  metadata.owner_identity = "relay:alice";
  metadata.title = "Weekend hike";
  metadata.roster_epoch = 1;
  ASSERT_TRUE(roster_->UpsertMetadata(metadata));
  // Decline must not assume invitee was ever on the active roster (pending ≠ member).
  ASSERT_TRUE(roster_->UpsertPendingInvite(MakePending()));

  ASSERT_TRUE(ApplyInviteDeclineToRoster(*roster_, "nonce-1", "relay:bob", "group:hike"));
  auto members = roster_->ListMembers("group:hike");
  ASSERT_TRUE(members);
  EXPECT_TRUE(members->empty());
  auto loaded = roster_->LoadPendingInvite("nonce-1");
  ASSERT_TRUE(loaded && loaded->has_value());
  EXPECT_EQ((*loaded)->status, InviteStatus::Declined);
}

TEST_F(GroupRosterInviteApplyTest, MemberJoinedAddsMemberAndBumpsEpoch) {
  GroupMetadata metadata;
  metadata.group_id = "group:hike";
  metadata.owner_identity = "relay:alice";
  metadata.title = "Weekend hike";
  metadata.roster_epoch = 1;
  ASSERT_TRUE(roster_->UpsertMetadata(metadata));
  GroupRosterMember alice;
  alice.member_identity = "relay:alice";
  alice.role = MemberRole::Owner;
  alice.joined_at = util::NowUnixMs();
  ASSERT_TRUE(roster_->UpsertMember("group:hike", alice));

  GroupMembershipCodec::MemberJoinedPayload payload;
  payload.group_id = "group:hike";
  payload.member_identity = "relay:bob";
  payload.role = MemberRole::Member;
  payload.roster_epoch = 2;
  ASSERT_TRUE(ApplyMemberJoinedToRoster(*roster_, payload, "relay:alice"));

  auto members = roster_->ListMembers("group:hike");
  ASSERT_TRUE(members);
  ASSERT_EQ(members->size(), 2u);
  auto meta = roster_->LoadMetadata("group:hike");
  ASSERT_TRUE(meta && meta->has_value());
  EXPECT_EQ((*meta)->roster_epoch, 2u);
}

TEST_F(GroupRosterInviteApplyTest, MemberJoinedSnapshotBackfillsPriorMembers) {
  // Late joiner (carol) only seeded owner+self locally; owner publish includes full roster.
  GroupMetadata metadata;
  metadata.group_id = "group:hike";
  metadata.owner_identity = "relay:alice";
  metadata.title = "Weekend hike";
  metadata.roster_epoch = 1;
  ASSERT_TRUE(roster_->UpsertMetadata(metadata));
  GroupRosterMember alice;
  alice.member_identity = "relay:alice";
  alice.role = MemberRole::Owner;
  alice.joined_at = util::NowUnixMs();
  ASSERT_TRUE(roster_->UpsertMember("group:hike", alice));
  GroupRosterMember carol_self;
  carol_self.member_identity = "relay:carol";
  carol_self.role = MemberRole::Member;
  carol_self.joined_at = util::NowUnixMs();
  ASSERT_TRUE(roster_->UpsertMember("group:hike", carol_self));

  GroupMembershipCodec::MemberJoinedPayload payload;
  payload.group_id = "group:hike";
  payload.member_identity = "relay:carol";
  payload.role = MemberRole::Member;
  payload.roster_epoch = 3;
  payload.members = {
      {"relay:alice", MemberRole::Owner},
      {"relay:bob", MemberRole::Member},
      {"relay:carol", MemberRole::Member},
  };
  ASSERT_TRUE(ApplyMemberJoinedToRoster(*roster_, payload, "relay:alice"));

  auto members = roster_->ListMembers("group:hike");
  ASSERT_TRUE(members);
  ASSERT_EQ(members->size(), 3u);
  bool saw_bob = false;
  for (const GroupRosterMember& row : *members) {
    if (row.member_identity == "relay:bob") {
      saw_bob = true;
    }
  }
  EXPECT_TRUE(saw_bob);
  auto meta = roster_->LoadMetadata("group:hike");
  ASSERT_TRUE(meta && meta->has_value());
  EXPECT_EQ((*meta)->roster_epoch, 3u);

  auto encoded = GroupMembershipCodec::EncodeMemberJoined(payload);
  ASSERT_TRUE(encoded);
  auto decoded = GroupMembershipCodec::DecodeMemberJoined(*encoded);
  ASSERT_TRUE(decoded);
  ASSERT_EQ(decoded->members.size(), 3u);
}

TEST_F(GroupRosterInviteApplyTest, MemberJoinedRejectsNonOwnerAndStaleEpoch) {
  GroupMetadata metadata;
  metadata.group_id = "group:hike";
  metadata.owner_identity = "relay:alice";
  metadata.title = "Weekend hike";
  metadata.roster_epoch = 2;
  ASSERT_TRUE(roster_->UpsertMetadata(metadata));

  GroupMembershipCodec::MemberJoinedPayload non_owner;
  non_owner.group_id = "group:hike";
  non_owner.member_identity = "relay:bob";
  non_owner.role = MemberRole::Member;
  non_owner.roster_epoch = 3;
  EXPECT_FALSE(ApplyMemberJoinedToRoster(*roster_, non_owner, "relay:eve"));

  GroupMembershipCodec::MemberJoinedPayload stale;
  stale.group_id = "group:hike";
  stale.member_identity = "relay:bob";
  stale.role = MemberRole::Member;
  stale.roster_epoch = 2;
  EXPECT_FALSE(ApplyMemberJoinedToRoster(*roster_, stale, "relay:alice"));
}

TEST_F(GroupRosterInviteApplyTest, PendingSnapshotRoundTrip) {
  ASSERT_TRUE(roster_->UpsertPendingInvite(MakePending()));
  auto loaded = roster_->LoadPendingInvite("nonce-1");
  ASSERT_TRUE(loaded && loaded->has_value());
  EXPECT_EQ((*loaded)->group_title, "Weekend hike");
  EXPECT_EQ((*loaded)->roster_epoch, 1u);
}

TEST_F(GroupRosterInviteApplyTest, InviteeAcceptSeedsOwnerAndSelfForFanout) {
  // Mirrors GroupMembershipWorkflow::AcceptInvite roster seeding on the invitee device.
  ASSERT_TRUE(roster_->UpsertPendingInvite(MakePending()));
  auto pending = roster_->LoadPendingInvite("nonce-1");
  ASSERT_TRUE(pending && pending->has_value());

  GroupMetadata metadata;
  metadata.group_id = (*pending)->group_id;
  metadata.owner_identity = (*pending)->inviter_identity;
  metadata.title = (*pending)->group_title;
  metadata.roster_epoch = (*pending)->roster_epoch;
  ASSERT_TRUE(roster_->UpsertMetadata(metadata));

  GroupRosterMember owner;
  owner.member_identity = (*pending)->inviter_identity;
  owner.role = MemberRole::Owner;
  owner.joined_at = util::NowUnixMs();
  ASSERT_TRUE(roster_->UpsertMember((*pending)->group_id, owner));

  GroupRosterMember self;
  self.member_identity = (*pending)->invitee_identity;
  self.role = MemberRole::Member;
  self.joined_at = util::NowUnixMs();
  ASSERT_TRUE(roster_->UpsertMember((*pending)->group_id, self));

  auto members = roster_->ListMembers("group:hike");
  ASSERT_TRUE(members);
  ASSERT_EQ(members->size(), 2u);

  // Skip-self fan-out from invitee must still target the owner.
  size_t outbound = 0;
  for (const GroupRosterMember& member : *members) {
    if (member.member_identity != "relay:bob") {
      ++outbound;
      EXPECT_EQ(member.member_identity, "relay:alice");
    }
  }
  EXPECT_EQ(outbound, 1u);
}

TEST_F(GroupRosterInviteApplyTest, ClearGroupTargetRemovesMapping) {
  ASSERT_TRUE(roster_->UpsertGroupTarget("group:hike", "thread-1", 1, 1));
  auto found = roster_->FindThreadIdForGroup("group:hike");
  ASSERT_TRUE(found && found->has_value());
  EXPECT_EQ(**found, "thread-1");
  ASSERT_TRUE(roster_->ClearGroupTarget("group:hike"));
  found = roster_->FindThreadIdForGroup("group:hike");
  ASSERT_TRUE(found);
  EXPECT_FALSE(found->has_value());
}

TEST_F(GroupRosterInviteApplyTest, TransferLeavePreviousSetsOwnerAndRemovesActor) {
  GroupMetadata metadata;
  metadata.group_id = "group:hike";
  metadata.owner_identity = "relay:alice";
  metadata.title = "Weekend hike";
  metadata.roster_epoch = 1;
  ASSERT_TRUE(roster_->UpsertMetadata(metadata));
  GroupRosterMember alice;
  alice.member_identity = "relay:alice";
  alice.role = MemberRole::Owner;
  alice.joined_at = util::NowUnixMs();
  GroupRosterMember bob;
  bob.member_identity = "relay:bob";
  bob.role = MemberRole::Member;
  bob.joined_at = util::NowUnixMs();
  ASSERT_TRUE(roster_->UpsertMember("group:hike", alice));
  ASSERT_TRUE(roster_->UpsertMember("group:hike", bob));

  GroupMembershipCodec::OwnerTransferredPayload payload;
  payload.group_id = "group:hike";
  payload.new_owner_identity = "relay:bob";
  payload.roster_epoch = 2;
  payload.leave_previous = true;
  ASSERT_TRUE(ApplyOwnerTransferredToRoster(*roster_, payload, "relay:alice"));

  auto meta = roster_->LoadMetadata("group:hike");
  ASSERT_TRUE(meta && meta->has_value());
  EXPECT_EQ((*meta)->owner_identity, "relay:bob");
  EXPECT_EQ((*meta)->roster_epoch, 2u);
  auto members = roster_->ListMembers("group:hike");
  ASSERT_TRUE(members);
  ASSERT_EQ(members->size(), 1u);
  EXPECT_EQ(members->front().member_identity, "relay:bob");
  EXPECT_EQ(members->front().role, MemberRole::Owner);
}

TEST_F(GroupRosterInviteApplyTest, MemberLeftRejectsOwnerAndStaleEpoch) {
  GroupMetadata metadata;
  metadata.group_id = "group:hike";
  metadata.owner_identity = "relay:alice";
  metadata.title = "Weekend hike";
  metadata.roster_epoch = 3;
  ASSERT_TRUE(roster_->UpsertMetadata(metadata));
  GroupRosterMember alice;
  alice.member_identity = "relay:alice";
  alice.role = MemberRole::Owner;
  alice.joined_at = util::NowUnixMs();
  GroupRosterMember bob;
  bob.member_identity = "relay:bob";
  bob.role = MemberRole::Member;
  bob.joined_at = util::NowUnixMs();
  ASSERT_TRUE(roster_->UpsertMember("group:hike", alice));
  ASSERT_TRUE(roster_->UpsertMember("group:hike", bob));

  GroupMembershipCodec::MemberLeftPayload owner_left;
  owner_left.group_id = "group:hike";
  owner_left.member_identity = "relay:alice";
  owner_left.roster_epoch = 4;
  EXPECT_FALSE(ApplyMemberLeftToRoster(*roster_, owner_left, "relay:alice"));

  GroupMembershipCodec::MemberLeftPayload stale;
  stale.group_id = "group:hike";
  stale.member_identity = "relay:bob";
  stale.roster_epoch = 3;
  EXPECT_FALSE(ApplyMemberLeftToRoster(*roster_, stale, "relay:bob"));

  GroupMembershipCodec::MemberLeftPayload ok;
  ok.group_id = "group:hike";
  ok.member_identity = "relay:bob";
  ok.roster_epoch = 4;
  ASSERT_TRUE(ApplyMemberLeftToRoster(*roster_, ok, "relay:bob"));
  auto members = roster_->ListMembers("group:hike");
  ASSERT_TRUE(members);
  ASSERT_EQ(members->size(), 1u);
  EXPECT_EQ(members->front().member_identity, "relay:alice");
}

// An accept counts only for an invite this device issued, for the group it names, which we own.
TEST_F(GroupRosterInviteApplyTest, AcceptNeedsAnInviteWeIssuedForAGroupWeOwn) {
  SeedOwnedGroup();
  PendingGroupInvite received = MakePending();
  received.invite_nonce = "nonce-in";
  received.outbound = false;
  ASSERT_TRUE(roster_->UpsertPendingInvite(received));
  EXPECT_FALSE(Accept("nonce-in", "relay:bob")) << "an invite we received";

  ASSERT_TRUE(roster_->UpsertPendingInvite(MakePending()));
  EXPECT_FALSE(Accept("nonce-1", "relay:bob", "group:other")) << "another group";
  EXPECT_FALSE(ApplyInviteAcceptToRoster(*roster_, "nonce-1", "relay:bob", "group:hike", "relay:carol",
                                         util::NowUnixMs()))
      << "a group this device does not own";
  EXPECT_FALSE(IsMember("relay:bob"));
}

TEST_F(GroupRosterInviteApplyTest, AnExpiredInviteIsNotAccepted) {
  SeedOwnedGroup();
  PendingGroupInvite pending = MakePending();
  pending.expires_at = util::NowUnixMs() - 1;
  ASSERT_TRUE(roster_->UpsertPendingInvite(pending));
  EXPECT_FALSE(Accept("nonce-1", "relay:bob"));
  EXPECT_FALSE(IsMember("relay:bob"));
  auto loaded = roster_->LoadPendingInvite("nonce-1");
  ASSERT_TRUE(loaded && loaded->has_value());
  EXPECT_EQ((*loaded)->status, InviteStatus::Expired);
}

// The roster epoch moves by our own +1, whatever the stored invite says.
TEST_F(GroupRosterInviteApplyTest, AcceptBumpsOurOwnEpochByOne) {
  SeedOwnedGroup(5);
  PendingGroupInvite pending = MakePending();
  pending.roster_epoch = 1000;
  ASSERT_TRUE(roster_->UpsertPendingInvite(pending));
  ASSERT_TRUE(Accept("nonce-1", "relay:bob"));
  auto meta = roster_->LoadMetadata("group:hike");
  ASSERT_TRUE(meta && meta->has_value());
  EXPECT_EQ((*meta)->roster_epoch, 6u);
}

// An accept already applied adds no one again: a member removed since needs a new invite.
TEST_F(GroupRosterInviteApplyTest, AnAppliedAcceptDoesNotReaddARemovedMember) {
  SeedOwnedGroup();
  ASSERT_TRUE(roster_->UpsertPendingInvite(MakePending()));
  ASSERT_TRUE(Accept("nonce-1", "relay:bob"));
  ASSERT_TRUE(IsMember("relay:bob"));

  GroupMembershipCodec::MemberRemovedPayload removed;
  removed.group_id = "group:hike";
  removed.member_identity = "relay:bob";
  removed.roster_epoch = 3;
  ASSERT_TRUE(ApplyMemberRemovedToRoster(*roster_, removed, "relay:alice"));
  ASSERT_FALSE(IsMember("relay:bob"));

  auto again = Accept("nonce-1", "relay:bob");
  ASSERT_TRUE(again);
  EXPECT_FALSE(*again);
  EXPECT_FALSE(IsMember("relay:bob"));
}

// A stored invite is never rewritten by another with the same nonce.
TEST_F(GroupRosterInviteApplyTest, AStoredInviteIsNotRewritten) {
  ASSERT_TRUE(roster_->UpsertPendingInvite(MakePending()));
  ASSERT_TRUE(roster_->UpdateInviteStatus("nonce-1", InviteStatus::Accepted));
  PendingGroupInvite again = MakePending();
  again.group_title = "Other";
  again.roster_epoch = 99;
  ASSERT_TRUE(roster_->UpsertPendingInvite(again));
  auto loaded = roster_->LoadPendingInvite("nonce-1");
  ASSERT_TRUE(loaded && loaded->has_value());
  EXPECT_EQ((*loaded)->status, InviteStatus::Accepted);
  EXPECT_EQ((*loaded)->group_title, "Weekend hike");
  EXPECT_EQ((*loaded)->roster_epoch, 1u);
}

// Renames: the owner's only, and only a newer one (a late or repeated rename keeps the title).
TEST_F(GroupRosterInviteApplyTest, OnlyANewerOwnerRenameChangesTheTitle) {
  SeedOwnedGroup();
  GroupMembershipCodec::GroupRenamedPayload first{"group:hike", "A", 1, 1};
  GroupMembershipCodec::GroupRenamedPayload second{"group:hike", "B", 1, 2};
  auto applied = ApplyGroupRenamedToRoster(*roster_, first, "relay:alice");
  ASSERT_TRUE(applied && *applied);
  applied = ApplyGroupRenamedToRoster(*roster_, second, "relay:alice");
  ASSERT_TRUE(applied && *applied);
  applied = ApplyGroupRenamedToRoster(*roster_, first, "relay:alice");
  ASSERT_TRUE(applied);
  EXPECT_FALSE(*applied) << "older";
  applied = ApplyGroupRenamedToRoster(*roster_, second, "relay:alice");
  ASSERT_TRUE(applied);
  EXPECT_FALSE(*applied) << "the same one again";
  EXPECT_FALSE(ApplyGroupRenamedToRoster(*roster_, GroupMembershipCodec::GroupRenamedPayload{"group:hike", "C", 1, 9},
                                         "relay:bob"))
      << "not the owner";
  auto meta = roster_->LoadMetadata("group:hike");
  ASSERT_TRUE(meta && meta->has_value());
  EXPECT_EQ((*meta)->title, "B");
  EXPECT_EQ((*meta)->title_version, 2u);
}

} // namespace
} // namespace pbr
