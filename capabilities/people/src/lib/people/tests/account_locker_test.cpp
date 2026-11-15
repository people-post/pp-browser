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
  AccountLockerState ours = SampleState(); // bob, group:hike at epoch 3, floor for bob
  AccountLockerState stored = SampleState();
  Contact carol;
  carol.id = "c2";
  carol.remote.ids.push_back({mesh::ContactIdKind::Account, "account:carol", true});
  SyncContactMirrors(carol);
  stored.contacts.push_back(carol);
  stored.contacts[0].local.display_name = "Bob (laptop's name)";
  stored.extras.groups[0].roster_epoch = 5; // the laptop saw a newer roster
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

  const AccountLockerState stored = account_locker::MergeForPublish(phone, phone); // what the phone wrote
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
  AccountLockerState stored = SampleState(); // group:hike at epoch 3, we are in
  laptop.extras.groups[0].roster_epoch = 4;
  laptop.extras.groups[0].left = true;
  AccountLockerState merged = account_locker::MergeForPublish(laptop, stored);
  ASSERT_EQ(merged.extras.groups.size(), 1u);
  EXPECT_TRUE(merged.extras.groups[0].left) << "the older copy where we were in does not bring it back";

  AccountLockerState phone = SampleState();
  phone.extras.groups[0].left = true; // same epoch 3: removed, found by the member check
  merged = account_locker::MergeForPublish(stored, phone);
  EXPECT_TRUE(merged.extras.groups[0].left) << "at the same epoch, being out wins";

  AccountLockerState reinvited = SampleState();
  reinvited.extras.groups[0].roster_epoch = 6; // invited back later
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
  live.extras.groups[0].roster_epoch = 9; // a device that never heard of it

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

// --- Device names (key `device_names`) ---------------------------------------------------------------------

DeviceNameEntry Named(const std::string& peer, const std::string& name, const int64_t at) {
  return DeviceNameEntry{peer, name, at};
}

std::string Rep(const std::string& unit, const size_t times) {
  std::string out;
  for (size_t i = 0; i < times; ++i) {
    out += unit;
  }
  return out;
}

/** What a state with just these names reads back as. */
std::vector<DeviceNameEntry> ReadBack(const std::vector<DeviceNameEntry>& entries) {
  AccountLockerState state;
  state.device_names = entries;
  auto read = account_locker::StateFromJson(account_locker::StateToJson(state));
  EXPECT_TRUE(read);
  return read ? read->device_names : std::vector<DeviceNameEntry>{};
}

TEST(AccountLockerTest, DeviceNamesRoundTripWhateverTheyHold) {
  const std::vector<DeviceNameEntry> names = {Named("peer:a", "\xE6\x88\x91\xE7\x9A\x84 iPhone", 5),
                                              Named("peer:b", "Kim's \"Mac\" \\ \xF0\x9F\x98\x80", 9)};
  EXPECT_EQ(ReadBack(names), names);
  EXPECT_TRUE(ReadBack({}).empty());
  auto snapshot = account_locker::Seal(AccountLockerState{.device_names = names}, KeysOf(0x21), 3);
  ASSERT_TRUE(snapshot);
  auto opened = account_locker::Open(*snapshot, KeysOf(0x21));
  ASSERT_TRUE(opened);
  EXPECT_EQ(opened->device_names, names);
}

