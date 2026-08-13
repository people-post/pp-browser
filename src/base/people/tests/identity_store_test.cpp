#include "base/people/Ed25519Signer.h"
#include "base/people/IdentityStore.h"
#include "base/crypto/CryptoConstants.h"
#include "base/crypto/CryptoUtil.h"
#include "base/crypto/FileCipher.h"
#include "libp2p/integration/host/PeerIdUtil.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <sodium.h>

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

  auto public_key = Ed25519Signer::FromBase64(identity->public_key_b64);
  ASSERT_TRUE(static_cast<bool>(public_key));
  auto derived = PeerIdFromEd25519PublicKey(*public_key);
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
  auto keys = Ed25519Signer::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(keys));
  const std::string public_key_b64 = Ed25519Signer::ToBase64(keys->public_key);
  const std::string private_key_b64 = Ed25519Signer::ToBase64(keys->private_key);

  IdentityStore store(data_dir_.string(), "test-profile");
  ASSERT_TRUE(store.SetDek(MakeTestDek()));
  auto created = store.LoadOrCreate();
  ASSERT_TRUE(static_cast<bool>(created));
  LocalIdentity identity = *created;
  identity.public_key_b64 = public_key_b64;
  identity.private_key_b64 = private_key_b64;
  identity.nickname = "alice";
  identity.relay_user_id = "relay:alice123";
  identity.brief_llm_api_key = "brf_llm_testkeyABCDEFGHIJKLMNOP";
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
  EXPECT_TRUE(loaded->registered);
  EXPECT_EQ(loaded->registration_expires_at, "2099-06-01T00:00:00.000Z");
  EXPECT_FALSE(loaded->peer_id.empty());
}

TEST_F(IdentityStoreTest, RequiresDek) {
  IdentityStore store(data_dir_.string(), "test-profile");
  auto identity = store.LoadOrCreate();
  ASSERT_FALSE(static_cast<bool>(identity));
}

TEST_F(IdentityStoreTest, WritesSchemaVersionAndMigratesLegacy) {
  const ByteVector dek = MakeTestDek();
  auto keys = Ed25519Signer::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(keys));

  std::filesystem::create_directories(data_dir_);
  const nlohmann::json legacy = {{"public_key_b64", Ed25519Signer::ToBase64(keys->public_key)},
                                 {"private_key_b64", Ed25519Signer::ToBase64(keys->private_key)},
                                 {"nickname", "legacy"},
                                 {"relay_user_id", ""},
                                 {"brief_llm_api_key", ""},
                                 {"registered", false},
                                 {"registration_expires_at", ""}};
  const std::string json = legacy.dump(2);
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
  ASSERT_TRUE(static_cast<bool>(identity)) << identity.error().message;
  EXPECT_EQ(identity->nickname, "legacy");

  // Reload should decrypt the rewritten schema_version payload.
  IdentityStore reloaded(data_dir_.string(), "test-profile");
  ASSERT_TRUE(reloaded.SetDek(dek));
  auto again = reloaded.Get();
  ASSERT_TRUE(static_cast<bool>(again)) << again.error().message;
  EXPECT_EQ(again->nickname, "legacy");

  std::ifstream in(data_dir_ / "identity.enc", std::ios::binary);
  ASSERT_TRUE(static_cast<bool>(in));
  ByteVector blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  auto plain = FileCipher::Decrypt(dek, blob, aad);
  ASSERT_TRUE(static_cast<bool>(plain)) << plain.error().message;
  const nlohmann::json root = nlohmann::json::parse(plain->begin(), plain->end(), nullptr, false);
  ASSERT_FALSE(root.is_discarded());
  ASSERT_TRUE(root.contains("schema_version"));
  EXPECT_EQ(root["schema_version"].get<int>(), IdentityStore::kSchemaVersion);
}

TEST_F(IdentityStoreTest, RejectsNewerSchemaVersion) {
  const ByteVector dek = MakeTestDek();
  auto keys = Ed25519Signer::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(keys));

  std::filesystem::create_directories(data_dir_);
  const nlohmann::json newer = {{"schema_version", IdentityStore::kSchemaVersion + 1},
                                {"public_key_b64", Ed25519Signer::ToBase64(keys->public_key)},
                                {"private_key_b64", Ed25519Signer::ToBase64(keys->private_key)},
                                {"nickname", "future"},
                                {"registered", false}};
  const std::string json = newer.dump(2);
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

} // namespace