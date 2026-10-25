#include "domain/peers/OwnCards.h"

#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/HybridKem.h"
#include "lib/net/SwitchableDirectoryClient.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

namespace pbr {
namespace {

ByteVector TestDek() {
  EnsureSodiumInit();
  return ByteVector(kDataEncryptionKeySize, 7);
}

/** One device of an account: its identity (own device keys) and its own card store. */
struct Device {
  explicit Device(const std::filesystem::path& dir) : identity(dir.string(), dir.filename().string()) {
    std::filesystem::create_directories(dir);
    EXPECT_TRUE(identity.SetDek(TestDek()));
    EXPECT_TRUE(identity.LoadOrCreate());
    cards = std::make_unique<PeerCardStore>((dir / "profile.db").string());
    keys = std::make_unique<AccountKeyResolver>(signing, kem, *cards, directory);
    own = std::make_unique<OwnCards>(identity, *keys);
  }

  /** Becomes a linked device of `other`'s account (the link bundle's account keys; its own device keys stay). */
  void LinkTo(Device& other) {
    LocalIdentity mine = identity.Get().value();
    const LocalIdentity theirs = other.identity.Get().value();
    mine.account_id = theirs.account_id;
    mine.account_signing_public_key_b64 = theirs.account_signing_public_key_b64;
    mine.account_signing_private_key_b64 = theirs.account_signing_private_key_b64;
    mine.kem_public_key_b64 = theirs.kem_public_key_b64;
    mine.kem_private_key_b64 = theirs.kem_private_key_b64;
    mine.nickname = theirs.nickname;
    ASSERT_TRUE(identity.Update(mine));
  }

  void Rename(const std::string& nickname) {
    LocalIdentity mine = identity.Get().value();
    mine.nickname = nickname;
    ASSERT_TRUE(identity.Update(mine));
  }

  std::string PeerId() { return own->DevicePeerId().value(); }

  IdentityStore identity;
  PeerSigningKeyStore signing;
  PeerKemKeyStore kem;
  SwitchableDirectoryClient directory;
  std::unique_ptr<PeerCardStore> cards;
  std::unique_ptr<AccountKeyResolver> keys;
  std::unique_ptr<OwnCards> own;
};

class OwnCardsTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("pp_own_cards_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::filesystem::path dir_;
};

constexpr int64_t kNow = 1'761'235'200'000;

TEST_F(OwnCardsTest, ADeviceHasItsOwnKemKeyApartFromTheAccounts) {
  Device phone(dir_ / "phone");
  const LocalIdentity identity = phone.identity.Get().value();
  EXPECT_FALSE(identity.device_kem_public_key_b64.empty());
  EXPECT_NE(identity.device_kem_public_key_b64, identity.kem_public_key_b64);
  EXPECT_EQ(phone.identity.GetDeviceKemPrivateKey().value().size(), kHybridKemPrivateKeyBytes);
  // Kept across a reload.
  IdentityStore again((dir_ / "phone").string(), "phone");
  ASSERT_TRUE(again.SetDek(TestDek()));
  EXPECT_EQ(again.Get().value().device_kem_public_key_b64, identity.device_kem_public_key_b64);
}

TEST_F(OwnCardsTest, TheCardListsThisDeviceAndIsReissuedOnlyWhenItChanges) {
  Device phone(dir_ / "phone");
  auto card = phone.own->Card(kNow);
  ASSERT_TRUE(card) << card.error().message;
  ASSERT_EQ(card->devices.size(), 1u);
  EXPECT_EQ(card->devices[0], phone.own->Self().value());
  EXPECT_TRUE(VerifyAccountCard(*card, kNow));

  EXPECT_EQ(phone.own->Card(kNow + 1000).value(), *card) << "nothing changed: the same card";
  phone.Rename("alice");
  auto renamed = phone.own->Card(kNow + 2000);
  ASSERT_TRUE(renamed);
  EXPECT_EQ(renamed->nickname, "alice");
  EXPECT_GT(renamed->issued_at, card->issued_at);
  EXPECT_EQ(renamed->devices, card->devices);
}

TEST_F(OwnCardsTest, ALinkedDeviceSignsItselfInAndBothConvergeOnOneList) {
  Device phone(dir_ / "phone");
  Device laptop(dir_ / "laptop");
  laptop.LinkTo(phone);
  const AccountCard phones = phone.own->Card(kNow).value();

  // The link bundle carries the phone's card: the laptop signs itself into it.
  auto learned = laptop.own->Learn(phones, kNow + 10);
  ASSERT_TRUE(learned) << learned.error().message;
  EXPECT_EQ(learned->devices.size(), 2u);
  EXPECT_TRUE(FindCardDevice(*learned, phone.PeerId()));
  EXPECT_TRUE(FindCardDevice(*learned, laptop.PeerId()));
  EXPECT_GT(learned->issued_at, phones.issued_at);

  // The phone takes the laptop's newer card as it is — nothing to add, nothing re-issued.
  auto taken = phone.own->Learn(*learned, kNow + 20);
  ASSERT_TRUE(taken);
  EXPECT_EQ(*taken, *learned);
  EXPECT_EQ(phone.own->Card(kNow + 30).value(), *learned);
  EXPECT_EQ(laptop.own->Card(kNow + 30).value(), *learned);
}

TEST_F(OwnCardsTest, CardsIssuedAtOnceKeepEveryDeviceAndTheNewestName) {
  Device phone(dir_ / "phone");
  Device laptop(dir_ / "laptop");
  laptop.LinkTo(phone);
  const AccountCard both = laptop.own->Learn(phone.own->Card(kNow).value(), kNow + 10).value();
  // Before hearing of the laptop the phone is renamed: its card lists only itself, and is newer.
  phone.Rename("Alice P.");
  const AccountCard phone_only = phone.own->Card(kNow + 20).value();
  ASSERT_EQ(phone_only.devices.size(), 1u);

  // Whichever device learns the other's card, the laptop is kept and the newest name wins.
  auto on_laptop = laptop.own->Learn(phone_only, kNow + 30);
  ASSERT_TRUE(on_laptop);
  EXPECT_EQ(on_laptop->devices.size(), 2u);
  EXPECT_EQ(on_laptop->nickname, "Alice P.");
  EXPECT_EQ(laptop.identity.Get().value().nickname, "Alice P.") << "the account's name, on every device";
  auto on_phone = phone.own->Learn(both, kNow + 40);
  ASSERT_TRUE(on_phone);
  EXPECT_EQ(on_phone->devices.size(), 2u);
  // Then each takes the other's: one list, no more re-issuing.
  auto settled = phone.own->Learn(*on_laptop, kNow + 50);
  ASSERT_TRUE(settled);
  auto settled_too = laptop.own->Learn(*settled, kNow + 60);
  ASSERT_TRUE(settled_too);
  EXPECT_EQ(*settled_too, *settled);
}

TEST_F(OwnCardsTest, AnotherAccountsCardIsNotLearned) {
  Device phone(dir_ / "phone");
  Device stranger(dir_ / "stranger");
  EXPECT_FALSE(phone.own->Learn(stranger.own->Card(kNow).value(), kNow));
}

} // namespace
} // namespace pbr
