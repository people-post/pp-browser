#include "lib/people/IdentityStore.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/FileCipher.h"
#include "foundation/crypto/KemKeyBinding.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/crypto/ProfileUnlockGate.h"
#include "foundation/crypto/RecoveryPhrase.h"
#include "foundation/crypto/RecoverySeed.h"
#include "crypto/PeerId.h"
#include "common/PlatformLimits.h"
#include "common/ValueJson.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <optional>
#include <sodium.h>
#include "common/PbrCompat.h"

namespace {

using namespace pbr;

ByteVector MakeTestDek() {
  EnsureSodiumInit();
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(i + 1);
  }
  return dek;
}

class IdentityStoreTest : public ::testing::Test {
protected:
  void SetUp() override {
    data_dir_ = std::filesystem::temp_directory_path() /
                ("pp_browser_identity_store_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                 "_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(data_dir_);
  }

  void TearDown() override { std::filesystem::remove_all(data_dir_); }

  std::filesystem::path data_dir_;
};

TEST_F(IdentityStoreTest, CreateHasPeerIdAndIsNotRegistered) {
  IdentityStore store(data_dir_.string(), "test-profile");
  ASSERT_TRUE(store.SetDek(MakeTestDek()));
  auto identity = store.LoadOrCreate();
  ASSERT_TRUE(static_cast<bool>(identity)) << identity.error().message;
  EXPECT_FALSE(identity->peer_id.empty());
  EXPECT_FALSE(identity->registered);
  EXPECT_FALSE(identity->account_id.empty());
  EXPECT_EQ(identity->account_id.rfind("account:", 0), 0u);
  EXPECT_FALSE(identity->account_signing_public_key_b64.empty());
  EXPECT_TRUE(std::filesystem::exists(data_dir_ / "identity.enc"));

  auto public_key = Base64Decode(identity->public_key_b64);
  ASSERT_TRUE(static_cast<bool>(public_key));
  EXPECT_EQ(public_key->size(), kMlDsa65PublicKeyBytes);
  auto derived = pp::PeerIdFromMlDsaPublicKey(*public_key);
  ASSERT_TRUE(static_cast<bool>(derived));
  EXPECT_EQ(identity->peer_id, *derived);
}

TEST_F(IdentityStoreTest, ANewAccountDerivesFromARecoverySeedItKeeps) {
  std::string account_id;
  std::string phrase;
  {
    IdentityStore store(data_dir_.string(), "test-profile");
    ASSERT_TRUE(store.SetDek(MakeTestDek()));
    auto identity = store.LoadOrCreate();
    ASSERT_TRUE(identity);
    account_id = identity->account_id;
    auto seed = store.RecoverySeed();
    ASSERT_TRUE(seed && *seed) << "M023: a new account has a recovery seed";
    phrase = EncodeRecoveryPhrase(**seed).value().Reveal();
  }
  // The phrase alone names the account (as a restore will find it).
  const ByteVector seed = DecodeRecoveryPhrase(phrase).value();
  auto keys = MlDsa::GenerateKeyPairFromSeed(DeriveRecoveryKeys(seed)->account_ml_dsa_seed);
  ASSERT_TRUE(keys);
  EXPECT_EQ(AccountIdFromMlDsaPublicKey(keys->public_key).value(), account_id);

  // Kept across a restart, under the DEK.
  IdentityStore reloaded(data_dir_.string(), "test-profile");
  ASSERT_TRUE(reloaded.SetDek(MakeTestDek()));
  EXPECT_EQ(reloaded.RecoverySeed().value(), std::optional<ByteVector>(seed));
}

TEST_F(IdentityStoreTest, DerivesSamePeerIdAcrossReload) {
  std::string peer_id;
  std::string account_id;
  {
    IdentityStore store(data_dir_.string(), "test-profile");
    ASSERT_TRUE(store.SetDek(MakeTestDek()));
    auto identity = store.LoadOrCreate();
    ASSERT_TRUE(static_cast<bool>(identity));
    peer_id = identity->peer_id;
    account_id = identity->account_id;
  }
  EXPECT_TRUE(std::filesystem::exists(data_dir_ / "identity.enc"));
  EXPECT_FALSE(std::filesystem::exists(data_dir_ / "identity.json"));
  IdentityStore reloaded(data_dir_.string(), "test-profile");
  ASSERT_TRUE(reloaded.SetDek(MakeTestDek()));
  auto identity = reloaded.Get();
  ASSERT_TRUE(static_cast<bool>(identity));
  EXPECT_EQ(identity->peer_id, peer_id);
  EXPECT_EQ(identity->account_id, account_id);
  auto got_account = reloaded.GetAccountId();
  ASSERT_TRUE(static_cast<bool>(got_account));
  EXPECT_EQ(*got_account, account_id);
}

TEST_F(IdentityStoreTest, KeepsTheRegistration) {
  IdentityStore store(data_dir_.string(), "test-profile");
  ASSERT_TRUE(store.SetDek(MakeTestDek()));
  auto created = store.LoadOrCreate();
  ASSERT_TRUE(static_cast<bool>(created));
  LocalIdentity identity = *created;
  identity.nickname = "alice";
  identity.registered = true;
  identity.brief_llm_api_key = "brf_llm_testkeyABCDEFGHIJKLMNOP";
  identity.brief_llm_token_expires_at = "2099-01-08T00:00:00.000Z";
  identity.brief_llm_guest_api_key = "brf_guest_testkeyABCDEFGHIJKLMNOP";
  identity.registered = true;
  identity.registration_expires_at = "2099-06-01T00:00:00.000Z";
  identity.registered_backend_url = "https://relay.example.test";
  auto updated = store.Update(identity);
  ASSERT_TRUE(static_cast<bool>(updated)) << updated.error().message;

  IdentityStore reloaded(data_dir_.string(), "test-profile");
  ASSERT_TRUE(reloaded.SetDek(MakeTestDek()));
  auto loaded = reloaded.LoadOrCreate();
  ASSERT_TRUE(static_cast<bool>(loaded)) << loaded.error().message;
  EXPECT_EQ(loaded->brief_llm_api_key, "brf_llm_testkeyABCDEFGHIJKLMNOP");
  EXPECT_EQ(loaded->brief_llm_token_expires_at, "2099-01-08T00:00:00.000Z") << "renewed before it, RC018 item 2";
  EXPECT_EQ(loaded->brief_llm_guest_api_key, "brf_guest_testkeyABCDEFGHIJKLMNOP");
  EXPECT_TRUE(loaded->registered);
  EXPECT_EQ(loaded->registration_expires_at, "2099-06-01T00:00:00.000Z");
  EXPECT_EQ(loaded->registered_backend_url, "https://relay.example.test");
  EXPECT_FALSE(loaded->peer_id.empty());
}

TEST_F(IdentityStoreTest, RequiresDek) {
  IdentityStore store(data_dir_.string(), "test-profile");
  auto identity = store.LoadOrCreate();
  ASSERT_FALSE(static_cast<bool>(identity));
}

TEST_F(IdentityStoreTest, RejectsLegacyEd25519DeviceKeys) {
  const ByteVector dek = MakeTestDek();
  ByteVector fake_pk(32, 0x11);
  ByteVector fake_sk(32, 0x22);

  std::filesystem::create_directories(data_dir_);
  Object legacy;
  legacy.set("schema_version", static_cast<int64_t>(2));
  legacy.set("public_key_b64", Base64Encode(fake_pk));
  legacy.set("private_key_b64", Base64Encode(fake_sk));
  legacy.set("nickname", "legacy");
  legacy.set("relay_user_id", "");
  legacy.set("brief_llm_api_key", "");
  legacy.set("registered", false);
  legacy.set("registration_expires_at", "");
  const std::string json = DumpJson(legacy, 2);
  const ByteVector plaintext(json.begin(), json.end());
  const std::string aad = FileCipher::BuildAad("identity", "test-profile");
  auto ciphertext = FileCipher::Encrypt(dek, plaintext, aad);
  ASSERT_TRUE(static_cast<bool>(ciphertext)) << ciphertext.error().message;
  {
    std::ofstream out(data_dir_ / "identity.enc", std::ios::binary);
    ASSERT_TRUE(static_cast<bool>(out));
    out.write(reinterpret_cast<const char*>(ciphertext->data()), static_cast<std::streamsize>(ciphertext->size()));
  }

  IdentityStore store(data_dir_.string(), "test-profile");
  ASSERT_TRUE(store.SetDek(dek));
  auto identity = store.LoadOrCreate();
  ASSERT_FALSE(static_cast<bool>(identity));
  EXPECT_NE(identity.error().message.find("wipe"), std::string::npos);
}

TEST_F(IdentityStoreTest, WritesSchemaVersionOnCreate) {
  IdentityStore store(data_dir_.string(), "test-profile");
  ASSERT_TRUE(store.SetDek(MakeTestDek()));
  ASSERT_TRUE(store.LoadOrCreate());

  const ByteVector dek = MakeTestDek();
  const std::string aad = FileCipher::BuildAad("identity", "test-profile");
  std::ifstream in(data_dir_ / "identity.enc", std::ios::binary);
  ASSERT_TRUE(static_cast<bool>(in));
  ByteVector blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  auto plain = FileCipher::Decrypt(dek, blob, aad);
  ASSERT_TRUE(static_cast<bool>(plain)) << plain.error().message;
  const std::string text(plain->begin(), plain->end());
  auto root = TryParseObject(text);
  ASSERT_TRUE(static_cast<bool>(root));
  ASSERT_TRUE(root->contains("schema_version"));
  EXPECT_EQ(static_cast<int>(root->getIf<int64_t>("schema_version").value_or(-1)), IdentityStore::kSchemaVersion);
}

// identity.enc's floor is release 1's schema, 4 (release-compatibility RC017: every tester reinstalls): an older file
// is refused, not stepped up, and left as it is; the profile is wiped.
TEST_F(IdentityStoreTest, AFileBelowTheFloorIsRefusedAndLeftAsItIs) {
  const ByteVector dek = MakeTestDek();
  const std::string aad = FileCipher::BuildAad("identity", "test-profile");
  {
    IdentityStore created(data_dir_.string(), "test-profile");
    ASSERT_TRUE(created.SetDek(dek));
    ASSERT_TRUE(static_cast<bool>(created.LoadOrCreate()));
  }
  std::ifstream in(data_dir_ / "identity.enc", std::ios::binary);
  ByteVector blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  auto plain = FileCipher::Decrypt(dek, blob, aad);
  ASSERT_TRUE(static_cast<bool>(plain));
  Object v3 = TryParseObject(std::string(plain->begin(), plain->end())).value();
  v3.set("schema_version", static_cast<int64_t>(IdentityStore::kFloorSchemaVersion - 1));
  const std::string json = DumpJson(v3, 2);
  auto ciphertext = FileCipher::Encrypt(dek, ByteVector(json.begin(), json.end()), aad);
  ASSERT_TRUE(static_cast<bool>(ciphertext));
  {
    std::ofstream out(data_dir_ / "identity.enc", std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(ciphertext->data()), static_cast<std::streamsize>(ciphertext->size()));
  }

  IdentityStore store(data_dir_.string(), "test-profile");
  ASSERT_TRUE(store.SetDek(dek));
  auto identity = store.Get();
  ASSERT_FALSE(static_cast<bool>(identity));
  EXPECT_NE(identity.error().message.find("wipe"), std::string::npos) << identity.error().message;
  store.Flush();
  std::ifstream again(data_dir_ / "identity.enc", std::ios::binary);
  const ByteVector kept((std::istreambuf_iterator<char>(again)), std::istreambuf_iterator<char>());
  EXPECT_EQ(kept, *ciphertext) << "not written over";
}

TEST_F(IdentityStoreTest, RejectsNewerSchemaVersion) {
  const ByteVector dek = MakeTestDek();
  auto keys = MlDsa::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(keys));

