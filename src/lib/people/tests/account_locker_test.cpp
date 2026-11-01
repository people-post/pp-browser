// The account locker (multi-device M023 §4): sealed, padded, signed snapshots any provider can hold.
#include "lib/people/AccountLocker.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/RecoveryPhrase.h"
#include "foundation/crypto/RecoverySeed.h"

#include <gtest/gtest.h>

#include <algorithm>
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
  AccountStateGroup group;
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

TEST(AccountLockerTest, PublishingMergesWhatAnotherDeviceStored) {
  AccountLockerState ours = SampleState();  // bob, group:hike at epoch 3, floor for bob
  AccountLockerState stored = SampleState();
  Contact carol;
  carol.id = "c2";
  carol.remote.ids.push_back({mesh::ContactIdKind::Account, "account:carol", true});
  SyncContactMirrors(carol);
  stored.contacts.push_back(carol);
  stored.contacts[0].local.display_name = "Bob (laptop's name)";
  stored.extras.groups[0].roster_epoch = 5;       // the laptop saw a newer roster
  stored.extras.groups[0].title = "Hike (renamed)";
  AccountStateGroup book_club;
  book_club.group_id = "group:books";
  stored.extras.groups.push_back(book_club);
  stored.extras.card_floors = {{"account:bob", 1'800'000'000'000}, {"account:carol", 5}};
  stored.providers = {"https://node.example/locker", "https://friend.example/locker"};

  const AccountLockerState merged = account_locker::MergeForPublish(ours, stored);
  ASSERT_EQ(merged.contacts.size(), 2u) << "carol, known only to the laptop, is kept";
  EXPECT_EQ(merged.contacts[0].local.display_name, "Bob from work") << "ours win for a contact both have";
  ASSERT_EQ(merged.extras.groups.size(), 2u);
  const auto hike = std::find_if(merged.extras.groups.begin(), merged.extras.groups.end(),
                                 [](const AccountStateGroup& g) { return g.group_id == "group:hike"; });
  ASSERT_NE(hike, merged.extras.groups.end());
  EXPECT_EQ(hike->roster_epoch, 5u) << "the newer roster wins";
  EXPECT_EQ(hike->title, "Hike (renamed)");
  EXPECT_EQ(merged.extras.card_floors,
            (std::vector<AccountCardFloor>{{"account:bob", 1'800'000'000'000}, {"account:carol", 5}}));
  EXPECT_EQ(merged.providers.size(), 2u);
}

TEST(AccountLockerTest, ASyncWithNothingNewerChangesNothingOnEitherDevice) {
  // The phone and the laptop each hold Bob as their own row (another local id), with the same register; their
  // lists in another order. Whichever device merges, the stored state stands — no write, no nudge back (M023 s4).
  AccountLockerState phone = SampleState();
  phone.contact_clock = {{"account:bob", 1000, true, ContactUserFieldsDigest(phone.contacts[0])},
                         {"account:carol", 900, false, ""}};
  AccountStateGroup books;
  books.group_id = "group:books";
  books.owner_identity = "account:alice";
  books.roster_epoch = 2;
  phone.extras.groups.push_back(books);
  phone.extras.card_floors = {{"account:bob", 7}, {"account:carol", 3}};
  AccountLockerState laptop = phone;
  laptop.contacts[0].id = "laptop-local-id";
  laptop.extras.groups[0].members = {{"account:alice", "owner", 1, "laptop-contact"}};
  std::reverse(laptop.contact_clock.begin(), laptop.contact_clock.end());
  std::reverse(laptop.extras.groups.begin(), laptop.extras.groups.end());
  std::reverse(laptop.extras.card_floors.begin(), laptop.extras.card_floors.end());

  const AccountLockerState stored = account_locker::MergeForPublish(phone, phone);  // what the phone wrote
  EXPECT_EQ(account_locker::StateToJson(account_locker::MergeForPublish(laptop, stored)),
            account_locker::StateToJson(stored));
  const AccountLockerState laptop_wrote = account_locker::MergeForPublish(laptop, laptop);
  EXPECT_EQ(account_locker::StateToJson(account_locker::MergeForPublish(phone, laptop_wrote)),
            account_locker::StateToJson(laptop_wrote));
}