TEST(AccountLockerTest, AnOldSnapshotHasNoNamesAndKeepsEverythingElse) {
  auto read = account_locker::StateFromJson(R"({"nickname":"alice","providers":["https://node.example/locker"]})");
  ASSERT_TRUE(read);
  EXPECT_TRUE(read->device_names.empty());
  EXPECT_EQ(read->nickname, "alice");
  EXPECT_EQ(read->providers.size(), 1u);
}

TEST(AccountLockerTest, UnknownKeysAndFieldsAreIgnoredOnRead) {
  auto read = account_locker::StateFromJson(
      R"({"future_key":{"x":1},"device_names":[{"peer_id":"peer:a","name":"Phone","at":4,"icon":"star"},)"
      R"({"peer_id":"peer:b","name":"Tablet","at":6,"extra":[1,2]}]})");
  ASSERT_TRUE(read);
  EXPECT_EQ(read->device_names,
            (std::vector<DeviceNameEntry>{Named("peer:a", "Phone", 4), Named("peer:b", "Tablet", 6)}));
}

TEST(AccountLockerTest, AnOldDeviceRewritingTheLockerCostsTheNewDeviceNothingLasting) {
  // The old device's output has no device_names; the new one merges it with what it has, puts its entry back
  // and the result reads fine.
  AccountLockerState old_rewrite = SampleState(); // never heard of names
  AccountLockerState ours = SampleState();
  ours.device_names = {Named("peer:a", "Phone", 5)};
  auto stored = account_locker::StateFromJson(account_locker::StateToJson(old_rewrite));
  ASSERT_TRUE(stored);
  const AccountLockerState merged = account_locker::MergeForPublish(ours, *stored);
  EXPECT_EQ(merged.device_names, ours.device_names);
}

TEST(AccountLockerTest, DeviceNamesMergeByPeerTheNewerAtWinsWhoeverWroteIt) {
  const std::vector<DeviceNameEntry> local = {Named("peer:a", "Mine A", 10), Named("peer:b", "Mine B", 5),
                                              Named("peer:c", "Only here", 1)};
  const std::vector<DeviceNameEntry> remote = {Named("peer:a", "Theirs A", 7), Named("peer:b", "Theirs B", 8),
                                               Named("peer:d", "Only there", 2)};
  EXPECT_EQ(account_locker::MergeDeviceNames(local, remote),
            (std::vector<DeviceNameEntry>{Named("peer:a", "Mine A", 10), Named("peer:b", "Theirs B", 8),
                                          Named("peer:c", "Only here", 1), Named("peer:d", "Only there", 2)}));
}

TEST(AccountLockerTest, ATieInDeviceNamesKeepsTheStoredOne) {
  EXPECT_EQ(account_locker::MergeDeviceNames({Named("peer:a", "Local", 5)}, {Named("peer:a", "Stored", 5)}),
            (std::vector<DeviceNameEntry>{Named("peer:a", "Stored", 5)}));
}

TEST(AccountLockerTest, AStoredStateWithoutTheKeyErasesNoLocalName) {
  AccountLockerState ours = SampleState();
  ours.device_names = {Named("peer:a", "Phone", 5), Named("peer:b", "Set by the main device", 9)};
  const AccountLockerState stored = SampleState(); // an old version wrote it: no device_names
  EXPECT_EQ(account_locker::MergeForPublish(ours, stored).device_names, ours.device_names);
}

TEST(AccountLockerTest, DevicesWithDifferentCardsKeepEachOthersNamesAndSettle) {
  // Phone knows the tablet (named by the phone); the laptop's card is older and does not list it.
  AccountLockerState phone = SampleState();
  phone.device_names = {Named("peer:phone", "Phone", 3), Named("peer:tablet", "Tablet (by phone)", 4)};
  AccountLockerState laptop = SampleState();
  laptop.device_names = {Named("peer:laptop", "Laptop", 6)};

  // The phone writes; the laptop merges (its card lacks the tablet) and writes back.
  const AccountLockerState stored1 = account_locker::MergeForPublish(phone, SampleState());
  const AccountLockerState stored2 = account_locker::MergeForPublish(laptop, stored1);
  EXPECT_EQ(stored2.device_names.size(), 3u) << "the tablet's name survives the laptop's older card";
  // The phone merges that back and finds nothing new; the laptop's next publish is the same bytes.
  const AccountLockerState phone_again = account_locker::MergeForPublish(phone, stored2);
  EXPECT_EQ(phone_again.device_names, stored2.device_names);
  const AccountLockerState laptop_again = account_locker::MergeForPublish(laptop, stored2);
  EXPECT_NE(account_locker::StateToJson(laptop_again).find("Tablet (by phone)"), std::string::npos);
  EXPECT_EQ(laptop_again.device_names, stored2.device_names);
}

DeviceNameEntry Gone(const std::string& peer, const int64_t at) {
  DeviceNameEntry entry;
  entry.peer_id = peer;
  entry.at = at;
  entry.removed = true;
  return entry;
}

std::vector<std::string> PeersOf(const std::vector<DeviceNameEntry>& entries) {
  std::vector<std::string> out;
  for (const DeviceNameEntry& e : entries) {
    out.push_back(e.peer_id);
  }
  return out;
}

TEST(AccountLockerTest, TheCapDropsTombstonesFirstThenTheOldestLiveEntriesAndNeedsNoCard) {
  std::vector<DeviceNameEntry> many;
  for (int i = 0; i < 9; ++i) {
    many.push_back(Named("peer:" + std::to_string(i), "Device " + std::to_string(i), 100 + i));
  }
  // Nine live: the oldest goes.
  auto capped = account_locker::CapDeviceNames(many);
  ASSERT_EQ(capped.size(), kMaxDeviceNameEntries);
  EXPECT_EQ(capped.front().peer_id, "peer:1");
  EXPECT_EQ(capped.back().peer_id, "peer:8");
  // Same input in another order: the same result (every device cuts alike).
  std::vector<DeviceNameEntry> shuffled(many.rbegin(), many.rend());
  EXPECT_EQ(account_locker::CapDeviceNames(shuffled), capped);
  // Tombstones go before any live entry, the oldest tombstone first, even when a tombstone is newer than a name.
  std::vector<DeviceNameEntry> mixed;
  for (int i = 0; i < 7; ++i) {
    mixed.push_back(Named("peer:" + std::to_string(i), "Device", 10 + i));
  }
  mixed.push_back(Gone("peer:x", 500));
  mixed.push_back(Gone("peer:y", 400));
  capped = account_locker::CapDeviceNames(mixed);
  ASSERT_EQ(capped.size(), kMaxDeviceNameEntries);
  const std::vector<std::string> peers = PeersOf(capped);
  EXPECT_NE(std::find(peers.begin(), peers.end(), "peer:x"), peers.end()) << "the newer tombstone stays";
  EXPECT_EQ(std::find(peers.begin(), peers.end(), "peer:y"), peers.end()) << "the older tombstone goes first";
  EXPECT_NE(std::find(peers.begin(), peers.end(), "peer:0"), peers.end()) << "no live entry was cut";
  // A tie in `at` is broken by peer_id: the smaller peer_id is kept.
  std::vector<DeviceNameEntry> tied;
  for (int i = 0; i < 9; ++i) {
    tied.push_back(Named("peer:" + std::to_string(i), "Same", 7));
  }
  const std::vector<std::string> kept = PeersOf(account_locker::CapDeviceNames(tied));
  EXPECT_EQ(std::find(kept.begin(), kept.end(), "peer:8"), kept.end());
  // Under the limit nothing is pruned.
  EXPECT_EQ(account_locker::CapDeviceNames({Named("peer:x", "X", 1)}).size(), 1u);
}

TEST(AccountLockerTest, ARemovalTombstoneRoundTripsWithoutANameAndWinsByAt) {
  const std::vector<DeviceNameEntry> names = {Named("peer:a", "Phone", 5), Gone("peer:b", 9)};
  EXPECT_EQ(ReadBack(names), names);
  AccountLockerState state;
  state.device_names = names;
  const std::string json = account_locker::StateToJson(state);
  EXPECT_NE(json.find("\"removed\":true"), std::string::npos);
  // A tombstone read from a file ignores any name it carries.
  auto read =
      account_locker::StateFromJson(R"({"device_names":[{"peer_id":"peer:b","name":"Leaked","at":9,"removed":true}]})");
  ASSERT_TRUE(read);
  EXPECT_EQ(read->device_names, (std::vector<DeviceNameEntry>{Gone("peer:b", 9)}));
  // The rule is unchanged: the larger `at` wins, tombstone or live.
  EXPECT_EQ(account_locker::MergeDeviceNames({Named("peer:b", "Old", 5)}, {Gone("peer:b", 9)}),
            (std::vector<DeviceNameEntry>{Gone("peer:b", 9)}));
  EXPECT_EQ(account_locker::MergeDeviceNames({Gone("peer:b", 9)}, {Named("peer:b", "Back", 12)}),
            (std::vector<DeviceNameEntry>{Named("peer:b", "Back", 12)}));
}

TEST(AccountLockerTest, AnEntryFarInTheFutureOrBelowZeroIsClampedOnRead) {
  auto read =
      account_locker::StateFromJson(R"({"device_names":[{"peer_id":"peer:a","name":"Far","at":9223372036854775807},)"
                                    R"({"peer_id":"peer:b","name":"Early","at":-5}]})");
  ASSERT_TRUE(read);
  ASSERT_EQ(read->device_names.size(), 2u);
  EXPECT_EQ(read->device_names[0].at, kMaxDeviceNameAt);
  EXPECT_EQ(read->device_names[1].at, 0);
}

TEST(AccountLockerTest, ANameReadFromTheLockerIsCleanedFirst) {
  const std::vector<DeviceNameEntry> read = ReadBack({
      Named("peer:a", "  Phone\n\t(old)\r  ", 1),
      Named("peer:b", "   \n ", 2),
      Named("peer:c", "", 3),
      Named("peer:d", "Name {device} {id}", 4),
      Named("", "No peer", 5),
  });
  EXPECT_EQ(read,
            (std::vector<DeviceNameEntry>{Named("peer:a", "Phone(old)", 1), Named("peer:d", "Name {device} {id}", 4)}));
}

TEST(AccountLockerTest, ANameReadFromTheLockerIsCutAtThirtyTwoCodePoints) {
  const std::string family = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";
  const std::vector<DeviceNameEntry> read = ReadBack({
      Named("peer:a", Rep("a", 40), 1),
      Named("peer:b", Rep("\xE4\xBD\xA0", 33), 2),
      Named("peer:c",
            Rep("a", 31) + "\xF0\x9F\x98\x80"
                           "b",
            3),
      Named("peer:d", Rep("a", 30) + family, 4),
  });
  ASSERT_EQ(read.size(), 4u);
  EXPECT_EQ(read[0].name, Rep("a", 32));
  EXPECT_EQ(read[1].name, Rep("\xE4\xBD\xA0", 32));
  EXPECT_EQ(read[2].name, Rep("a", 31) + "\xF0\x9F\x98\x80");
  EXPECT_EQ(read[3].name, Rep("a", 30) + "\xF0\x9F\x91\xA8\xE2\x80\x8D");
}

TEST(AccountLockerTest, MoreEntriesThanTheLimitAreDroppedOnRead) {
  std::string json = R"({"device_names":[)";
  for (int i = 0; i < 12; ++i) {
    json += std::string(i ? "," : "") + R"({"peer_id":"peer:)" + std::to_string(i) + R"(","name":"D","at":)" +
            std::to_string(i + 1) + "}";
  }
  json += "]}";
  auto read = account_locker::StateFromJson(json);
  ASSERT_TRUE(read);
  EXPECT_EQ(read->device_names.size(), kMaxDeviceNameEntries);
}

// Grants given to the account (multi-relay MR005) ride the locker, so a restored or linked device's first sends to
// its grantors are not a stranger's (ACCOUNT_LOCKER § State JSON `grants`).
AccountStateGrant GrantFrom(const std::string& grantor, int64_t issued_at, int64_t expires_at = 0,
                            const std::string& signature = "c2ln") {
  return AccountStateGrant{.grantor_account_id = grantor,
                           .grantee_account_id = "account:alice",
                           .issued_at = issued_at,
                           .expires_at = expires_at ? expires_at : issued_at + 1'000'000,
                           .signature = signature};
}

TEST(AccountLockerTest, GrantsRoundTripAndABrokenRowIsDropped) {
  AccountLockerState state = SampleState();
  state.extras.grants = {GrantFrom("account:bob", 100), GrantFrom("account:carol", 200)};
  const LockerKeys keys = KeysOf(0x11);
  auto opened = account_locker::Open(account_locker::Seal(state, keys, 1).value(), keys);
  ASSERT_TRUE(opened) << opened.error().message;
  EXPECT_EQ(opened->extras.grants, state.extras.grants);

  auto read = account_locker::StateFromJson(
      R"({"grants":[{"recipient_account_id":"account:bob","sender_account_id":"account:alice","issued_at":5,)"
      R"("expires_at":9,"signature":"c2ln","future":1},)"
      R"({"recipient_account_id":"account:carol","sender_account_id":"account:alice","issued_at":5,"expires_at":9},)"
      R"({"recipient_account_id":"account:dave","sender_account_id":"account:alice","issued_at":9,"expires_at":5,)"
      R"("signature":"c2ln"}]})");
  ASSERT_TRUE(read);
  EXPECT_EQ(read->extras.grants, (std::vector<AccountStateGrant>{GrantFrom("account:bob", 5, 9)}))
      << "no signature, or expiring before it was issued: dropped";
  EXPECT_TRUE(account_locker::StateFromJson(R"({"nickname":"a"})")->extras.grants.empty()) << "an older snapshot";
}

TEST(AccountLockerTest, GrantsMergeToTheLatestPerGrantorWithoutTheExpired) {
  AccountLockerState ours;
  ours.extras.grants = {GrantFrom("account:bob", 300), GrantFrom("account:carol", 100, 150)};
  AccountLockerState stored;
  stored.extras.grants = {GrantFrom("account:bob", 200), GrantFrom("account:dave", 50)};
  const AccountLockerState merged = account_locker::MergeForPublish(ours, stored);
  EXPECT_EQ(account_locker::CapGrants(merged.extras.grants, 160),
            (std::vector<AccountStateGrant>{GrantFrom("account:bob", 300), GrantFrom("account:dave", 50)}))
      << "Bob's later one; Carol's ran out at 150; Dave's from the other device";
  EXPECT_EQ(
      account_locker::CapGrants({GrantFrom("account:bob", 300, 0, "YQ"), GrantFrom("account:bob", 300, 0, "Yg")}, 0),
      (std::vector<AccountStateGrant>{GrantFrom("account:bob", 300, 0, "Yg")}))
      << "a tie keeps the same one on every device";
}

// D2 (ACCOUNT_LOCKER § State JSON `relays`): the locker carries our relays, so a restore keeps the person's list; the
// copy from the newer card wins a merge, and an untrusted copy is bounded.
TEST(AccountLockerTest, OurRelaysRoundTripAndTheNewerCardsListWinsAMerge) {
  AccountLockerState state = SampleState();
  state.extras.relays = {.origins = {"https://b.example/api/relay", "https://a.example/api/relay"},
                         .card_issued_at = 500};
  const LockerKeys keys = KeysOf(0x11);
  auto opened = account_locker::Open(account_locker::Seal(state, keys, 1).value(), keys);
  ASSERT_TRUE(opened) << opened.error().message;
  EXPECT_EQ(opened->extras.relays, state.extras.relays);

  AccountLockerState older = state;
  older.extras.relays = {.origins = {"https://a.example/api/relay"}, .card_issued_at = 400};
  EXPECT_EQ(account_locker::MergeForPublish(older, state).extras.relays, state.extras.relays)
      << "the stored list came with a newer card";
  EXPECT_EQ(account_locker::MergeForPublish(state, older).extras.relays, state.extras.relays) << "ours is newer";

  auto read = account_locker::StateFromJson(
      R"({"relays":{"origins":["https://a.example","https://a.example","","https://b.example","https://c.example",)"
      R"("https://d.example","https://e.example"],"card_issued_at":7}})");
  ASSERT_TRUE(read);
  EXPECT_EQ(read->extras.relays.origins, (std::vector<std::string>{"https://a.example", "https://b.example",
                                                                   "https://c.example", "https://d.example"}))
      << "each once, none empty, at most four";
  EXPECT_TRUE(account_locker::StateFromJson(R"({"nickname":"a"})")->extras.relays.origins.empty())
      << "an older snapshot: none";
}

TEST(AccountLockerTest, OverTheCapTheLatestIssuedGrantsStay) {
  std::vector<AccountStateGrant> grants;
  for (size_t i = 0; i < kMaxLockerGrants + 3; ++i) {
    grants.push_back(GrantFrom("account:g" + std::to_string(100 + i), static_cast<int64_t>(1000 + i)));
  }
  const std::vector<AccountStateGrant> capped = account_locker::CapGrants(grants, 0);
  ASSERT_EQ(capped.size(), kMaxLockerGrants);
  EXPECT_EQ(capped.front().grantor_account_id, "account:g103") << "the three issued first went";
  EXPECT_TRUE(std::is_sorted(capped.begin(), capped.end(),
                             [](const auto& a, const auto& b) { return a.grantor_account_id < b.grantor_account_id; }));
}

TEST(AccountLockerTest, SignBytesMatchTheRelaysGoldenVector) {
  // Shared with the relay (web2 www __tests__/AccountLocker.test.ts): what a provider verifies.
  ByteVector locker_id(32);
  for (size_t i = 0; i < locker_id.size(); ++i) {
    locker_id[i] = static_cast<uint8_t>(i);
  }
  EXPECT_EQ(
      BytesToHex(account_locker::SignBytes(locker_id, 7, ByteVector{0xaa, 0xbb})),
      "70702d62726f777365723a6163636f756e742d6c6f636b65722d763100010000000000000020000102030405060708090a0b0c0d0e0f"
      "101112131415161718191a1b1c1d1e1f00000000000000070000000000000002aabb");
}

} // namespace
} // namespace pbr