  std::filesystem::create_directories(data_dir_);
  Object newer;
  newer.set("schema_version", static_cast<int64_t>(IdentityStore::kSchemaVersion + 1));
  newer.set("public_key_b64", Base64Encode(keys->public_key));
  newer.set("private_key_b64", Base64Encode(keys->secret_key));
  newer.set("nickname", "future");
  newer.set("registered", false);
  const std::string json = DumpJson(newer, 2);
  const ByteVector plaintext(json.begin(), json.end());
  const std::string aad = FileCipher::BuildAad("identity", "test-profile");
  auto ciphertext = FileCipher::Encrypt(dek, plaintext, aad);
  ASSERT_TRUE(static_cast<bool>(ciphertext));
  {
    std::ofstream out(data_dir_ / "identity.enc", std::ios::binary);
    ASSERT_TRUE(static_cast<bool>(out));
    out.write(reinterpret_cast<const char*>(ciphertext->data()), static_cast<std::streamsize>(ciphertext->size()));
  }

  IdentityStore store(data_dir_.string(), "test-profile");
  ASSERT_TRUE(store.SetDek(dek));
  auto identity = store.Get();
  ASSERT_FALSE(static_cast<bool>(identity));
  EXPECT_NE(identity.error().message.find("schema"), std::string::npos);
}

