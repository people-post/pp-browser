#include "lib/messaging/AccountCard.h"
#include "lib/messaging/PeerCardStore.h"

#include "lib/messaging/tests/TestAccount.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace pbr {
namespace {

constexpr int64_t kNow = 1'761'235'200'000;

KeyCard KeysOf(const test::TestAccount& account) {
  return KeyCard{.account_id = account.account_id,
                 .signing_public_key_b64 = account.SigningPublicB64(),
                 .kem_public_key_b64 = account.KemPublicB64(),
                 .kem_binding_signature_b64 = account.KemBindingB64()};
}

AccountCard CardOf(const test::TestAccount& account, std::vector<AccountCardDevice> devices, int64_t issued_at,
                   std::string nickname = "alice") {
  AccountCard card{.keys = KeysOf(account), .nickname = std::move(nickname), .devices = std::move(devices),
                   .issued_at = issued_at};
  return SignAccountCard(card, [&account](const ByteVector& bytes) { return account.Sign(bytes); }).value();
}

TEST(AccountCardTest, TheAccountsOwnCardVerifiesAndRoundTrips) {
  const test::TestAccount alice = test::TestAccount::Make();
  const AccountCard card = CardOf(alice, test::Devices({"QmPhone", "QmLaptop"}), kNow);
  EXPECT_TRUE(VerifyAccountCard(card, kNow));
  auto decoded = AccountCardFromJson(AccountCardToJson(card));
  ASSERT_TRUE(decoded) << decoded.error().message;
  EXPECT_EQ(*decoded, card);
  EXPECT_TRUE(VerifyAccountCard(*decoded, kNow));
}

TEST(AccountCardTest, AnythingTheAccountDidNotSignIsRefused) {
  const test::TestAccount alice = test::TestAccount::Make();
  const test::TestAccount mallory = test::TestAccount::Make();
  const AccountCard card = CardOf(alice, test::Devices({"QmPhone"}), kNow);

  AccountCard added_device = card;
  added_device.devices.push_back(test::NamedDevice("QmMallory").Entry());
  EXPECT_FALSE(VerifyAccountCard(added_device, kNow)) << "a device the account did not list";
  AccountCard renamed = card;
  renamed.nickname = "bob";
  EXPECT_FALSE(VerifyAccountCard(renamed, kNow));
  AccountCard other_kem = card;
  other_kem.keys.kem_public_key_b64 = mallory.KemPublicB64();
  EXPECT_FALSE(VerifyAccountCard(other_kem, kNow));
  AccountCard claimed = CardOf(mallory, test::Devices({"QmMallory"}), kNow);
  claimed.keys.account_id = alice.account_id;
  EXPECT_FALSE(VerifyAccountCard(claimed, kNow)) << "Mallory's card claimed for Alice";

  EXPECT_FALSE(VerifyAccountCard(CardOf(alice, test::Devices({"QmA", "QmA"}), kNow), kNow)) << "duplicate device";
  EXPECT_FALSE(VerifyAccountCard(CardOf(alice, test::Devices({"QmA"}), kNow + kAccountCardClockSkewMs + 1), kNow))
      << "from the future";
  EXPECT_FALSE(VerifyAccountCard(CardOf(alice, {}, kNow), kNow)) << "no device";
  std::vector<AccountCardDevice> many;
  for (size_t i = 0; i <= kMaxAccountCardDevices; ++i) {
    many.push_back(test::NamedDevice("Qm" + std::to_string(i)).Entry());
  }
  EXPECT_FALSE(VerifyAccountCard(CardOf(alice, many, kNow), kNow)) << "more than the cap";
  // A device must be the one its keys say: a PeerId that is not its signing key's, signed or not, is refused.
  AccountCardDevice posing = test::NamedDevice("QmA").Entry();
  posing.peer_id = test::PeerIdOf("QmB");
  EXPECT_FALSE(VerifyAccountCard(CardOf(alice, {posing}, kNow), kNow)) << "a PeerId that is not its key's";
  AccountCardDevice bad_kem = test::NamedDevice("QmA").Entry();
  bad_kem.kem_public_key_b64 = Base64Encode(ByteVector(16, 1));
  EXPECT_FALSE(SignAccountCard(AccountCard{.keys = KeysOf(alice), .devices = {bad_kem}, .issued_at = kNow},
                               [&alice](const ByteVector& bytes) { return alice.Sign(bytes); }))
      << "a device KEM key of the wrong size";
}

TEST(AccountCardTest, EveryDeviceKeyIsSigned) {
  const test::TestAccount alice = test::TestAccount::Make();
  const AccountCard card = CardOf(alice, test::Devices({"QmPhone"}), kNow);
  AccountCard other_device_kem = card;
  other_device_kem.devices[0].kem_public_key_b64 = Base64Encode(test::NamedDevice("QmLaptop").kem_public);
  EXPECT_FALSE(VerifyAccountCard(other_device_kem, kNow)) << "a device's KEM key swapped";
  EXPECT_EQ(FindCardDevice(card, test::PeerIdOf("QmPhone"))->kem_public_key_b64,
            Base64Encode(test::NamedDevice("QmPhone").kem_public));
  EXPECT_EQ(FindCardDevice(card, test::PeerIdOf("QmLaptop")), nullptr);
}

TEST(AccountCardTest, DeviceOrderDoesNotChangeTheSignature) {
  const test::TestAccount alice = test::TestAccount::Make();
  AccountCard card = CardOf(alice, test::Devices({"QmB", "QmA"}), kNow);
  std::swap(card.devices[0], card.devices[1]);
  EXPECT_TRUE(VerifyAccountCard(card, kNow));
}

TEST(PeerCardMergeTest, TheNewestCardIsTheDeviceList) {
  const test::TestAccount alice = test::TestAccount::Make();
  PeerCardRecord known =
      MergeAccountCard(std::nullopt, CardOf(alice, test::Devices({"QmPhone"}), kNow, "alice"), "envelope");
  // The newer card is the account's list now (M021): the laptop in, the phone gone.
  known = MergeAccountCard(known, CardOf(alice, test::Devices({"QmLaptop"}), kNow + 10, "Alice L."), "device");
  EXPECT_EQ(known.devices, (std::vector<std::string>{test::PeerIdOf("QmLaptop")}));
  EXPECT_EQ(known.nickname, "Alice L.");
  EXPECT_EQ(known.issued_at, kNow + 10);
  // An older card changes nothing.
  known = MergeAccountCard(known, CardOf(alice, test::Devices({"QmTablet"}), kNow - 10, "old"), "contact_card");
  EXPECT_EQ(known.devices, (std::vector<std::string>{test::PeerIdOf("QmLaptop")}));
  EXPECT_EQ(known.nickname, "Alice L.");
  ASSERT_TRUE(known.newest_card);
  EXPECT_EQ(known.newest_card->issued_at, kNow + 10);
}

TEST(PeerCardMergeTest, AnUndatedKeyCardDoesNotOverrideASignedCard) {
  const test::TestAccount alice = test::TestAccount::Make();
  const test::TestAccount other = test::TestAccount::Make();
  PeerCardRecord known = MergeAccountCard(std::nullopt, CardOf(alice, test::Devices({"QmPhone"}), kNow), "envelope");
  KeyCard different = KeysOf(alice);
  different.kem_public_key_b64 = other.KemPublicB64();  // shape only: merging trusts checked input
  EXPECT_TRUE(SameKeys(MergeKeyCard(known, different, "directory").keys, KeysOf(alice)));
  // With only key cards known, the latest one is taken.
  PeerCardRecord keys_only = MergeKeyCard(std::nullopt, KeysOf(alice), "directory");
  EXPECT_EQ(MergeKeyCard(keys_only, different, "directory").keys, different);
}

TEST(PeerCardStoreTest, KeepsOnlyCheckedCardsAndPersistsTheNewest) {
  const auto dir = std::filesystem::temp_directory_path() / "pp_peer_card_store_test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const std::string db = (dir / "profile.db").string();
  const test::TestAccount alice = test::TestAccount::Make();
  const test::TestAccount mallory = test::TestAccount::Make();
  {
    PeerCardStore store(db);
    KeyCard forged = KeysOf(alice);
    forged.kem_public_key_b64 = mallory.KemPublicB64();
    EXPECT_FALSE(store.AddKeyCard(forged, "directory"));
    AccountCard tampered = CardOf(alice, test::Devices({"QmPhone"}), kNow);
    tampered.devices.push_back(test::NamedDevice("QmMallory").Entry());
    EXPECT_FALSE(store.AddAccountCard(tampered, kNow, "envelope"));
    EXPECT_FALSE(store.Get(alice.account_id).value().has_value());

    ASSERT_TRUE(store.AddKeyCard(KeysOf(alice), "directory"));
    ASSERT_TRUE(store.AddAccountCard(CardOf(alice, test::Devices({"QmPhone"}), kNow), kNow, "envelope"));
    ASSERT_TRUE(
        store.AddAccountCard(CardOf(alice, test::Devices({"QmLaptop", "QmPhone"}), kNow + 1), kNow + 1, "device"));
  }
  PeerCardStore reopened(db);
  auto found = reopened.Get(alice.account_id);
  ASSERT_TRUE(found && found->has_value());
  EXPECT_TRUE(SameKeys((*found)->keys, KeysOf(alice)));
  EXPECT_EQ((*found)->devices, (std::vector<std::string>{test::PeerIdOf("QmLaptop"), test::PeerIdOf("QmPhone")}));
  EXPECT_EQ(reopened.List().value().size(), 1u);
  std::filesystem::remove_all(dir);
}

TEST(PeerCardStoreTest, ARestoredFloorRefusesOlderCardsAndIsBackedUp) {
  const auto dir = std::filesystem::temp_directory_path() / "pp_peer_card_floor_test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const std::string db = (dir / "profile.db").string();
  const test::TestAccount alice = test::TestAccount::Make();
  const test::TestAccount bob = test::TestAccount::Make();
  {
    PeerCardStore store(db);
    int newer = 0;
    store.SetOnNewerCard([&newer]() { ++newer; });
    // Restored: the account locker knew Alice's card issued at kNow + 5 (her stolen phone taken out by then).
    ASSERT_TRUE(store.RaiseFloors({{alice.account_id, kNow + 5}}));
    ASSERT_TRUE(store.RaiseFloors({{alice.account_id, kNow + 1}})) << "a lower floor changes nothing";
    EXPECT_EQ(store.Floor(alice.account_id).value(), kNow + 5);
    EXPECT_EQ(store.Floor(bob.account_id).value(), 0);

    EXPECT_FALSE(store.AddAccountCard(CardOf(alice, test::Devices({"QmStolen", "QmPhone"}), kNow), kNow + 10,
                                      "envelope"))
        << "an older card listing the stolen phone";
    EXPECT_FALSE(store.Get(alice.account_id).value().has_value());
    ASSERT_TRUE(store.AddAccountCard(CardOf(alice, test::Devices({"QmPhone"}), kNow + 5), kNow + 10, "envelope"));
    ASSERT_TRUE(store.AddAccountCard(CardOf(bob, test::Devices({"QmBob"}), kNow + 2), kNow + 10, "envelope"));
    EXPECT_EQ(newer, 2) << "each newer card republishes the locker";
    ASSERT_TRUE(store.AddAccountCard(CardOf(bob, test::Devices({"QmBob"}), kNow + 2), kNow + 10, "envelope"));
    EXPECT_EQ(newer, 2) << "the same card again is not newer";
  }
  PeerCardStore reopened(db);
  std::vector<PeerCardFloor> newest = reopened.NewestKnown().value();
  std::sort(newest.begin(), newest.end(),
            [](const PeerCardFloor& x, const PeerCardFloor& y) { return x.account_id < y.account_id; });
  std::vector<PeerCardFloor> expected{{alice.account_id, kNow + 5}, {bob.account_id, kNow + 2}};
  std::sort(expected.begin(), expected.end(),
            [](const PeerCardFloor& x, const PeerCardFloor& y) { return x.account_id < y.account_id; });
  EXPECT_EQ(newest, expected);
  std::filesystem::remove_all(dir);
}

} // namespace
} // namespace pbr
