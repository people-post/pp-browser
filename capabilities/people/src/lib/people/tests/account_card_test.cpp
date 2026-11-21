// Account card v5 and the key card (docs/contracts/ACCOUNT_CARD.md; release-compatibility SIGNED_FORMATS § 2.2):
// their bytes, their bounds and the receiver's order. The frozen vectors are account_card_vectors_test.cpp's.

#include "lib/people/AccountCard.h"
#include "lib/people/CardKeys.h"
#include "lib/people/PeerCards.h"
#include "lib/people/testing/InMemoryPeerCards.h"

#include "lib/people/tests/TestAccount.h"

#include "common/ValueJson.h"
#include "foundation/crypto/Sha256Hex.h"
#include "foundation/crypto/SignedBytes.h"
#include "foundation/error/AppError.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace pbr {
namespace {

constexpr int64_t kNow = 1'761'235'200'000;
constexpr uint8_t kUnknownAlgId = 0x7f;

KeyCard KeysOf(const test::TestAccount& account) {
  return account.Card();
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

/** Each origin with a relay id of its own. */
std::vector<AccountCardRelay> Relays(std::initializer_list<const char*> origins) {
  std::vector<AccountCardRelay> out;
  uint8_t fill = 0x60;
  for (const char* origin : origins) {
    out.push_back(test::CardRelay(origin, fill++));
  }
  return out;
}

/** A typed key of `size` raw bytes of `fill`, as a card carries it. */
std::string TypedFill(const pp::BlobKind kind, size_t size, uint8_t fill) {
  return WriteTypedBlobB64(kind, ByteVector(size, fill)).value();
}

std::string WithFirstByte(const std::string& b64, const uint8_t first) {
  ByteVector bytes = Base64Decode(b64).value();
  bytes[0] = first;
  return Base64Encode(bytes);
}

pp::ExtArea Entry(const uint16_t type) {
  return pp::ExtArea::FromEntries({{.type = type, .value = {0x01, 0x02, 0x03}}}).value();
}

// RC020 § 2.2: version 5's bytes, computed independently with a struct script: account key 0x01 ‖ 1952 x 0x42, KEM
// 0x01 ‖ 1184 x 0x43, devices (0x01 ‖ 1952 x 0x01, 0x01 ‖ 1184 x 0x02) and (0x01 ‖ 1952 x 0x03, 0x01 ‖ 1184 x 0x04)
// listed out of PeerId order, nickname "alice", relays relay.example.org (relay id 32 x 0x42) then www.brief.global (32
// x 0x01) in list order, initiation_floor 250, issued_at 1761235200000, an empty area (and one entry 0x0101 = 010203).
TEST(AccountCardSignBytesTest, MatchTheGoldenVector) {
  AccountCard card{.keys = KeyCard{.signing_public_key_b64 = TypedFill(pp::BlobKind::kSignaturePublicKey, 1952, 0x42),
                                   .kem_public_key_b64 = TypedFill(pp::BlobKind::kKemPublicKey, 1184, 0x43)},
                   .nickname = "alice",
                   .devices = {{.peer_id = "QmeKYz9h9AozszFqAkRzwxx4xGheEMYRVrLHuh7EMG7Bie",
                                .signing_public_key_b64 = TypedFill(pp::BlobKind::kSignaturePublicKey, 1952, 0x01),
                                .kem_public_key_b64 = TypedFill(pp::BlobKind::kKemPublicKey, 1184, 0x02)},
                               {.peer_id = "QmbSD2vbEJTdL9PW5w5mr3pak9EYyoiYfiqCPJJ6AxJBig",
                                .signing_public_key_b64 = TypedFill(pp::BlobKind::kSignaturePublicKey, 1952, 0x03),
                                .kem_public_key_b64 = TypedFill(pp::BlobKind::kKemPublicKey, 1184, 0x04)}},
                   .relays = {test::CardRelay("https://relay.example.org/api/relay", 0x42),
                              test::CardRelay("https://www.brief.global/api/relay", 0x01)},
                   .initiation_floor = 250,
                   .issued_at = 1761235200000};
  const ByteVector bytes = AccountCardSignBytes(card);
  ASSERT_EQ(bytes.size(), 9797u);
  EXPECT_EQ(Sha256Hex(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())),
            "de85f649dcfa2aec66ff026f82052f6e7cd3b3a3a932d2e17c60e832ff176fcf");
  EXPECT_EQ(SignedBytesVersion(bytes), std::optional<uint8_t>(5));
  AccountCard swapped = card;
  std::swap(swapped.relays[0], swapped.relays[1]);
  EXPECT_NE(AccountCardSignBytes(swapped), bytes) << "the preferred relay is signed as the first";
  AccountCard other_relay = card;
  other_relay.relays[0].relay_id = test::RelayIdOf(0x07);
  EXPECT_NE(AccountCardSignBytes(other_relay), bytes) << "each relay's relay id is signed";
  card.ext = Entry(0x0101);
  const ByteVector with_entry = AccountCardSignBytes(card);
  ASSERT_EQ(with_entry.size(), 9806u);
  EXPECT_EQ(Sha256Hex(std::string_view(reinterpret_cast<const char*>(with_entry.data()), with_entry.size())),
            "db3f3b0af6596756567bd1cc04d065475a9cd3e63c21714985ee1f304619f858")
      << "the area is the last field";
}

TEST(AccountCardTest, RelaysAndFloorAreSignedAndRoundTrip) {
  const test::TestAccount alice = test::TestAccount::Make();
  AccountCard card = CardOf(alice, test::Devices({"QmPhone"}), kNow);
  card.relays = Relays({"https://relay.example.org/api/relay", "https://www.brief.global/api/relay"});
  card.initiation_floor = 500;
  card = Signed(alice, card);
  ASSERT_TRUE(VerifyAccountCard(card, kNow));
  const Object json = AccountCardToJson(card);
  const Array* relays = json.getArray("relays");
  ASSERT_TRUE(relays && relays->elements.size() == 2);
  const Object* first = asObject(relays->elements[0]);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->getString("relay_id"), test::RelayIdOf(0x60).ToString()) << "`relay:` and base64url";
  EXPECT_FALSE(first->contains("peer_id"));
  auto decoded = AccountCardFromJson(json);
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

TEST(AccountCardTest, TheJsonIsVersion5WithTypedKeysAndTheBindingBlob) {
  const test::TestAccount alice = test::TestAccount::Make();
  const Object json = AccountCardToJson(CardOf(alice, test::Devices({"QmPhone"}), kNow));
  EXPECT_EQ(json.getIf<int64_t>("card_version"), 5);
  EXPECT_FALSE(json.contains("ext_b64")) << "an empty area is left out";
  EXPECT_FALSE(json.contains("kem_binding_signature_b64"));
  for (const char* key : {"signing_public_key_b64", "kem_public_key_b64", "card_signature_b64"}) {
    auto bytes = Base64Decode(json.getString(key).value_or(""));
    ASSERT_TRUE(bytes && !bytes->empty()) << key;
    EXPECT_EQ((*bytes)[0], 1) << key << " is typed: ml-dsa-65 / ml-kem-768";
  }
  EXPECT_EQ(Base64Decode(json.getString("kem_binding_b64").value_or("")).value()[0], 2) << "the binding blob's version";
  const Object* device = asObject(json.getArray("devices")->elements[0]);
  EXPECT_EQ(Base64Decode(device->getString("signing_public_key_b64").value_or("")).value().size(), 1953u);
  EXPECT_EQ(Base64Decode(device->getString("kem_public_key_b64").value_or("")).value().size(), 1185u);
}

TEST(AccountCardTest, AnotherVersionOrAMissingFieldIsRefused) {
  const test::TestAccount alice = test::TestAccount::Make();
  const Object json = AccountCardToJson(CardOf(alice, test::Devices({"QmPhone"}), kNow));
  for (const int64_t version : {2, 3, 4}) {
    Object other = json;
    other.set("card_version", version);
    auto read = AccountCardFromJson(other);
    ASSERT_FALSE(read) << "version " << version << " is refused (no bridges)";
    EXPECT_FALSE(AppError::IsNewerFormat(read.error())) << "an older app's card is not 'update needed'";
  }
  for (const char* field : {"relays", "initiation_floor", "kem_binding_b64", "card_signature_b64"}) {
    Object missing = json;
    missing.erase(field);
    EXPECT_FALSE(AccountCardFromJson(missing)) << field;
  }
  Object no_relay_id = json;
  Object relay;
  relay.set("origin", std::string("https://a.example"));
  no_relay_id.set("relays", ArrayValue({ObjectValue(relay)}));
  EXPECT_FALSE(AccountCardFromJson(no_relay_id)) << "a relay without its relay_id";
  Object peer_id = json;
  relay.set("peer_id", std::string("QmeKYz9h9AozszFqAkRzwxx4xGheEMYRVrLHuh7EMG7Bie"));
  peer_id.set("relays", ArrayValue({ObjectValue(relay)}));
  EXPECT_FALSE(AccountCardFromJson(peer_id)) << "card v4's relay entry";
  for (const char* not_a_relay_id : {"QmeKYz9h9AozszFqAkRzwxx4xGheEMYRVrLHuh7EMG7Bie", "relay:", "relay:abc"}) {
    Object bad = json;
    Object row;
    row.set("origin", std::string("https://a.example"));
    row.set("relay_id", std::string(not_a_relay_id));
    bad.set("relays", ArrayValue({ObjectValue(row)}));
    EXPECT_FALSE(AccountCardFromJson(bad)) << not_a_relay_id;
  }
}

// RC020-ORDER (SIGNED_EXTENSIONS § The receiver's order): version, then fields, then algorithm ids, then the
// signature, then critical entries — "update needed" never for an unverified critical entry, never as a forgery.
TEST(AccountCardTest, TheReceiversOrder) {
  const test::TestAccount alice = test::TestAccount::Make();
  const AccountCard card = CardOf(alice, test::Devices({"QmPhone"}), kNow);
  const Object json = AccountCardToJson(card);
  const auto verdict = [](const Object& received) { return ReceiveAccountCard(received, kNow).verdict; };
  EXPECT_EQ(verdict(json), SignedVerdict::Accepted);

  Object newer = json;
  newer.set("card_version", int64_t{6});
  newer.erase("relays"); // a newer card's fields are not read
  EXPECT_EQ(verdict(newer), SignedVerdict::NeedsUpdate);
  EXPECT_TRUE(IsNewerAccountCard(newer));
  EXPECT_TRUE(AppError::IsNewerFormat(AccountCardFromJson(newer).error()));
  Object older = json;
  older.set("card_version", int64_t{4});
  EXPECT_EQ(verdict(older), SignedVerdict::Older);
  EXPECT_FALSE(IsNewerAccountCard(older));
  Object none = json;
  none.erase("card_version");
  EXPECT_EQ(verdict(none), SignedVerdict::Older);

  Object bad_area = json;
  bad_area.set("ext_b64", std::string("AAA=")); // two bytes: too few for an entry's header
  EXPECT_EQ(verdict(bad_area), SignedVerdict::Malformed);

  // An unknown algorithm id anywhere (the account's keys, a device's, the signature): update needed, before verifying.
  for (const char* key : {"signing_public_key_b64", "kem_public_key_b64", "card_signature_b64"}) {
    Object unknown = json;
    unknown.set(key, WithFirstByte(json.getString(key).value(), kUnknownAlgId));
    EXPECT_EQ(verdict(unknown), SignedVerdict::NeedsUpdate) << key;
    EXPECT_TRUE(IsNewerAccountCard(unknown)) << key;
  }
  AccountCard device_unknown = card;
  device_unknown.devices[0].kem_public_key_b64 = WithFirstByte(card.devices[0].kem_public_key_b64, kUnknownAlgId);
  EXPECT_EQ(verdict(AccountCardToJson(Signed(alice, device_unknown))), SignedVerdict::NeedsUpdate)
      << "a device's key of an algorithm this build does not know";

  Object forged = json;
  forged.set("nickname", std::string("mallory"));
  EXPECT_EQ(verdict(forged), SignedVerdict::Forged);

  AccountCard critical = card;
  critical.ext = Entry(0x8101);
  const Object critical_signed = AccountCardToJson(Signed(alice, critical));
  EXPECT_EQ(verdict(critical_signed), SignedVerdict::NeedsUpdate) << "signed, then not understood";
  EXPECT_FALSE(IsNewerAccountCard(critical_signed)) << "the area is read only once the card verifies";
  Object critical_unsigned = json;
  WriteExtB64(critical_unsigned, Entry(0x8101));
  EXPECT_EQ(verdict(critical_unsigned), SignedVerdict::Forged) << "an unverified card raises no 'update needed'";

  AccountCard non_critical = card;
  non_critical.ext = Entry(0x0101);
  const Object kept_json = AccountCardToJson(Signed(alice, non_critical));
  AccountCard kept;
  ASSERT_TRUE(ReceiveAccountCard(kept_json, kNow, &kept).accepted());
  EXPECT_EQ(kept.ext, Entry(0x0101)) << "an unknown non-critical entry is ignored and kept";
  EXPECT_EQ(AccountCardToJson(kept).getString("ext_b64"), kept_json.getString("ext_b64"));

  // Step 6: the key card's binding (its own order), whose key signed.
  AccountCard old_binding = card;
  old_binding.keys.kem_binding_b64 = WithFirstByte(card.keys.kem_binding_b64, 1);
  EXPECT_EQ(verdict(AccountCardToJson(old_binding)), SignedVerdict::Older);
  AccountCard newer_binding = card;
  newer_binding.keys.kem_binding_b64 = WithFirstByte(card.keys.kem_binding_b64, 3);
  EXPECT_EQ(verdict(AccountCardToJson(newer_binding)), SignedVerdict::NeedsUpdate);
}

TEST(AccountCardTest, AKeyCardChecksItsBindingAndItsAccount) {
  const test::TestAccount alice = test::TestAccount::Make();
  const test::TestAccount mallory = test::TestAccount::Make();
  const KeyCard keys = alice.Card();
  EXPECT_TRUE(VerifyKeyCard(keys));
  auto json = KeyCardToJson(keys);
  EXPECT_TRUE(json.contains("kem_binding_b64"));
  EXPECT_FALSE(json.contains("kem_binding_signature_b64"));
  auto read = KeyCardFromJson(json);
  ASSERT_TRUE(read);
  EXPECT_EQ(*read, keys);
  KeyCard claimed = mallory.Card();
  claimed.account_id = alice.account_id;
  EXPECT_FALSE(VerifyKeyCard(claimed)) << "Mallory's keys claimed for Alice";
  KeyCard raw = alice.Card();
  raw.signing_public_key_b64 = alice.SigningPublicB64();
  EXPECT_FALSE(VerifyKeyCard(raw)) << "an untyped key";
  KeyCard unknown = alice.Card();
  unknown.kem_public_key_b64 = WithFirstByte(unknown.kem_public_key_b64, kUnknownAlgId);
  auto unknown_checked = VerifyKeyCard(unknown);
  ASSERT_FALSE(unknown_checked);
  EXPECT_TRUE(AppError::IsNewerFormat(unknown_checked.error()));
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
  EXPECT_TRUE(VerifyAccountCard(with({test::CardRelay(longest, 0x42)}), kNow));

  const std::vector<std::vector<AccountCardRelay>> refused = {
      Relays({"https://r0.example", "https://r1.example", "https://r2.example", "https://r3.example",
              "https://r4.example"}),
      {test::CardRelay(longest + "a", 0x42)},
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
      // SIGNED_FORMATS § 2.2: each relay names a relay id, and no two the same one.
      {AccountCardRelay{.origin = "https://a.example"}},
      {test::CardRelay("https://a.example", 0x42), test::CardRelay("https://b.example", 0x42)},
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
  other_kem.keys.kem_public_key_b64 = mallory.TypedKemPublicB64();
  EXPECT_FALSE(VerifyAccountCard(other_kem, kNow));
  AccountCard claimed = CardOf(mallory, test::Devices({"QmMallory"}), kNow);
  claimed.keys.account_id = alice.account_id;
  EXPECT_FALSE(VerifyAccountCard(claimed, kNow)) << "Mallory's card claimed for Alice";
  EXPECT_EQ(ReceiveAccountCard(AccountCardToJson(claimed), kNow).verdict, SignedVerdict::Forged);

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
  EXPECT_FALSE(VerifyAccountCard(CardOf(alice, {bad_kem}, kNow), kNow)) << "a device KEM key of the wrong size";
  AccountCardDevice untyped = test::NamedDevice("QmA").Entry();
  untyped.signing_public_key_b64 = Base64Encode(test::NamedDevice("QmA").signing_public);
  EXPECT_FALSE(VerifyAccountCard(CardOf(alice, {untyped}, kNow), kNow)) << "a device's raw (untyped) key";
}

TEST(AccountCardTest, EveryDeviceKeyIsSigned) {
  const test::TestAccount alice = test::TestAccount::Make();
  const AccountCard card = CardOf(alice, test::Devices({"QmPhone"}), kNow);
  AccountCard other_device_kem = card;
  other_device_kem.devices[0].kem_public_key_b64 = test::TypedKemB64(test::NamedDevice("QmLaptop").kem_public);
  EXPECT_FALSE(VerifyAccountCard(other_device_kem, kNow)) << "a device's KEM key swapped";
  const AccountCardDevice* phone = FindCardDevice(card, test::PeerIdOf("QmPhone"));
  ASSERT_NE(phone, nullptr);
  EXPECT_EQ(RawKemPublicKey(phone->kem_public_key_b64).value(), test::NamedDevice("QmPhone").kem_public)
      << "the raw key HybridKem takes";
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
  different.kem_public_key_b64 = other.TypedKemPublicB64(); // shape only: merging trusts checked input
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
    forged.kem_public_key_b64 = mallory.TypedKemPublicB64();
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

// A card that names a device the kept one did not (a device linked or restored since) says so, with the account: what
// was sent to the old list (a grant among it) never reached that device.
TEST(PeerCardsTest, ANewerCardWithANewDeviceSaysWhichAccountGainedOne) {
  InMemoryPeerCards cards;
  PeerCards& store = cards;
  const test::TestAccount alice = test::TestAccount::Make();
  std::vector<std::string> gained;
  store.SetOnDevicesAdded([&gained](const std::string& account) { gained.push_back(account); });
  ASSERT_TRUE(store.AddAccountCard(CardOf(alice, test::Devices({"QmPhone"}), kNow), kNow + 10, "envelope"));
  EXPECT_TRUE(gained.empty()) << "the first card: nothing was sent to any list of it yet";
  ASSERT_TRUE(store.AddAccountCard(CardOf(alice, test::Devices({"QmPhone"}), kNow + 1), kNow + 10, "envelope"));
  EXPECT_TRUE(gained.empty()) << "newer, the same devices";
  ASSERT_TRUE(
      store.AddAccountCard(CardOf(alice, test::Devices({"QmPhone", "QmLaptop"}), kNow + 2), kNow + 10, "envelope"));
  EXPECT_EQ(gained, (std::vector<std::string>{alice.account_id}));
  ASSERT_TRUE(store.AddAccountCard(CardOf(alice, test::Devices({"QmLaptop"}), kNow + 3), kNow + 10, "envelope"));
  EXPECT_EQ(gained.size(), 1u) << "a device gone is no new device";
  store.SetOnDevicesAdded(nullptr);
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
