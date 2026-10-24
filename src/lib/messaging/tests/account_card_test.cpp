#include "lib/messaging/AccountCard.h"
#include "lib/messaging/PeerCardStore.h"

#include "lib/messaging/tests/TestAccount.h"

#include <gtest/gtest.h>

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

AccountCard CardOf(const test::TestAccount& account, std::vector<std::string> devices, int64_t issued_at,
                   std::string nickname = "alice") {
  AccountCard card{.keys = KeysOf(account), .nickname = std::move(nickname), .devices = std::move(devices),
                   .issued_at = issued_at};
  return SignAccountCard(card, [&account](const ByteVector& bytes) { return account.Sign(bytes); }).value();
}

TEST(AccountCardTest, TheAccountsOwnCardVerifiesAndRoundTrips) {
  const test::TestAccount alice = test::TestAccount::Make();
  const AccountCard card = CardOf(alice, {"QmPhone", "QmLaptop"}, kNow);
  EXPECT_TRUE(VerifyAccountCard(card, kNow));
  auto decoded = AccountCardFromJson(AccountCardToJson(card));
  ASSERT_TRUE(decoded) << decoded.error().message;
  EXPECT_EQ(*decoded, card);
  EXPECT_TRUE(VerifyAccountCard(*decoded, kNow));
}

TEST(AccountCardTest, AnythingTheAccountDidNotSignIsRefused) {
  const test::TestAccount alice = test::TestAccount::Make();
  const test::TestAccount mallory = test::TestAccount::Make();
  const AccountCard card = CardOf(alice, {"QmPhone"}, kNow);

  AccountCard added_device = card;
  added_device.devices.push_back("QmMallory");
  EXPECT_FALSE(VerifyAccountCard(added_device, kNow)) << "a device the account did not list";
  AccountCard renamed = card;
  renamed.nickname = "bob";
  EXPECT_FALSE(VerifyAccountCard(renamed, kNow));
  AccountCard other_kem = card;
  other_kem.keys.kem_public_key_b64 = mallory.KemPublicB64();
  EXPECT_FALSE(VerifyAccountCard(other_kem, kNow));
  AccountCard claimed = CardOf(mallory, {"QmMallory"}, kNow);
  claimed.keys.account_id = alice.account_id;
  EXPECT_FALSE(VerifyAccountCard(claimed, kNow)) << "Mallory's card claimed for Alice";

  EXPECT_FALSE(VerifyAccountCard(CardOf(alice, {"QmA", "QmA"}, kNow), kNow)) << "duplicate device";
  EXPECT_FALSE(VerifyAccountCard(CardOf(alice, {"QmA"}, kNow + kAccountCardClockSkewMs + 1), kNow))
      << "from the future";
  std::vector<std::string> many;
  for (size_t i = 0; i <= kMaxAccountCardDevices; ++i) {
    many.push_back("Qm" + std::to_string(i));
  }
  EXPECT_FALSE(VerifyAccountCard(CardOf(alice, many, kNow), kNow));
}

TEST(AccountCardTest, DeviceOrderDoesNotChangeTheSignature) {
  const test::TestAccount alice = test::TestAccount::Make();
  AccountCard card = CardOf(alice, {"QmB", "QmA"}, kNow);
  std::swap(card.devices[0], card.devices[1]);
  EXPECT_TRUE(VerifyAccountCard(card, kNow));
}

TEST(PeerCardMergeTest, NewestCardGivesKeysAndNameWhileDevicesAreTheUnion) {
  const test::TestAccount alice = test::TestAccount::Make();
  // The phone does not know the laptop yet; the laptop's newer card lists both.
  PeerCardRecord known = MergeAccountCard(std::nullopt, CardOf(alice, {"QmPhone"}, kNow, "alice"), "envelope");
  known = MergeAccountCard(known, CardOf(alice, {"QmLaptop"}, kNow + 10, "Alice L."), "device");
  EXPECT_EQ(known.devices, (std::vector<std::string>{"QmLaptop", "QmPhone"}));
  EXPECT_EQ(known.nickname, "Alice L.");
  EXPECT_EQ(known.issued_at, kNow + 10);
  // An older card adds its devices without taking over the name.
  known = MergeAccountCard(known, CardOf(alice, {"QmTablet"}, kNow - 10, "old"), "contact_card");
  EXPECT_EQ(known.devices, (std::vector<std::string>{"QmLaptop", "QmPhone", "QmTablet"}));
  EXPECT_EQ(known.nickname, "Alice L.");
}

TEST(PeerCardMergeTest, AnUndatedKeyCardDoesNotOverrideASignedCard) {
  const test::TestAccount alice = test::TestAccount::Make();
  const test::TestAccount other = test::TestAccount::Make();
  PeerCardRecord known = MergeAccountCard(std::nullopt, CardOf(alice, {"QmPhone"}, kNow), "envelope");
  KeyCard different = KeysOf(alice);
  different.kem_public_key_b64 = other.KemPublicB64();  // shape only: merging trusts checked input
  EXPECT_TRUE(SameKeys(MergeKeyCard(known, different, "directory").keys, KeysOf(alice)));
  // With only key cards known, the latest one is taken.
  PeerCardRecord keys_only = MergeKeyCard(std::nullopt, KeysOf(alice), "directory");
  EXPECT_EQ(MergeKeyCard(keys_only, different, "directory").keys, different);
}

TEST(PeerCardStoreTest, KeepsOnlyCheckedCardsAndPersistsTheMerge) {
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
    AccountCard tampered = CardOf(alice, {"QmPhone"}, kNow);
    tampered.devices.push_back("QmMallory");
    EXPECT_FALSE(store.AddAccountCard(tampered, kNow, "envelope"));
    EXPECT_FALSE(store.Get(alice.account_id).value().has_value());

    ASSERT_TRUE(store.AddKeyCard(KeysOf(alice), "directory"));
    ASSERT_TRUE(store.AddAccountCard(CardOf(alice, {"QmPhone"}, kNow), kNow, "envelope"));
    ASSERT_TRUE(store.AddAccountCard(CardOf(alice, {"QmLaptop"}, kNow + 1), kNow + 1, "device"));
  }
  PeerCardStore reopened(db);
  auto found = reopened.Get(alice.account_id);
  ASSERT_TRUE(found && found->has_value());
  EXPECT_TRUE(SameKeys((*found)->keys, KeysOf(alice)));
  EXPECT_EQ((*found)->devices, (std::vector<std::string>{"QmLaptop", "QmPhone"}));
  EXPECT_EQ(reopened.List().value().size(), 1u);
  std::filesystem::remove_all(dir);
}

} // namespace
} // namespace pbr
