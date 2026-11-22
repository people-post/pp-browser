// Account card v5's and the key card's frozen vectors (release-compatibility SIGNED_FORMATS § 2.2, § 6;
// docs/contracts/ACCOUNT_CARD.md): a signed card of fixed keys with its signed bytes, the same card with one unknown
// non-critical entry (verifies, entry kept), its key card, and the cards that must be refused, each with the verdict
// the receiver's order gives it. The committed file is data/account_card_vectors.json, which pp-relay-apis (R2) reads
// byte for byte: register/finish's `account_card`, stored and served as received. PP_WRITE_ACCOUNT_CARD_VECTORS=<path>
// writes a fresh one (keys from fixed seeds; ML-DSA signatures are randomized, so only they differ between runs).

#include "lib/people/AccountCard.h"
#include "lib/people/CardKeys.h"

#include "common/tests/VectorFile.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/HybridKem.h"
#include "foundation/crypto/KemKeyBinding.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/crypto/SignedBytes.h"
#include "foundation/crypto/TypedBlob.h"
#include "crypto/PeerId.h"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

namespace pbr {
namespace {

using pp::ExtArea;
using pp::kMlDsa65SeedBytes;
using pp::common::Array;
using pp::common::Object;
using pp::common::Value;
using test::VectorFile;
using test::WithoutKeys;

const VectorFile kFile{PP_ACCOUNT_CARD_VECTORS_PATH, "PP_WRITE_ACCOUNT_CARD_VECTORS"};
/** The fields that hold a (randomized) ML-DSA signature. */
const std::set<std::string> kSignedKeys{"card_signature_b64", "kem_binding_b64"};
/** The receivers' clock: the cards are issued at it. */
constexpr int64_t kNowMs = 1'761'235'200'000;
constexpr uint8_t kUnknownAlgId = 0x7f;

struct Keys {
  MlDsaKeyPair signing;
  ByteVector kem;
};

Keys KeysOfSeed(const uint8_t seed, const uint8_t coins) {
  return Keys{.signing = MlDsa::GenerateKeyPairFromSeed(ByteVector(kMlDsa65SeedBytes, seed)).value(),
              .kem = HybridKem::GenerateKeyPairFromSeed(ByteVector(64, coins)).value().public_key};
}

/** The account (seed 0x71, KEM coins 0x72), its two devices (0x73 / 0x74, 0x75 / 0x76), another account (0x77). */
const Keys& Account() {
  static const Keys keys = KeysOfSeed(0x71, 0x72);
  return keys;
}
const Keys& Phone() {
  static const Keys keys = KeysOfSeed(0x73, 0x74);
  return keys;
}
const Keys& Laptop() {
  static const Keys keys = KeysOfSeed(0x75, 0x76);
  return keys;
}
const Keys& Other() {
  static const Keys keys = KeysOfSeed(0x77, 0x78);
  return keys;
}

AccountRawSignFn SignerOf(const Keys& keys) {
  return [&keys](const ByteVector& bytes) -> Roe<std::string> {
    auto signature = MlDsa::Sign(keys.signing.secret_key, bytes);
    if (!signature) {
      return signature.error();
    }
    return Base64Encode(*signature);
  };
}

std::string AccountIdOf(const Keys& keys) {
  return AccountIdFromMlDsaPublicKey(keys.signing.public_key).value();
}

KeyCard KeyCardOf(const Keys& keys) {
  return MakeKeyCard(AccountIdOf(keys), Base64Encode(keys.signing.public_key), Base64Encode(keys.kem), SignerOf(keys))
      .value();
}

AccountCardDevice DeviceOf(const Keys& keys) {
  return AccountCardDevice{
      .peer_id = pp::PeerIdFromMlDsaPublicKey(keys.signing.public_key).value(),
      .signing_public_key_b64 =
          TypedKeyB64(pp::BlobKind::kSignaturePublicKey, Base64Encode(keys.signing.public_key)).value(),
      .kem_public_key_b64 = TypedKeyB64(pp::BlobKind::kKemPublicKey, Base64Encode(keys.kem)).value()};
}

RelayId RelayOf(const uint8_t fill) {
  return RelayId::FromRaw(ByteVector(kRelayIdBytes, fill)).value();
}

/** The fixture: two devices, two relays, a floor and a nickname, issued at kNowMs, signed by `signer` over `ext`. */
AccountCard Fixture(const ExtArea& ext = {}, const Keys& signer = Account()) {
  AccountCard card{.keys = KeyCardOf(Account()),
                   .nickname = "alice",
                   .devices = {DeviceOf(Phone()), DeviceOf(Laptop())},
                   .relays = {{.origin = "https://relay.example.org/api/relay", .relay_id = RelayOf(0x42)},
                              {.origin = "https://www.brief.global/api/relay", .relay_id = RelayOf(0x01)}},
                   .initiation_floor = 250,
                   .issued_at = kNowMs,
                   .ext = ext};
  return SignAccountCard(std::move(card), SignerOf(signer)).value();
}

ExtArea Entry(const uint16_t type) {
  return ExtArea::FromEntries({{.type = type, .value = {0x01, 0x02, 0x03}}}).value();
}

std::string WithFirstByte(const std::string& b64, const uint8_t first) {
  ByteVector bytes = Base64Decode(b64).value();
  bytes[0] = first;
  return Base64Encode(bytes);
}

/** `json` with relay `index`'s `field` set to `value` (or left out when null). */
Object WithRelayField(Object json, const size_t index, const std::string& field,
                      const std::optional<std::string>& value) {
  Array rows = *json.getArray("relays");
  Object row = *asObject(rows.elements[index]);
  if (value) {
    row.set(field, *value);
  } else {
    row.erase(field);
  }
  rows.elements[index] = ObjectValue(std::move(row));
  json.set("relays", ArrayValue(std::move(rows.elements)));
  return json;
}

Object Case(const std::string& name, const Object& card, const bool with_bytes = false) {
  Object row;
  row.set("name", name);
  row.set("card", ObjectValue(card));
  row.set("verdict", std::string(SignedVerdictName(ReceiveAccountCard(card, kNowMs).verdict)));
  if (with_bytes) {
    auto read = AccountCardFromJson(card);
    EXPECT_TRUE(read) << name;
    row.set("sign_bytes_hex", BytesToHex(AccountCardSignBytes(read.valueOr(AccountCard{}))));
  }
  return row;
}

std::vector<Value> Cases() {
  const AccountCard card = Fixture();
  const Object json = AccountCardToJson(card);
  std::vector<Value> rows;
  const auto add = [&rows](Object row) { rows.push_back(ObjectValue(std::move(row))); };
  add(Case("accepted", json, true));
  add(Case("unknown_non_critical_entry_accepted_and_kept", AccountCardToJson(Fixture(Entry(0x0101))), true));

  Object v6 = json;
  v6.set("card_version", int64_t{6});
  add(Case("card_version_6_needs_update", v6));
  Object v4 = json;
  v4.set("card_version", int64_t{4});
  add(Case("card_version_4_older", v4));
  Object no_version = json;
  no_version.erase("card_version");
  add(Case("no_card_version_older", no_version));

  add(Case("unknown_critical_entry_needs_update", AccountCardToJson(Fixture(Entry(0x8101)))));
  Object critical_unsigned = json;
  WriteExtB64(critical_unsigned, Entry(0x8101));
  add(Case("unknown_critical_entry_not_signed_forged", critical_unsigned));
  Object bad_area = json;
  bad_area.set("ext_b64", std::string("AAA="));
  add(Case("ext_b64_not_an_area_malformed", bad_area));

  AccountCard unknown_key = card;
  unknown_key.keys.signing_public_key_b64 = WithFirstByte(card.keys.signing_public_key_b64, kUnknownAlgId);
  add(Case("unknown_signing_key_algorithm_needs_update", AccountCardToJson(unknown_key)));
  AccountCard unknown_device = card;
  unknown_device.devices[1].signing_public_key_b64 =
      WithFirstByte(card.devices[1].signing_public_key_b64, kUnknownAlgId);
  add(Case("unknown_device_key_algorithm_needs_update",
           AccountCardToJson(SignAccountCard(unknown_device, SignerOf(Account())).value())));
  Object unknown_signature = json;
  unknown_signature.set("card_signature_b64", WithFirstByte(card.card_signature_b64, kUnknownAlgId));
  add(Case("unknown_signature_algorithm_needs_update", unknown_signature));
  Object short_key = json;
  ByteVector truncated = Base64Decode(card.keys.signing_public_key_b64).value(); // ml-dsa-65's id, a byte short
  truncated.pop_back();
  short_key.set("signing_public_key_b64", Base64Encode(truncated));
  add(Case("signing_key_of_the_wrong_size_malformed", short_key));

  add(Case("relay_named_by_peer_id_malformed",
           WithRelayField(json, 1, "relay_id", std::string("QmeKYz9h9AozszFqAkRzwxx4xGheEMYRVrLHuh7EMG7Bie"))));
  add(Case("relay_without_relay_id_malformed", WithRelayField(json, 1, "relay_id", std::nullopt)));
  AccountCard same_relay = card;
  same_relay.relays[1].relay_id = same_relay.relays[0].relay_id;
  add(Case("two_relays_one_relay_id_malformed",
           AccountCardToJson(SignAccountCard(same_relay, SignerOf(Account())).value())));
  AccountCard five = card;
  five.relays.clear();
  for (uint8_t i = 0; i < 5; ++i) {
    five.relays.push_back(
        {.origin = "https://r" + std::to_string(i) + ".example", .relay_id = RelayOf(static_cast<uint8_t>(0x10 + i))});
  }
  add(Case("five_relays_malformed", AccountCardToJson(SignAccountCard(five, SignerOf(Account())).value())));
  AccountCard future = card;
  future.issued_at = kNowMs + kAccountCardClockSkewMs + 1;
  add(Case("issued_in_the_future_malformed", AccountCardToJson(SignAccountCard(future, SignerOf(Account())).value())));

  Object renamed = json;
  renamed.set("nickname", std::string("mallory"));
  add(Case("nickname_not_signed_forged", renamed));
  add(Case("signed_by_another_account_forged", AccountCardToJson(Fixture({}, Other()))));
  AccountCard claimed = card;
  claimed.keys = KeyCardOf(Other());
  claimed.keys.account_id = AccountIdOf(Account());
  add(Case("another_accounts_keys_claimed_forged",
           AccountCardToJson(SignAccountCard(claimed, SignerOf(Other())).value())));
  AccountCard old_binding = card;
  old_binding.keys.kem_binding_b64 = WithFirstByte(card.keys.kem_binding_b64, 1);
  add(Case("kem_binding_version_1_older", AccountCardToJson(old_binding)));
  AccountCard other_kem = card;
  other_kem.keys.kem_public_key_b64 = KeyCardOf(Other()).kem_public_key_b64;
  add(Case("kem_key_the_binding_does_not_bind_forged",
           AccountCardToJson(SignAccountCard(other_kem, SignerOf(Account())).value())));
  return rows;
}

Object BuildVectors() {
  Object root;
  root.set("format", "pp-browser account card vectors v1");
  root.set("about",
           "Account card v5 and its key card (release-compatibility SIGNED_FORMATS 2.2, 2.3; docs/contracts/"
           "ACCOUNT_CARD.md): sign_bytes are frozen (hex); each case gives the receiver's verdict (SIGNED_FORMATS "
           "1.1) at now_ms. Signatures are randomized: compare without card_signature_b64 and kem_binding_b64, and "
           "check the committed cards give their verdicts.");
  root.set("domain", kAccountCardDomain);
  root.set("card_version", kAccountCardVersion);
  root.set("now_ms", kNowMs);
  root.set("account_seed_hex", BytesToHex(ByteVector(kMlDsa65SeedBytes, 0x71)));
  root.set("account_kem_coins_hex", BytesToHex(ByteVector(64, 0x72)));
  root.set("device_seeds_hex", ArrayValue({Value(BytesToHex(ByteVector(kMlDsa65SeedBytes, 0x73))),
                                           Value(BytesToHex(ByteVector(kMlDsa65SeedBytes, 0x75)))}));
  root.set("device_kem_coins_hex",
           ArrayValue({Value(BytesToHex(ByteVector(64, 0x74))), Value(BytesToHex(ByteVector(64, 0x76)))}));
  Object key_card;
  key_card.set("key_card", ObjectValue(KeyCardToJson(KeyCardOf(Account()))));
  key_card.set("verdict", "accepted");
  root.set("key_card", ObjectValue(std::move(key_card)));
  root.set("cards", ArrayValue(Cases()));
  return root;
}

TEST(AccountCardVectorsTest, WriteAFreshFileWhenAsked) {
  if (!kFile.WriteRequested()) {
    GTEST_SKIP() << kFile.SkipReason();
  }
  ASSERT_TRUE(kFile.Write(BuildVectors()));
}

// RC020 § 6: the committed file is what the builders make (but for the randomized signatures).
TEST(AccountCardVectorsTest, TheCommittedFileIsWhatTheBuildersMake) {
  EXPECT_EQ(VectorFile::Text(WithoutKeys(kFile.Read(), kSignedKeys)),
            VectorFile::Text(WithoutKeys(BuildVectors(), kSignedKeys)))
      << kFile.RegenerateHint();
}

// RC020 § 6: every committed card gives its verdict, and an accepted one rebuilds its committed bytes and is kept
// as it came (its area too).
TEST(AccountCardVectorsTest, EachCommittedCardGivesItsVerdict) {
  const Object committed = kFile.Read();
  const Array* cases = committed.getArray("cards");
  ASSERT_NE(cases, nullptr);
  ASSERT_FALSE(cases->elements.empty());
  for (const Value& item : cases->elements) {
    const Object* row = asObject(item);
    ASSERT_NE(row, nullptr);
    const std::string name = row->getString("name").value_or("");
    const Object* card_json = row->getObject("card");
    ASSERT_NE(card_json, nullptr) << name;
    AccountCard card;
    const SignedReceipt receipt = ReceiveAccountCard(*card_json, kNowMs, &card);
    EXPECT_EQ(SignedVerdictName(receipt.verdict), row->getString("verdict").value_or(""))
        << name << ": " << receipt.detail;
    if (auto hex = row->getString("sign_bytes_hex")) {
      ASSERT_TRUE(receipt.accepted()) << name;
      EXPECT_EQ(BytesToHex(AccountCardSignBytes(card)), *hex) << name;
      EXPECT_EQ(VectorFile::Text(AccountCardToJson(card)), VectorFile::Text(*card_json)) << name << " kept as received";
    }
  }
  const Object* key_card = committed.getObject("key_card");
  ASSERT_NE(key_card, nullptr);
  auto keys = KeyCardFromJson(*key_card->getObject("key_card"));
  ASSERT_TRUE(keys);
  EXPECT_TRUE(VerifyKeyCard(*keys));
}

} // namespace
} // namespace pbr