TEST(AccountLockerTest, AGroupLeftOnOneDeviceStaysLeft) {
  // M023 s4: the laptop left the hike group (epoch 4, a tombstone); the phone's stored copy is from epoch 3 and,
  // after the check, the phone's own record is a tombstone at the epoch it still had.
  AccountLockerState laptop = SampleState();
  AccountLockerState stored = SampleState();  // group:hike at epoch 3, we are in
  laptop.extras.groups[0].roster_epoch = 4;
  laptop.extras.groups[0].left = true;
  AccountLockerState merged = account_locker::MergeForPublish(laptop, stored);
  ASSERT_EQ(merged.extras.groups.size(), 1u);
  EXPECT_TRUE(merged.extras.groups[0].left) << "the older copy where we were in does not bring it back";

  AccountLockerState phone = SampleState();
  phone.extras.groups[0].left = true;  // same epoch 3: removed, found by the member check
  merged = account_locker::MergeForPublish(stored, phone);
  EXPECT_TRUE(merged.extras.groups[0].left) << "at the same epoch, being out wins";

  AccountLockerState reinvited = SampleState();
  reinvited.extras.groups[0].roster_epoch = 6;  // invited back later
  merged = account_locker::MergeForPublish(reinvited, laptop);
  EXPECT_FALSE(merged.extras.groups[0].left) << "a newer roster with us in it wins over the tombstone";
}

TEST(AccountLockerTest, ADissolvedGroupStaysDissolvedWhateverTheOtherCopySays) {
  // G016: the owner ended the hike group at epoch 4. A copy at a higher epoch, or one where we were a member, does not
  // bring it back — whichever side of the merge holds the tombstone.
  AccountLockerState dissolved = SampleState();
  dissolved.extras.groups[0].roster_epoch = 4;
  dissolved.extras.groups[0].left = true;
  dissolved.extras.groups[0].dissolved = true;
  AccountLockerState live = SampleState();
  live.extras.groups[0].roster_epoch = 9;  // a device that never heard of it

  AccountLockerState merged = account_locker::MergeForPublish(live, dissolved);
  ASSERT_EQ(merged.extras.groups.size(), 1u);
  EXPECT_TRUE(merged.extras.groups[0].dissolved) << "stored dissolved beats our higher-epoch live copy";
  EXPECT_TRUE(merged.extras.groups[0].left);
  merged = account_locker::MergeForPublish(dissolved, live);
  EXPECT_TRUE(merged.extras.groups[0].dissolved) << "ours dissolved beats the stored higher-epoch live copy";
  EXPECT_EQ(merged.extras.groups[0].roster_epoch, 4u);

  // Two dissolved copies keep the higher epoch.
  AccountLockerState later = dissolved;
  later.extras.groups[0].roster_epoch = 6;
  merged = account_locker::MergeForPublish(dissolved, later);
  EXPECT_TRUE(merged.extras.groups[0].dissolved);
  EXPECT_EQ(merged.extras.groups[0].roster_epoch, 6u);
}

TEST(AccountLockerTest, SignBytesMatchTheRelaysGoldenVector) {
  // Shared with the relay (web2 www __tests__/AccountLocker.test.ts): what a provider verifies.
  ByteVector locker_id(32);
  for (size_t i = 0; i < locker_id.size(); ++i) {
    locker_id[i] = static_cast<uint8_t>(i);
  }
  EXPECT_EQ(BytesToHex(account_locker::SignBytes(locker_id, 7, ByteVector{0xaa, 0xbb})),
            "70702d62726f777365723a6163636f756e742d6c6f636b65722d763100010000000000000020000102030405060708090a0b0c0d0e0f"
            "101112131415161718191a1b1c1d1e1f00000000000000070000000000000002aabb");
}

} // namespace
} // namespace pbr