TEST_F(IdentityStoreTest, LoadsIdentityAtProfileJsonFileSizeLimit) {
  const ByteVector dek = MakeTestDek();
  IdentityStore created(data_dir_.string(), "test-profile");
  ASSERT_TRUE(created.SetDek(dek));
  ASSERT_TRUE(created.LoadOrCreate());

  const std::string aad = FileCipher::BuildAad("identity", "test-profile");
  std::ifstream in(data_dir_ / "identity.enc", std::ios::binary);
  ASSERT_TRUE(static_cast<bool>(in));
  ByteVector blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  auto plaintext = FileCipher::Decrypt(dek, blob, aad);
  ASSERT_TRUE(static_cast<bool>(plaintext)) << plaintext.error().message;
  auto root = TryParseObject(std::string(plaintext->begin(), plaintext->end()));
  ASSERT_TRUE(static_cast<bool>(root));

  root->set("nickname", "");
  const std::string base_json = DumpJson(*root, 2);
  auto encrypted = FileCipher::Encrypt(dek, ByteVector(base_json.begin(), base_json.end()), aad);
  ASSERT_TRUE(static_cast<bool>(encrypted)) << encrypted.error().message;
  ASSERT_LE(encrypted->size(), kMaxProfileJsonFileBytes);
  const size_t nickname_size = kMaxProfileJsonFileBytes - encrypted->size();
  root->set("nickname", std::string(nickname_size, 'x'));
  const std::string json = DumpJson(*root, 2);
  encrypted = FileCipher::Encrypt(dek, ByteVector(json.begin(), json.end()), aad);
  ASSERT_TRUE(static_cast<bool>(encrypted)) << encrypted.error().message;
  ASSERT_EQ(encrypted->size(), kMaxProfileJsonFileBytes);
  {
    std::ofstream out(data_dir_ / "identity.enc", std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(static_cast<bool>(out));
    out.write(reinterpret_cast<const char*>(encrypted->data()), static_cast<std::streamsize>(encrypted->size()));
  }

  IdentityStore reloaded(data_dir_.string(), "test-profile");
  ASSERT_TRUE(reloaded.SetDek(dek));
  auto identity = reloaded.LoadOrCreate();
  ASSERT_TRUE(static_cast<bool>(identity)) << identity.error().message;
  EXPECT_EQ(identity->nickname.size(), nickname_size);
}

TEST_F(IdentityStoreTest, RejectsIdentityOverProfileJsonFileSizeLimit) {
  std::filesystem::create_directories(data_dir_);
  std::ofstream out(data_dir_ / "identity.enc", std::ios::binary);
  ASSERT_TRUE(static_cast<bool>(out));
  std::string oversized(kMaxProfileJsonFileBytes + 1, 'x');
  out.write(oversized.data(), static_cast<std::streamsize>(oversized.size()));
  out.close();

  IdentityStore store(data_dir_.string(), "test-profile");
  ASSERT_TRUE(store.SetDek(MakeTestDek()));
  auto identity = store.LoadOrCreate();
  ASSERT_FALSE(static_cast<bool>(identity));
  EXPECT_NE(identity.error().message.find("exceeds limit"), std::string::npos);
}

TEST_F(IdentityStoreTest, SeededMintIsDeterministicAndFailClosed) {
  ByteVector seed(32, 0x55);
  std::string peer_id;
  std::string account_id;
  {
    IdentityStore store(data_dir_.string(), "test-profile");
    ASSERT_TRUE(store.SetDek(MakeTestDek()));
    ASSERT_TRUE(store.SetIdentitySeed(seed));
    auto identity = store.LoadOrCreate();
    ASSERT_TRUE(static_cast<bool>(identity)) << identity.error().message;
    peer_id = identity->peer_id;
    account_id = identity->account_id;
  }
  {
    IdentityStore reloaded(data_dir_.string(), "test-profile");
    ASSERT_TRUE(reloaded.SetDek(MakeTestDek()));
    ASSERT_TRUE(reloaded.SetIdentitySeed(seed));
    auto identity = reloaded.LoadOrCreate();
    ASSERT_TRUE(static_cast<bool>(identity)) << identity.error().message;
    EXPECT_EQ(identity->peer_id, peer_id);
    EXPECT_EQ(identity->account_id, account_id);
  }
  {
    IdentityStore mismatch(data_dir_.string(), "test-profile");
    ASSERT_TRUE(mismatch.SetDek(MakeTestDek()));
    ByteVector other(32, 0x66);
    ASSERT_TRUE(mismatch.SetIdentitySeed(other));
    auto identity = mismatch.LoadOrCreate();
    ASSERT_FALSE(static_cast<bool>(identity));
    EXPECT_NE(identity.error().message.find("fail-closed"), std::string::npos);
  }
  // Wipe + same seed remints identical ids
  std::filesystem::remove_all(data_dir_);
  IdentityStore remint(data_dir_.string(), "test-profile");
  ASSERT_TRUE(remint.SetDek(MakeTestDek()));
  ASSERT_TRUE(remint.SetIdentitySeed(seed));
  auto again = remint.LoadOrCreate();
  ASSERT_TRUE(static_cast<bool>(again)) << again.error().message;
  EXPECT_EQ(again->peer_id, peer_id);
  EXPECT_EQ(again->account_id, account_id);
}

// A write that fails leaves the identity in memory as it was: Get() never shows an unsaved field (a guest key that
// would stop a re-mint, then reach the disk with a later unrelated save).
TEST_F(IdentityStoreTest, AFailedSaveLeavesTheIdentityAsItWas) {
  IdentityStore store(data_dir_.string(), "test-profile");
  ASSERT_TRUE(store.SetDek(MakeTestDek()));
  auto created = store.LoadOrCreate();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;

  // identity.enc can no longer be replaced: a non-empty directory where the file was.
  const std::filesystem::path file = data_dir_ / "identity.enc";
  std::filesystem::remove(file);
  std::filesystem::create_directories(file);
  {
    std::ofstream(file / "keep") << "x";
  }

  auto modified = store.Modify([](LocalIdentity& identity) {
    identity.brief_llm_guest_api_key = "brf_guest_unsaved";
    return true;
  });
  EXPECT_FALSE(static_cast<bool>(modified));
  EXPECT_TRUE(store.Get().value().brief_llm_guest_api_key.empty()) << "Modify: nothing half-applied";

  LocalIdentity changed = store.Get().value();
  changed.nickname = "unsaved-name";
  EXPECT_FALSE(static_cast<bool>(store.Update(changed)));
  EXPECT_EQ(store.Get().value().nickname, created->nickname) << "Update: nothing half-applied";

  std::filesystem::remove_all(file);
  store.Flush(); // nothing pending: the failed writes left nothing to flush
  EXPECT_FALSE(std::filesystem::exists(file));

  ASSERT_TRUE(static_cast<bool>(store.Modify([](LocalIdentity& identity) {
    identity.brief_llm_guest_api_key = "brf_guest_saved";
    return true;
  })));
  EXPECT_EQ(store.Get().value().brief_llm_guest_api_key.Reveal(), "brf_guest_saved");
  EXPECT_TRUE(std::filesystem::exists(file));
}

} // namespace

