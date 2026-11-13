#include "lib/people/AccountCard.h"
#include "lib/people/PeerCards.h"
#include "lib/people/testing/InMemoryPeerCards.h"

#include "lib/people/tests/TestAccount.h"

#include "foundation/crypto/Sha256Hex.h"

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
  AccountCard card{
      .keys = KeysOf(account), .nickname = std::move(nickname), .devices = std::move(devices), .issued_at = issued_at};
  return SignAccountCard(card, [&account](const ByteVector& bytes) { return account.Sign(bytes); }).value();
}

AccountCard Signed(const test::TestAccount& account, AccountCard card) {
  return SignAccountCard(std::move(card), [&account](const ByteVector& bytes) { return account.Sign(bytes); }).value();
}

std::vector<AccountCardRelay> Relays(std::initializer_list<const char*> origins) {
  std::vector<AccountCardRelay> out;
  for (const char* origin : origins) {
    out.push_back(AccountCardRelay{.origin = origin});
  }
  return out;
}

std::string B64Fill(size_t size, uint8_t fill) {
  return Base64Encode(ByteVector(size, fill));
}

// Ported to the relay (www AccountCard.test.ts, "account card sign bytes"); computed independently with a struct
// script: account key 1952 x 0x42, KEM 1184 x 0x43, devices (1952 x 0x01, 1184 x 0x02) and (1952 x 0x03,
// 1184 x 0x04) listed out of PeerId order, nickname "alice", relays relay.example.org then www.brief.global (list
// order), initiation_floor 250, issued_at 1761235200000.
TEST(AccountCardSignBytesTest, MatchTheGoldenVector) {
  const AccountCard card{
      .keys = KeyCard{.signing_public_key_b64 = B64Fill(1952, 0x42), .kem_public_key_b64 = B64Fill(1184, 0x43)},
      .nickname = "alice",
      .devices = {{.peer_id = "QmeKYz9h9AozszFqAkRzwxx4xGheEMYRVrLHuh7EMG7Bie",
                   .signing_public_key_b64 = B64Fill(1952, 0x01),
                   .kem_public_key_b64 = B64Fill(1184, 0x02)},
                  {.peer_id = "QmbSD2vbEJTdL9PW5w5mr3pak9EYyoiYfiqCPJJ6AxJBig",
                   .signing_public_key_b64 = B64Fill(1952, 0x03),
                   .kem_public_key_b64 = B64Fill(1184, 0x04)}},
      .relays = Relays({"https://relay.example.org/api/relay", "https://www.brief.global/api/relay"}),
      .initiation_floor = 250,
      .issued_at = 1761235200000};
  const ByteVector bytes = AccountCardSignBytes(card);
  ASSERT_EQ(bytes.size(), 9674u);
  EXPECT_EQ(Sha256Hex(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())),
            "8dd1c36b17a5c72edebbd722d8736ca9656f1128b13d9bc01ec300e5c33afe77");
  AccountCard swapped = card;
  std::swap(swapped.relays[0], swapped.relays[1]);
  EXPECT_NE(AccountCardSignBytes(swapped), bytes) << "the preferred relay is signed as the first";
}

TEST(AccountCardTest, RelaysAndFloorAreSignedAndRoundTrip) {
  const test::TestAccount alice = test::TestAccount::Make();
  AccountCard card = CardOf(alice, test::Devices({"QmPhone"}), kNow);
  card.relays = Relays({"https://relay.example.org/api/relay", "https://www.brief.global/api/relay"});
  card.initiation_floor = 500;
  card = Signed(alice, card);
  ASSERT_TRUE(VerifyAccountCard(card, kNow));
  auto decoded = AccountCardFromJson(AccountCardToJson(card));
  ASSERT_TRUE(decoded) << decoded.error().message;
  EXPECT_EQ(*decoded, card);

  AccountCard cheaper = card;
  cheaper.initiation_floor = 0;
  EXPECT_FALSE(VerifyAccountCard(cheaper, kNow)) << "a floor the account did not sign";
  AccountCard reordered = card;
  std::swap(reordered.relays[0], reordered.relays[1]);
  EXPECT_FALSE(VerifyAccountCard(reordered, kNow)) << "another preferred relay";
  AccountCard dropped = card;
  dropped.relays.pop_back();
  EXPECT_FALSE(VerifyAccountCard(dropped, kNow));
}

