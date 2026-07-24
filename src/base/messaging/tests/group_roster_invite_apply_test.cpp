#include "base/messaging/GroupMembershipApply.h"
#include "base/messaging/GroupRosterStore.h"
#include "base/messaging/SqliteThreadStore.h"

#include "common/Utilities.h"

#include <filesystem>
#include <gtest/gtest.h>

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
    return pending;
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

  ASSERT_TRUE(ApplyInviteAcceptToRoster(*roster_, "nonce-1", "relay:bob"));

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
  ASSERT_TRUE(roster_->UpsertPendingInvite(MakePending()));
  auto result = ApplyInviteAcceptToRoster(*roster_, "nonce-1", "relay:eve");
  ASSERT_FALSE(result);
}

TEST_F(GroupRosterInviteApplyTest, DeclineUpdatesStatusOnly) {
  GroupMetadata metadata;
  metadata.group_id = "group:hike";
  metadata.owner_identity = "relay:alice";
  metadata.title = "Weekend hike";
  metadata.roster_epoch = 1;
  ASSERT_TRUE(roster_->UpsertMetadata(metadata));
  ASSERT_TRUE(roster_->UpsertPendingInvite(MakePending()));

  ASSERT_TRUE(ApplyInviteDeclineToRoster(*roster_, "nonce-1", "relay:bob"));

  auto members = roster_->ListMembers("group:hike");
  ASSERT_TRUE(members);
  EXPECT_TRUE(members->empty());

  auto loaded = roster_->LoadPendingInvite("nonce-1");
  ASSERT_TRUE(loaded && loaded->has_value());
  EXPECT_EQ((*loaded)->status, InviteStatus::Declined);
}

TEST_F(GroupRosterInviteApplyTest, PendingSnapshotRoundTrip) {
  ASSERT_TRUE(roster_->UpsertPendingInvite(MakePending()));
  auto loaded = roster_->LoadPendingInvite("nonce-1");
  ASSERT_TRUE(loaded && loaded->has_value());
  EXPECT_EQ((*loaded)->group_title, "Weekend hike");
  EXPECT_EQ((*loaded)->roster_epoch, 1u);
}

TEST_F(GroupRosterInviteApplyTest, InviteeAcceptSeedsOwnerAndSelfForFanout) {
  // Mirrors GroupMembershipService::AcceptInvite roster seeding on the invitee device.
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

} // namespace
} // namespace pbr
