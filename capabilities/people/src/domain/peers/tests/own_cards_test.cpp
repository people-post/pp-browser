#include "lib/people/testing/InMemoryPeerCards.h"
#include "domain/peers/OwnCards.h"

#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/HybridKem.h"

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

/** No directory: every lookup fails, so keys come from cards alone. */
class NoDirectory : public mesh::IDirectoryClient {
public:
  Roe<std::vector<mesh::DirectoryHit>> SearchPeople(const std::string& /*query*/) override { return None(); }
  Roe<mesh::DirectoryHit> LookupByAccount(const std::string& /*account_id*/) override { return None(); }

private:
  static Error None() { return Error("Directory not configured"); }
};

/** One device of an account: its identity (own device keys) and its own card store. */
struct Device {
  explicit Device(const std::filesystem::path& dir) : identity(dir.string(), dir.filename().string()) {
    std::filesystem::create_directories(dir);
    EXPECT_TRUE(identity.SetDek(TestDek()));
    EXPECT_TRUE(identity.LoadOrCreate());
    cards = std::make_unique<PeerCards>(rows);
    keys = std::make_unique<AccountKeyResolver>(signing, kem, *cards, directory);
    own = std::make_unique<OwnCards>(identity, *keys);
  }

  /** Becomes a linked device of `other`'s account (the link bundle's account keys; its own device keys stay). */
  void LinkTo(Device& other) { TakeAccountOf(other, kCardIntentJoin); }
  /** Becomes `other`'s account restored from its recovery phrase: a fresh list, this device its master. */
  void RestoreAs(Device& other) { TakeAccountOf(other, kCardIntentFresh); }

  void TakeAccountOf(Device& other, const char* intent) {
    LocalIdentity mine = identity.Get().value();
    const LocalIdentity theirs = other.identity.Get().value();
    mine.account_id = theirs.account_id;
    mine.account_signing_public_key_b64 = theirs.account_signing_public_key_b64;
    mine.account_signing_private_key_b64 = theirs.account_signing_private_key_b64;
    mine.kem_public_key_b64 = theirs.kem_public_key_b64;
    mine.kem_private_key_b64 = theirs.kem_private_key_b64;
    mine.nickname = theirs.nickname;
    mine.card_intent = intent;
    ASSERT_TRUE(identity.Update(mine));
  }

  void Rename(const std::string& nickname) {
    LocalIdentity mine = identity.Get().value();
    mine.nickname = nickname;
    ASSERT_TRUE(identity.Update(mine));
  }

  void SetFloor(int64_t floor) {
    LocalIdentity mine = identity.Get().value();
    mine.initiation_floor = floor;
    ASSERT_TRUE(identity.Update(mine));
  }

  std::string PeerId() { return own->DevicePeerId().value(); }

  IdentityStore identity;
  PeerSigningKeyStore signing;
  PeerKemKeyStore kem;
  NoDirectory directory;
  InMemoryPeerCardStore rows; // the kept rows, as the app's storage holds them
  std::unique_ptr<PeerCards> cards;
  std::unique_ptr<AccountKeyResolver> keys;
  std::unique_ptr<OwnCards> own;
};

class OwnCardsTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ =
        std::filesystem::temp_directory_path() / ("pp_own_cards_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
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

/** Phone (master) and laptop linked to it, both holding the list [phone, laptop]. */
AccountCard LinkLaptop(Device& phone, Device& laptop, const int64_t now) {
  laptop.LinkTo(phone);
  const AccountCard phones = phone.own->Card(now).value();
  EXPECT_TRUE(laptop.own->Learn(phones, now + 10)); // the link bundle's card
  const AccountCard joined = laptop.own->Card(now + 20).value();
  EXPECT_TRUE(phone.own->Learn(joined, now + 30)); // the laptop's notice through the shared mailbox
  return joined;
}

TEST_F(OwnCardsTest, ALinkedDeviceJoinsAtTheEndAndTheMasterStaysFirst) {
  Device phone(dir_ / "phone");
  Device laptop(dir_ / "laptop");
  const AccountCard joined = LinkLaptop(phone, laptop, kNow);
  ASSERT_EQ(joined.devices.size(), 2u);
  EXPECT_EQ(joined.devices[0].peer_id, phone.PeerId()) << "the master stays first (M021 d4)";
  EXPECT_EQ(joined.devices[1].peer_id, laptop.PeerId());
  EXPECT_TRUE(laptop.identity.Get().value().card_intent.empty()) << "joined once";

  // Both now hold one list and re-issue nothing.
  EXPECT_EQ(phone.own->Card(kNow + 40).value(), joined);
  EXPECT_EQ(laptop.own->Card(kNow + 40).value(), joined);
  EXPECT_FALSE(phone.own->SignedOut());
  EXPECT_FALSE(laptop.own->SignedOut());
}

TEST_F(OwnCardsTest, OnlyTheMasterHearsThatADeviceJoined) {
  // M021 d3: the master, which has the contacts, tells them of a new device; the new device does not.
  Device phone(dir_ / "phone");
  Device laptop(dir_ / "laptop");
  int phone_heard = 0;
  int laptop_heard = 0;
  phone.own->SetOnDeviceJoined([&](const AccountCard&) { ++phone_heard; });
  laptop.own->SetOnDeviceJoined([&](const AccountCard&) { ++laptop_heard; });
  const AccountCard joined = LinkLaptop(phone, laptop, kNow);
  EXPECT_EQ(phone_heard, 1);
  EXPECT_EQ(laptop_heard, 0);
  ASSERT_TRUE(phone.own->Learn(joined, kNow + 40));
  EXPECT_EQ(phone_heard, 1) << "the same list again: nothing new";
}

TEST_F(OwnCardsTest, TheMasterSignsADeviceOutAndItCannotSignItselfBackIn) {
  Device phone(dir_ / "phone");
  Device laptop(dir_ / "laptop");
  LinkLaptop(phone, laptop, kNow);
  EXPECT_FALSE(laptop.own->RemoveDevice(phone.PeerId(), kNow + 50)) << "only the master changes the list";

  auto revoked = phone.own->RemoveDevice(laptop.PeerId(), kNow + 60);
  ASSERT_TRUE(revoked) << revoked.error().message;
  ASSERT_EQ(revoked->devices.size(), 1u);
  auto learned = laptop.own->Learn(*revoked, kNow + 70);
  ASSERT_TRUE(learned) << "not an error to hear it";
  EXPECT_TRUE(laptop.own->SignedOut());
  EXPECT_FALSE(laptop.own->Card(kNow + 80)) << "a signed-out device issues no card";
  laptop.Rename("still me");
  EXPECT_FALSE(laptop.own->Card(kNow + 90));
  EXPECT_EQ(phone.own->Card(kNow + 100).value(), *revoked) << "nothing put the laptop back";
}

TEST_F(OwnCardsTest, TheMasterHandsItsRoleOn) {
  Device phone(dir_ / "phone");
  Device laptop(dir_ / "laptop");
  LinkLaptop(phone, laptop, kNow);
  auto handed = phone.own->MakeMaster(laptop.PeerId(), kNow + 50);
  ASSERT_TRUE(handed) << handed.error().message;
  EXPECT_EQ(handed->devices[0].peer_id, laptop.PeerId());
  ASSERT_TRUE(laptop.own->Learn(*handed, kNow + 60));
  EXPECT_FALSE(phone.own->RemoveDevice(laptop.PeerId(), kNow + 70)) << "the phone is no longer the master";
  EXPECT_TRUE(laptop.own->RemoveDevice(phone.PeerId(), kNow + 80));
}

TEST_F(OwnCardsTest, ANonMasterRenameKeepsTheListAsItIs) {
  Device phone(dir_ / "phone");
  Device laptop(dir_ / "laptop");
  const AccountCard joined = LinkLaptop(phone, laptop, kNow);
  laptop.Rename("Alice L.");
  auto renamed = laptop.own->Card(kNow + 50);
  ASSERT_TRUE(renamed);
  EXPECT_EQ(renamed->nickname, "Alice L.");
  EXPECT_EQ(renamed->devices, joined.devices);
  auto on_phone = phone.own->Learn(*renamed, kNow + 60);
  ASSERT_TRUE(on_phone);
  EXPECT_EQ(phone.identity.Get().value().nickname, "Alice L.") << "the account's name, on every device";
}

std::vector<AccountCardRelay> Relays(std::initializer_list<const char*> origins) {
  std::vector<AccountCardRelay> out;
  for (const char* origin : origins) {
    out.push_back(AccountCardRelay{.origin = origin});
  }
  return out;
}

// MR002 / MR005: the card carries the relays we register on and our floor, re-issued when either changes.
TEST_F(OwnCardsTest, TheCardCarriesOurRelaysAndFloorAndIsReissuedWhenEitherChanges) {
  Device phone(dir_ / "phone");
  phone.own->SetRelays({"https://www.brief.global/api/relay", "http://127.0.0.1:8080/api/relay", "",
                        "https://WWW.brief.global:443/api/relay"});
  const AccountCard card = phone.own->Card(kNow).value();
  EXPECT_EQ(card.relays, Relays({"https://www.brief.global/api/relay"}))
      << "only origins within MR004's bounds: no http lab relay, no duplicate";
  EXPECT_EQ(card.initiation_floor, 0);
  EXPECT_TRUE(VerifyAccountCard(card, kNow));
  EXPECT_EQ(phone.own->Card(kNow + 10).value(), card) << "nothing changed";

  phone.SetFloor(250);
  const AccountCard priced = phone.own->Card(kNow + 20).value();
  EXPECT_EQ(priced.initiation_floor, 250);
  EXPECT_GT(priced.issued_at, card.issued_at);
  EXPECT_TRUE(VerifyAccountCard(priced, kNow + 20));

  phone.own->SetRelays({"https://relay.example.org/api/relay"});
  const AccountCard moved = phone.own->Card(kNow + 30).value();
  EXPECT_EQ(moved.relays, Relays({"https://relay.example.org/api/relay"}));
  EXPECT_GT(moved.issued_at, priced.issued_at);
  phone.own->SetRelays({});
  EXPECT_TRUE(phone.own->Card(kNow + 40).value().relays.empty()) << "no relay: the mesh only";
}

// The relay list is the master's (like the device list); the floor is the account's, like the nickname.
TEST_F(OwnCardsTest, ALinkedDeviceKeepsTheMastersRelaysAndTheFloorIsTheAccounts) {
  Device phone(dir_ / "phone");
  Device laptop(dir_ / "laptop");
  phone.own->SetRelays({"https://a.example/api/relay"});
  laptop.own->SetRelays({"https://b.example/api/relay"});
  const AccountCard joined = LinkLaptop(phone, laptop, kNow);
  EXPECT_EQ(joined.relays, Relays({"https://a.example/api/relay"})) << "the laptop is not the master";
  EXPECT_EQ(laptop.own->Card(kNow + 40).value(), joined) << "no tug of war over the list";
  EXPECT_EQ(phone.own->Card(kNow + 40).value(), joined);

  laptop.SetFloor(300);
  const AccountCard priced = laptop.own->Card(kNow + 50).value();
  EXPECT_EQ(priced.initiation_floor, 300);
  EXPECT_EQ(priced.relays, joined.relays);
  ASSERT_TRUE(phone.own->Learn(priced, kNow + 60));
  EXPECT_EQ(phone.identity.Get().value().initiation_floor, 300) << "the account's price, on every device";
  EXPECT_EQ(phone.own->Card(kNow + 70).value(), priced);
}

// profile.db 6 → 7 kept our version 2 card in the version 3 shape, its signature over the old bytes: the first card
// asked for is a new one with the same device list, so no sibling is signed out by the cutover.
TEST_F(OwnCardsTest, AKeptCardThatDoesNotVerifyIsReissuedWithItsList) {
  Device phone(dir_ / "phone");
  Device laptop(dir_ / "laptop");
  const AccountCard joined = LinkLaptop(phone, laptop, kNow);
  PeerCardRecord stepped = phone.rows.Get(joined.keys.account_id).value().value();
  stepped.newest_card->card_signature_b64 = laptop.own->Card(kNow + 40).value().keys.kem_binding_signature_b64;
  ASSERT_TRUE(phone.rows.Put(stepped));

  const AccountCard reissued = phone.own->Card(kNow + 50).value();
  EXPECT_TRUE(VerifyAccountCard(reissued, kNow + 50));
  EXPECT_GT(reissued.issued_at, joined.issued_at);
  EXPECT_EQ(reissued.devices, joined.devices) << "the laptop stays on the list";
  EXPECT_EQ(phone.own->Card(kNow + 60).value(), reissued);
}

TEST_F(OwnCardsTest, ARestoredDeviceStartsAFreshListAndTheOthersAreSignedOut) {
  Device phone(dir_ / "phone");
  Device laptop(dir_ / "laptop");
  const AccountCard old_list = LinkLaptop(phone, laptop, kNow);
  Device restored(dir_ / "restored");
  restored.RestoreAs(phone);
  // An old card may reach it first (a sibling's notice in the mailbox): it still starts a fresh list.
  ASSERT_TRUE(restored.own->Learn(old_list, kNow + 50));
  auto fresh = restored.own->Card(kNow + 60);
  ASSERT_TRUE(fresh) << fresh.error().message;
  ASSERT_EQ(fresh->devices.size(), 1u);
  EXPECT_EQ(fresh->devices[0].peer_id, restored.PeerId());
  EXPECT_GT(fresh->issued_at, old_list.issued_at);

  for (Device* old : {&phone, &laptop}) {
    ASSERT_TRUE(old->own->Learn(*fresh, kNow + 70));
    EXPECT_TRUE(old->own->SignedOut());
  }
  // Their old card, heard again, changes nothing on the new master (it gave out no transfer code).
  ASSERT_TRUE(restored.own->Learn(old_list, kNow + 80));
  EXPECT_EQ(restored.own->Card(kNow + 90).value(), *fresh);
}

TEST_F(OwnCardsTest, AJoinThatReachesTheMasterAfterItsNextCardIsKept) {
  Device phone(dir_ / "phone");
  Device laptop(dir_ / "laptop");
  laptop.LinkTo(phone);
  const AccountCard phones = phone.own->Card(kNow).value();
  phone.own->ExpectJoin(kNow + 15 * 60 * 1000); // the transfer code is out
  ASSERT_TRUE(laptop.own->Learn(phones, kNow + 10));
  const AccountCard joined = laptop.own->Card(kNow + 20).value();
  // Before hearing of the laptop the phone is renamed: a newer card listing only the phone.
  phone.Rename("Alice P.");
  const AccountCard renamed = phone.own->Card(kNow + 30).value();
  ASSERT_EQ(renamed.devices.size(), 1u);

  auto on_phone = phone.own->Learn(joined, kNow + 40);
  ASSERT_TRUE(on_phone);
  ASSERT_EQ(on_phone->devices.size(), 2u) << "the joining laptop is kept";
  EXPECT_EQ(on_phone->nickname, "Alice P.");
  ASSERT_TRUE(laptop.own->Learn(*on_phone, kNow + 50));
  EXPECT_FALSE(laptop.own->SignedOut());
}

TEST_F(OwnCardsTest, AnotherAccountsCardIsNotLearned) {
  Device phone(dir_ / "phone");
  Device stranger(dir_ / "stranger");
  EXPECT_FALSE(phone.own->Learn(stranger.own->Card(kNow).value(), kNow));
}

} // namespace
} // namespace pbr