namespace {

/** Our account's card listing this device, `nickname` and `issued_at` as given, signed by the account. */
pbr::AccountCard OwnCardOf(pbr::IdentityStore& store, const std::string& nickname, int64_t issued_at,
                           const std::string& account_id = {}) {
  const pbr::LocalIdentity identity = store.Get().value();
  pbr::AccountCard card;
  card.keys.account_id = account_id.empty() ? identity.account_id : account_id;
  card.keys.signing_public_key_b64 = identity.account_signing_public_key_b64;
  card.keys.kem_public_key_b64 = identity.kem_public_key_b64;
  card.keys.kem_binding_signature_b64 =
      store.SignBytes(pbr::KemBindingSignBytes(identity.account_signing_public_key_b64, identity.kem_public_key_b64))
          .value();
  card.devices = {pbr::AccountCardDevice{.peer_id = identity.peer_id,
                                         .signing_public_key_b64 = identity.public_key_b64,
                                         .kem_public_key_b64 = identity.device_kem_public_key_b64}};
  card.nickname = nickname;
  card.issued_at = issued_at;
  return pbr::SignAccountCard(card, [&store](const pbr::ByteVector& bytes) { return store.SignBytes(bytes); }).value();
}

TEST_F(IdentityStoreTest, ANewAccountIsMarkedToStartItsDeviceList) {
  IdentityStore store(data_dir_.string(), "test-profile");
  ASSERT_TRUE(store.SetDek(MakeTestDek()));
  EXPECT_EQ(store.LoadOrCreate().value().card_intent, kCardIntentFresh);
  IdentityStore again(data_dir_.string(), "test-profile");
  ASSERT_TRUE(again.SetDek(MakeTestDek()));
  EXPECT_EQ(again.Get().value().card_intent, kCardIntentFresh) << "kept until its first card";
}

TEST_F(IdentityStoreTest, OurCardIsSealedBesideTheIdentityAndOnlyANewerOneReplacesIt) {
  IdentityStore store(data_dir_.string(), "test-profile");
  ASSERT_TRUE(store.SetDek(MakeTestDek()));
  ASSERT_TRUE(store.LoadOrCreate());
  EXPECT_FALSE(store.OwnCard().value()) << "none yet";

  const AccountCard card = OwnCardOf(store, "alice-sealed", 100);
  ASSERT_TRUE(store.KeepOwnCard(card));
  std::string raw;
  {
    // Closed before the file is replaced below: Windows refuses to replace an open file.
    std::ifstream in(data_dir_ / "own_card.enc", std::ios::binary);
    raw.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  EXPECT_FALSE(raw.empty());
  EXPECT_EQ(raw.find("alice-sealed"), std::string::npos) << "under the DEK, not plaintext";

  IdentityStore reloaded(data_dir_.string(), "test-profile");
  ASSERT_TRUE(reloaded.SetDek(MakeTestDek()));
  EXPECT_EQ(reloaded.OwnCard().value(), card);

  ASSERT_TRUE(reloaded.KeepOwnCard(OwnCardOf(store, "older", 50)));
  EXPECT_EQ(reloaded.OwnCard().value()->nickname, "alice-sealed") << "no older card of ours is taken";
  ASSERT_TRUE(reloaded.KeepOwnCard(OwnCardOf(store, "newer", 200)));
  EXPECT_EQ(reloaded.OwnCard().value()->nickname, "newer");

  EXPECT_FALSE(reloaded.KeepOwnCard(OwnCardOf(store, "other", 300, "account:someone-else")))
      << "another account's card is refused";
  EXPECT_EQ(reloaded.OwnCard().value()->nickname, "newer");
}

// INVENTORY S1: own_card.enc from a newer app is refused, and a card of ours is never written over it (a downgrade
// would lose what the newer app kept there); the card is read from the chat store meanwhile.
TEST_F(IdentityStoreTest, ANewerOwnCardFileIsRefusedAndNeverWrittenOver) {
  IdentityStore store(data_dir_.string(), "test-profile");
  ASSERT_TRUE(store.SetDek(MakeTestDek()));
  ASSERT_TRUE(store.LoadOrCreate());
  Object newer;
  newer.set("schema_version", static_cast<int64_t>(IdentityStore::kOwnCardSchemaVersion + 1));
  newer.set("cards", ArrayValue({}));
  const std::string json = DumpJson(newer, 2);
  auto sealed = FileCipher::Encrypt(MakeTestDek(), ByteVector(json.begin(), json.end()),
                                    FileCipher::BuildAad("own-card", "test-profile"));
  ASSERT_TRUE(static_cast<bool>(sealed));
  {
    std::ofstream out(data_dir_ / "own_card.enc", std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(sealed->data()), static_cast<std::streamsize>(sealed->size()));
  }

  IdentityStore reloaded(data_dir_.string(), "test-profile");
  ASSERT_TRUE(reloaded.SetDek(MakeTestDek()));
  auto own = reloaded.OwnCard();
  ASSERT_FALSE(static_cast<bool>(own));
  EXPECT_NE(own.error().message.find("schema"), std::string::npos) << own.error().message;
  auto kept = reloaded.KeepOwnCard(OwnCardOf(store, "alice", 100));
  ASSERT_FALSE(static_cast<bool>(kept));
  EXPECT_NE(kept.error().message.find("newer"), std::string::npos) << kept.error().message;
  std::ifstream in(data_dir_ / "own_card.enc", std::ios::binary);
  const ByteVector on_disk((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(on_disk, *sealed) << "the newer file is left as it was";
}

TEST_F(IdentityStoreTest, ANewDekForALinkOrRestoreDropsTheOldAccountsCard) {
  IdentityStore store(data_dir_.string(), "test-profile");
  ASSERT_TRUE(store.SetDek(MakeTestDek()));
  ASSERT_TRUE(store.LoadOrCreate());
  ASSERT_TRUE(store.KeepOwnCard(OwnCardOf(store, "alice", 100)));
  ByteVector other_dek = MakeTestDek();
  other_dek[0] ^= 0xff;
  ASSERT_TRUE(store.ReplaceDekKeepLoaded(other_dek));
  EXPECT_FALSE(std::filesystem::exists(data_dir_ / "own_card.enc"));
  EXPECT_FALSE(store.OwnCard().value());
}

// The account data a build can never open, whatever the PIN (Windows: an October identity.enc, schema 3, refused at
// every unlock and shown as a PIN error): the unlock gate tells it apart by its kind, never by its text.
class IdentityStoreUnopenableTest : public IdentityStoreTest {
protected:
  void WriteIdentityFile(const ByteVector& bytes) {
    std::filesystem::create_directories(data_dir_);
    std::ofstream out(data_dir_ / "identity.enc", std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(static_cast<bool>(out));
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  } // closed before the store reads it

  void WriteIdentityJson(const Object& root, const ByteVector& dek) {
    const std::string json = DumpJson(root, 2);
    auto ciphertext = FileCipher::Encrypt(dek, ByteVector(json.begin(), json.end()),
                                          FileCipher::BuildAad("identity", "test-profile"));
    ASSERT_TRUE(static_cast<bool>(ciphertext)) << ciphertext.error().message;
    WriteIdentityFile(*ciphertext);
  }

  std::optional<ProfileOpenProblem> ProblemOnLoad() {
    IdentityStore store(data_dir_.string(), "test-profile");
    EXPECT_TRUE(store.SetDek(MakeTestDek()));
    auto loaded = store.LoadOrCreate();
    EXPECT_FALSE(static_cast<bool>(loaded));
    if (loaded) {
      return std::nullopt;
    }
    return ProfileUnlockGate::ProfileProblemOf(loaded.error());
  }
};

TEST_F(IdentityStoreUnopenableTest, AnIdentityFromBeforeRelease1CannotBeOpened) {
  Object old;
  old.set("schema_version", static_cast<int64_t>(3));
  old.set("nickname", "october");
  WriteIdentityJson(old, MakeTestDek());
  EXPECT_EQ(ProblemOnLoad(), std::optional(ProfileOpenProblem::CannotOpen));
}

TEST_F(IdentityStoreUnopenableTest, AnIdentityWithoutAVersionCannotBeOpened) {
  Object old;
  old.set("nickname", "no-version");
  WriteIdentityJson(old, MakeTestDek());
  EXPECT_EQ(ProblemOnLoad(), std::optional(ProfileOpenProblem::CannotOpen));
}

TEST_F(IdentityStoreUnopenableTest, ANewerAppsIdentityAsksForAnUpdate) {
  Object newer;
  newer.set("schema_version", static_cast<int64_t>(IdentityStore::kSchemaVersion + 1));
  WriteIdentityJson(newer, MakeTestDek());
  EXPECT_EQ(ProblemOnLoad(), std::optional(ProfileOpenProblem::Newer));
}

// A newer app's envelope (its version byte above this build's): "update PP", never the wipe.
TEST_F(IdentityStoreUnopenableTest, ANewerEnvelopeAsksForAnUpdate) {
  Object current;
  current.set("schema_version", static_cast<int64_t>(IdentityStore::kSchemaVersion));
  const std::string json = DumpJson(current, 2);
  auto ciphertext = FileCipher::Encrypt(MakeTestDek(), ByteVector(json.begin(), json.end()),
                                        FileCipher::BuildAad("identity", "test-profile"));
  ASSERT_TRUE(static_cast<bool>(ciphertext));
  ByteVector bytes = *ciphertext;
  bytes[0] = static_cast<uint8_t>(kEncryptedPayloadVersion + 1);
  WriteIdentityFile(bytes);
  EXPECT_EQ(ProblemOnLoad(), std::optional(ProfileOpenProblem::Newer));
}

TEST_F(IdentityStoreUnopenableTest, AnIdentityUnderAnotherKeyCannotBeOpened) {
  Object current;
  current.set("schema_version", static_cast<int64_t>(IdentityStore::kSchemaVersion));
  WriteIdentityJson(current, ByteVector(kDataEncryptionKeySize, 0x7a)); // not this vault's key
  EXPECT_EQ(ProblemOnLoad(), std::optional(ProfileOpenProblem::CannotOpen));
}

TEST_F(IdentityStoreUnopenableTest, ADamagedIdentityFileCannotBeOpened) {
  ByteVector damaged(200, 0x5c);
  damaged[0] = kEncryptedPayloadVersion; // this build's envelope, a body that does not open (a higher byte: "newer")
  WriteIdentityFile(damaged);
  EXPECT_EQ(ProblemOnLoad(), std::optional(ProfileOpenProblem::CannotOpen));
}

TEST_F(IdentityStoreUnopenableTest, AMissingKeyIsNotAProfileProblem) {
  // No DEK yet (the vault still locked): the PIN is what is missing, not a profile this build cannot open.
  Object current;
  current.set("schema_version", static_cast<int64_t>(IdentityStore::kSchemaVersion));
  WriteIdentityJson(current, MakeTestDek());
  IdentityStore store(data_dir_.string(), "test-profile");
  auto loaded = store.LoadOrCreate();
  ASSERT_FALSE(static_cast<bool>(loaded));
  EXPECT_EQ(ProfileUnlockGate::ProfileProblemOf(loaded.error()), std::nullopt);
}

} // namespace