TEST(AccountCardTest, AnotherVersionOrAMissingFieldIsRefused) {
  const test::TestAccount alice = test::TestAccount::Make();
  const Object json = AccountCardToJson(CardOf(alice, test::Devices({"QmPhone"}), kNow));
  Object v2 = json;
  v2.set("card_version", int64_t{2});
  EXPECT_FALSE(AccountCardFromJson(v2)) << "version 2 is gone (no bridges)";
  for (const char* field : {"relays", "initiation_floor"}) {
    Object missing = json;
    missing.erase(field);
    EXPECT_FALSE(AccountCardFromJson(missing)) << field;
  }
}

TEST(AccountCardTest, ACardBreakingARelayBoundIsRefusedWhole) {
  const test::TestAccount alice = test::TestAccount::Make();
  const auto with = [&alice](std::vector<AccountCardRelay> relays, int64_t floor = 0) {
    AccountCard card{.keys = KeysOf(alice),
                     .devices = test::Devices({"QmPhone"}),
                     .relays = std::move(relays),
                     .initiation_floor = floor,
                     .issued_at = kNow};
    return Signed(alice, std::move(card));
  };
  EXPECT_TRUE(VerifyAccountCard(with({}), kNow)) << "an empty list: the mesh only";
  EXPECT_TRUE(VerifyAccountCard(with(Relays({"https://a.example", "https://b.example:8443/x",
                                             "https://relay.example.org/api/relay", "https://[::1]:8443/r"})),
                                kNow));
  const std::string longest = "https://" + std::string(kMaxCardRelayOriginBytes - 8, 'a');
  EXPECT_TRUE(VerifyAccountCard(with({{.origin = longest}}), kNow));

  const std::vector<std::vector<AccountCardRelay>> refused = {
      Relays({"https://r0.example", "https://r1.example", "https://r2.example", "https://r3.example",
              "https://r4.example"}),
      {{.origin = longest + "a"}},
      Relays({"http://a.example"}),
      Relays({"wss://a.example"}),
      Relays({"https:a.example"}),
      Relays({"/api/relay"}),
      Relays({"https://"}),
      Relays({"https://a.example/x?y=1"}),
      Relays({"https://a.example/x#frag"}),
      Relays({"https://user:pw@a.example"}),
      Relays({"https://a.example/x "}),
      Relays({"https://a.example:port"}),
      Relays({"https://a.example:65536"}),
      Relays({"https://a.example/x", "https://A.example:443/x"}),
      Relays({"https://a.example", "https://a.example/"}),
  };
  for (const auto& relays : refused) {
    EXPECT_FALSE(VerifyAccountCard(with(relays), kNow)) << (relays.empty() ? "" : relays.back().origin);
  }
  EXPECT_FALSE(VerifyAccountCard(with({}, -1), kNow)) << "a negative floor";
  EXPECT_FALSE(VerifyAccountCard(with({}, kMaxCardInitiationFloor + 1), kNow)) << "beyond what the relay reads";
  EXPECT_TRUE(VerifyAccountCard(with({}, kMaxCardInitiationFloor), kNow));
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

TEST(PeerCardMergeTest, TheNewestCardsRelaysAndFloorAreTheAccounts) {
  const test::TestAccount alice = test::TestAccount::Make();
  const auto card = [&alice](int64_t issued_at, const char* relay, int64_t floor) {
    return Signed(alice, AccountCard{.keys = KeysOf(alice),
                                     .devices = test::Devices({"QmPhone"}),
                                     .relays = Relays({relay}),
                                     .initiation_floor = floor,
                                     .issued_at = issued_at});
  };
  PeerCardRecord known = MergeAccountCard(std::nullopt, card(kNow, "https://a.example", 0), "envelope");
  known = MergeAccountCard(known, card(kNow + 10, "https://b.example", 300), "device");
  EXPECT_EQ(KeptRelays(known), Relays({"https://b.example"}));
  EXPECT_EQ(KeptInitiationFloor(known), 300);
  known = MergeAccountCard(known, card(kNow - 10, "https://old.example", 5), "contact_card");
  EXPECT_EQ(KeptRelays(known), Relays({"https://b.example"})) << "an older card changes nothing";
  EXPECT_EQ(KeptInitiationFloor(known), 300);
  // Kept as stored and read back (the store keeps the record's JSON).
  auto reread = PeerCardRecordFromJson(PeerCardRecordToJson(known));
  ASSERT_TRUE(reread);
  EXPECT_EQ(KeptRelays(*reread), Relays({"https://b.example"}));
  EXPECT_EQ(KeptInitiationFloor(*reread), 300);
  // Only a key card known: no word on either.
  const PeerCardRecord keys_only = MergeKeyCard(std::nullopt, KeysOf(alice), "directory");
  EXPECT_TRUE(KeptRelays(keys_only).empty());
  EXPECT_FALSE(KeptInitiationFloor(keys_only));
}

TEST(PeerCardMergeTest, AnUndatedKeyCardDoesNotOverrideASignedCard) {
  const test::TestAccount alice = test::TestAccount::Make();
  const test::TestAccount other = test::TestAccount::Make();
  PeerCardRecord known = MergeAccountCard(std::nullopt, CardOf(alice, test::Devices({"QmPhone"}), kNow), "envelope");
  KeyCard different = KeysOf(alice);
  different.kem_public_key_b64 = other.KemPublicB64(); // shape only: merging trusts checked input
  EXPECT_TRUE(SameKeys(MergeKeyCard(known, different, "directory").keys, KeysOf(alice)));
  // With only key cards known, the latest one is taken.
  PeerCardRecord keys_only = MergeKeyCard(std::nullopt, KeysOf(alice), "directory");
  EXPECT_EQ(MergeKeyCard(keys_only, different, "directory").keys, different);
}

TEST(PeerCardsTest, KeepsOnlyCheckedCardsAndTheNewest) {
  InMemoryPeerCards cards; // that the cards survive a restart is the app storage's (peer_card_store_contract_test)
  const test::TestAccount alice = test::TestAccount::Make();
  const test::TestAccount mallory = test::TestAccount::Make();
  {
    PeerCards& store = cards;
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
  PeerCards& reopened = cards;
  auto found = reopened.Get(alice.account_id);
  ASSERT_TRUE(found && found->has_value());
  EXPECT_TRUE(SameKeys((*found)->keys, KeysOf(alice)));
  EXPECT_EQ((*found)->devices, (std::vector<std::string>{test::PeerIdOf("QmLaptop"), test::PeerIdOf("QmPhone")}));
  EXPECT_EQ(reopened.List().value().size(), 1u);
}

TEST(PeerCardsTest, ARestoredFloorRefusesOlderCardsAndIsBackedUp) {
  InMemoryPeerCards cards; // that the cards survive a restart is the app storage's (peer_card_store_contract_test)
  const test::TestAccount alice = test::TestAccount::Make();
  const test::TestAccount bob = test::TestAccount::Make();
  {
    PeerCards& store = cards;
    int newer = 0;
    store.SetOnNewerCard([&newer]() { ++newer; });
    // Restored: the account locker knew Alice's card issued at kNow + 5 (her stolen phone taken out by then).
    ASSERT_TRUE(store.RaiseFloors({{alice.account_id, kNow + 5}}));
    ASSERT_TRUE(store.RaiseFloors({{alice.account_id, kNow + 1}})) << "a lower floor changes nothing";
    EXPECT_EQ(store.Floor(alice.account_id).value(), kNow + 5);
    EXPECT_EQ(store.Floor(bob.account_id).value(), 0);

    EXPECT_FALSE(
        store.AddAccountCard(CardOf(alice, test::Devices({"QmStolen", "QmPhone"}), kNow), kNow + 10, "envelope"))
        << "an older card listing the stolen phone";
    EXPECT_FALSE(store.Get(alice.account_id).value().has_value());
    ASSERT_TRUE(store.AddAccountCard(CardOf(alice, test::Devices({"QmPhone"}), kNow + 5), kNow + 10, "envelope"));
    ASSERT_TRUE(store.AddAccountCard(CardOf(bob, test::Devices({"QmBob"}), kNow + 2), kNow + 10, "envelope"));
    EXPECT_EQ(newer, 2) << "each newer card republishes the locker";
    ASSERT_TRUE(store.AddAccountCard(CardOf(bob, test::Devices({"QmBob"}), kNow + 2), kNow + 10, "envelope"));
    EXPECT_EQ(newer, 2) << "the same card again is not newer";
  }
  PeerCards& reopened = cards;
  std::vector<PeerCardFloor> newest = reopened.NewestKnown().value();
  std::sort(newest.begin(), newest.end(),
            [](const PeerCardFloor& x, const PeerCardFloor& y) { return x.account_id < y.account_id; });
  std::vector<PeerCardFloor> expected{{alice.account_id, kNow + 5}, {bob.account_id, kNow + 2}};
  std::sort(expected.begin(), expected.end(),
            [](const PeerCardFloor& x, const PeerCardFloor& y) { return x.account_id < y.account_id; });
  EXPECT_EQ(newest, expected);
}

} // namespace
} // namespace pbr
