#include "domain/people/IdentityStore.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/FileCipher.h"
#include "foundation/crypto/MlDsa.h"
#include "base/mesh/identity/PeerIdUtil.h"
#include "common/ValueJson.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
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

TEST_F(IdentityStoreTest, CreateHasPeerIdAndEmptyRelay) {
  IdentityStore store(data_dir_.string(), "test-profile");
  ASSERT_TRUE(store.SetDek(MakeTestDek()));
  auto identity = store.LoadOrCreate();
  ASSERT_TRUE(static_cast<bool>(identity)) << identity.error().message;
  EXPECT_FALSE(identity->peer_id.empty());
  EXPECT_TRUE(identity->relay_user_id.empty());
  EXPECT_FALSE(identity->registered);
  EXPECT_FALSE(identity->account_id.empty());
  EXPECT_EQ(identity->account_id.rfind("account:", 0), 0u);
  EXPECT_FALSE(identity->account_signing_public_key_b64.empty());
  EXPECT_TRUE(std::filesystem::exists(data_dir_ / "identity.enc"));

  auto public_key = Base64Decode(identity->public_key_b64);
  ASSERT_TRUE(static_cast<bool>(public_key));
  EXPECT_EQ(public_key->size(), kMlDsa65PublicKeyBytes);
  auto derived = PeerIdFromMlDsaPublicKey(*public_key);
  ASSERT_TRUE(static_cast<bool>(derived));
  EXPECT_EQ(identity->peer_id, *derived);
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
  EXPECT_TRUE(identity->relay_user_id.empty());
  auto got_account = reloaded.GetAccountId();
  ASSERT_TRUE(static_cast<bool>(got_account));
  EXPECT_EQ(*got_account, account_id);
}

TEST_F(IdentityStoreTest, KeepsRegisteredRelayId) {
  IdentityStore store(data_dir_.string(), "test-profile");
  ASSERT_TRUE(store.SetDek(MakeTestDek()));
  auto created = store.LoadOrCreate();
  ASSERT_TRUE(static_cast<bool>(created));
  LocalIdentity identity = *created;
  identity.nickname = "alice";
  identity.relay_user_id = "relay:alice123";
  identity.brief_llm_api_key = "brf_llm_testkeyABCDEFGHIJKLMNOP";
  identity.brief_llm_guest_api_key = "brf_guest_testkeyABCDEFGHIJKLMNOP";
  identity.registered = true;
  identity.registration_expires_at = "2099-06-01T00:00:00.000Z";
  auto updated = store.Update(identity);
  ASSERT_TRUE(static_cast<bool>(updated)) << updated.error().message;

  IdentityStore reloaded(data_dir_.string(), "test-profile");
  ASSERT_TRUE(reloaded.SetDek(MakeTestDek()));
  auto loaded = reloaded.LoadOrCreate();
  ASSERT_TRUE(static_cast<bool>(loaded)) << loaded.error().message;
  EXPECT_EQ(loaded->relay_user_id, "relay:alice123");
  EXPECT_EQ(loaded->brief_llm_api_key, "brf_llm_testkeyABCDEFGHIJKLMNOP");
  EXPECT_EQ(loaded->brief_llm_guest_api_key, "brf_guest_testkeyABCDEFGHIJKLMNOP");
  EXPECT_TRUE(loaded->registered);
  EXPECT_EQ(loaded->registration_expires_at, "2099-06-01T00:00:00.000Z");
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
    out.write(reinterpret_cast<const char*>(ciphertext->data()),
              static_cast<std::streamsize>(ciphertext->size()));
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
  EXPECT_EQ(static_cast<int>(root->getIf<int64_t>("schema_version").value_or(-1)),
            IdentityStore::kSchemaVersion);
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
    out.write(reinterpret_cast<const char*>(ciphertext->data()),
              static_cast<std::streamsize>(ciphertext->size()));
  }

  IdentityStore store(data_dir_.string(), "test-profile");
  ASSERT_TRUE(store.SetDek(dek));
  auto identity = store.Get();
  ASSERT_FALSE(static_cast<bool>(identity));
  EXPECT_NE(identity.error().message.find("schema"), std::string::npos);
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

} // namespace
