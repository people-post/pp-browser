// The account locker (multi-device M023 §4): sealed, padded, signed snapshots any provider can hold.
#include "lib/people/AccountLocker.h"

#include "foundation/crypto/RecoveryPhrase.h"
#include "foundation/crypto/RecoverySeed.h"

#include <gtest/gtest.h>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

LockerKeys KeysOf(uint8_t fill) {
  return account_locker::KeysFrom(DeriveRecoveryKeys(ByteVector(kRecoverySeedBytes, fill)).value()).value();
}

AccountLockerState SampleState() {
  AccountLockerState state;
  state.relay_user_id = "relay:alice";
  state.nickname = "alice";
  Contact bob;
  bob.id = "c1";
  bob.local.display_name = "Bob from work";
  bob.remote.nickname = "bob";
  bob.remote.ids.push_back({mesh::ContactIdKind::Account, "account:bob", true});
  SyncContactMirrors(bob);
  state.contacts = {bob};
  AccountBackupGroup group;
  group.group_id = "group:hike";
  group.title = "Weekend hike";
  group.owner_identity = "account:alice";
  group.roster_epoch = 3;
  group.members = {{"account:alice", "owner", 1000, ""}, {"account:bob", "member", 2000, "c1"}};
  state.extras.groups = {group};
  state.extras.card_floors = {{"account:bob", 1'700'000'000'000}};
  state.providers = {"https://node.example/locker"};
  return state;
}

TEST(AccountLockerTest, ASealedSnapshotOpensToTheSameState) {
  const LockerKeys keys = KeysOf(0x11);
  auto snapshot = account_locker::Seal(SampleState(), keys, 7);
  ASSERT_TRUE(snapshot) << snapshot.error().message;
  EXPECT_EQ(snapshot->version, 7u);
  auto opened = account_locker::Open(*snapshot, keys);
  ASSERT_TRUE(opened) << opened.error().message;
  EXPECT_EQ(opened->relay_user_id, "relay:alice");
  ASSERT_EQ(opened->contacts.size(), 1u);
  EXPECT_EQ(opened->contacts[0].local.display_name, "Bob from work");
  ASSERT_EQ(opened->extras.groups.size(), 1u);
  EXPECT_EQ(opened->extras.groups[0].members.size(), 2u);
  EXPECT_EQ(opened->extras.card_floors, SampleState().extras.card_floors);
  EXPECT_EQ(opened->providers, SampleState().providers);

  EXPECT_FALSE(account_locker::Open(*snapshot, KeysOf(0x12))) << "another seed's locker";
  EXPECT_EQ(KeysOf(0x11).locker_id, keys.locker_id) << "the seed alone finds the locker again";
}

TEST(AccountLockerTest, AProviderChecksTheOwnerWithoutKnowingTheAccount) {
  const LockerKeys keys = KeysOf(0x21);
  const LockerSnapshot snapshot = account_locker::Seal(SampleState(), keys, 1).value();
  EXPECT_TRUE(account_locker::VerifyOwner(snapshot));

  // Someone else writes under this locker's id: their key does not hash to it.
  LockerSnapshot squatter = account_locker::Seal(SampleState(), KeysOf(0x22), 2).value();
  squatter.locker_id = keys.locker_id;
  EXPECT_FALSE(account_locker::VerifyOwner(squatter));
  // The owner's snapshot with its version bumped by someone else: the signature no longer covers it.
  LockerSnapshot bumped = snapshot;
  bumped.version = 99;
  EXPECT_FALSE(account_locker::VerifyOwner(bumped));
  LockerSnapshot edited = snapshot;
  edited.sealed.back() ^= 0x01;
  EXPECT_FALSE(account_locker::VerifyOwner(edited));
}

TEST(AccountLockerTest, TheNewestValidSnapshotWinsAndAnOldOneCannotRollBack) {
  const LockerKeys keys = KeysOf(0x31);
  AccountLockerState older = SampleState();
  AccountLockerState newer = SampleState();
  newer.nickname = "alice2";
  const LockerSnapshot v3 = account_locker::Seal(older, keys, 3).value();
  const LockerSnapshot v5 = account_locker::Seal(newer, keys, 5).value();
  LockerSnapshot forged_v9 = account_locker::Seal(newer, KeysOf(0x32), 9).value();
  forged_v9.locker_id = keys.locker_id;

  auto picked = account_locker::PickNewest({v3, forged_v9, v5}, keys);
  ASSERT_TRUE(picked);
  EXPECT_EQ(picked->version, 5u) << "the forged newer one does not count";
  // This device has seen version 5: a provider still serving 3 cannot take it back.
  EXPECT_FALSE(account_locker::PickNewest({v3}, keys, /*at_least=*/5));
  EXPECT_FALSE(account_locker::PickNewest({}, keys));
}

TEST(AccountLockerTest, SizesAreBucketedSoSmallChangesDoNotShow) {
  const LockerKeys keys = KeysOf(0x41);
  AccountLockerState few = SampleState();
  AccountLockerState more = SampleState();
  more.contacts.push_back(more.contacts[0]);
  const auto a = account_locker::Seal(few, keys, 1).value();
  const auto b = account_locker::Seal(more, keys, 2).value();
  EXPECT_EQ(a.sealed.size(), b.sealed.size());

  AccountLockerState many = SampleState();
  for (int i = 0; i < 200; ++i) {
    many.contacts.push_back(many.contacts[0]);
  }
  const auto c = account_locker::Seal(many, keys, 3).value();
  EXPECT_GT(c.sealed.size(), a.sealed.size()) << "a much larger state moves up a bucket";
  EXPECT_TRUE(account_locker::Open(c, keys));
}

} // namespace
} // namespace pbr
