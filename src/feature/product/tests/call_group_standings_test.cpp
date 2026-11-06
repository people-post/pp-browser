#include "feature/product/CallGroupStandings.h"

#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "lib/messaging/testing/InMemoryMessagingStores.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// V062 / G017: who brings and removes guests in a group's call, by role and preset.
TEST(CallGroupStandingsTest, RolesAndPresetsInTheGroupsCall) {
  const CallGroupStanding open_member = CallStandingOfMember(MemberRole::Member, GroupPreset::Open);
  EXPECT_TRUE(open_member.member);
  EXPECT_TRUE(open_member.may_bring_guest) << "Open: any member brings a guest";
  EXPECT_FALSE(open_member.may_remove_guest);

  const CallGroupStanding managed_member = CallStandingOfMember(MemberRole::Member, GroupPreset::Managed);
  EXPECT_FALSE(managed_member.may_bring_guest) << "Managed: the owner and admins only";
  EXPECT_FALSE(managed_member.may_remove_guest);

  const CallGroupStanding managed_admin = CallStandingOfMember(MemberRole::Admin, GroupPreset::Managed);
  EXPECT_TRUE(managed_admin.may_bring_guest);
  EXPECT_TRUE(managed_admin.may_remove_guest);

  const CallGroupStanding owner = CallStandingOfMember(MemberRole::Owner, GroupPreset::Managed);
  EXPECT_TRUE(owner.may_bring_guest);
  EXPECT_TRUE(owner.may_remove_guest);
  EXPECT_TRUE(owner.may_invite_to_group);
}

GroupRosterMember Member(const std::string& identity, const MemberRole role) {
  GroupRosterMember member;
  member.member_identity = identity;
  member.role = role;
  return member;
}

// V062: the standing read from the roster store — none for a group not held here (a guest's device), a non-member for
// someone the held roster does not list, nobody in a dissolved group.
TEST(CallGroupStandingsTest, FromTheRosterThisDeviceHolds) {
  InMemoryDeliveryStore threads;
  InMemoryGroupRosterStore rosters(threads);
  GroupMetadata metadata;
  metadata.group_id = "group:1";
  metadata.owner_identity = "account:alice";
  metadata.policy.preset = GroupPreset::Managed;
  ASSERT_TRUE(rosters.UpsertMetadata(metadata));
  ASSERT_TRUE(rosters.UpsertMember("group:1", Member("account:alice", MemberRole::Owner)));
  ASSERT_TRUE(rosters.UpsertMember("group:1", Member("account:bob", MemberRole::Member)));

  EXPECT_FALSE(CallStandingFromRoster(rosters, "group:other", "account:bob")) << "no roster here";
  const auto erin = CallStandingFromRoster(rosters, "group:1", "account:erin");
  ASSERT_TRUE(erin);
  EXPECT_FALSE(erin->member);
  const auto bob = CallStandingFromRoster(rosters, "group:1", "account:bob");
  ASSERT_TRUE(bob);
  EXPECT_TRUE(bob->member);
  EXPECT_FALSE(bob->may_bring_guest) << "the preset is the group's";
  const auto alice = CallStandingFromRoster(rosters, "group:1", "account:alice");
  ASSERT_TRUE(alice);
  EXPECT_TRUE(alice->may_remove_guest);

  metadata.group_id = "group:2";
  metadata.sync_state = GroupSyncState::Dissolved;
  ASSERT_TRUE(rosters.UpsertMetadata(metadata));
  const auto dissolved = CallStandingFromRoster(rosters, "group:2", "account:alice");
  ASSERT_TRUE(dissolved);
  EXPECT_FALSE(dissolved->member);
}

} // namespace
} // namespace pbr
