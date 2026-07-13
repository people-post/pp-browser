#include "base/people/Ed25519Signer.h"
#include "base/people/IdentityStore.h"
#include "base/crypto/CryptoConstants.h"
#include "base/crypto/CryptoUtil.h"
#include "libp2p/integration/host/PeerIdUtil.h"

#include <filesystem>
#include <gtest/gtest.h>
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
  EXPECT_TRUE(std::filesystem::exists(data_dir_ / "identity.enc"));

  auto public_key = Ed25519Signer::FromBase64(identity->public_key_b64);
  ASSERT_TRUE(static_cast<bool>(public_key));
  auto derived = PeerIdFromEd25519PublicKey(*public_key);
  ASSERT_TRUE(static_cast<bool>(derived));
  EXPECT_EQ(identity->peer_id, *derived);
}

TEST_F(IdentityStoreTest, DerivesSamePeerIdAcrossReload) {
  std::string peer_id;
  {
    IdentityStore store(data_dir_.string(), "test-profile");
    ASSERT_TRUE(store.SetDek(MakeTestDek()));
    auto identity = store.LoadOrCreate();
    ASSERT_TRUE(static_cast<bool>(identity));
    peer_id = identity->peer_id;
  }
  EXPECT_TRUE(std::filesystem::exists(data_dir_ / "identity.enc"));
  EXPECT_FALSE(std::filesystem::exists(data_dir_ / "identity.json"));
  IdentityStore reloaded(data_dir_.string(), "test-profile");
  ASSERT_TRUE(reloaded.SetDek(MakeTestDek()));
  auto identity = reloaded.Get();
  ASSERT_TRUE(static_cast<bool>(identity));
  EXPECT_EQ(identity->peer_id, peer_id);
  EXPECT_TRUE(identity->relay_user_id.empty());
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

} // namespace
