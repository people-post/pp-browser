#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/DataKeyVault.h"
#include "domain/people/IdentityStore.h"
#include "domain/people/LinkDeviceExchange.h"

#include <filesystem>
#include <gtest/gtest.h>

namespace pbr {
namespace {

ByteVector MakeDek() {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0x31 + i);
  }
  return dek;
}

class LinkDeviceExchangeTest : public ::testing::Test {
protected:
  void SetUp() override {
    old_dir_ = std::filesystem::temp_directory_path() / ("pp_link_old_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    new_dir_ = std::filesystem::temp_directory_path() / ("pp_link_new_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(old_dir_);
    std::filesystem::remove_all(new_dir_);
    std::filesystem::create_directories(old_dir_);
    std::filesystem::create_directories(new_dir_);
  }

  void TearDown() override {
    std::filesystem::remove_all(old_dir_);
    std::filesystem::remove_all(new_dir_);
  }

  std::filesystem::path old_dir_;
  std::filesystem::path new_dir_;
};

TEST_F(LinkDeviceExchangeTest, ImportKeepsDeviceKeysReplacesAccount) {
  const ByteVector dek = MakeDek();
  IdentityStore old_store(old_dir_.string(), "old");
  ASSERT_TRUE(old_store.SetDek(dek));
  auto old_id = old_store.LoadOrCreate();
  ASSERT_TRUE(old_id);
  LocalIdentity seeded = *old_id;
  seeded.relay_user_id = "relay:alice";
  seeded.nickname = "alice";
  seeded.registered = true;
  auto saved = old_store.Update(seeded);
  ASSERT_TRUE(saved);

  const std::string old_account = saved->account_id;
  const std::string old_peer = saved->peer_id;
  constexpr int64_t kNow = 1'700'000'000'000;
  auto json = LinkDeviceExchange::ExportJson(*saved, dek, {}, kNow);
  ASSERT_TRUE(json) << json.error().message;

  IdentityStore new_store(new_dir_.string(), "new");
  DataKeyVault new_vault(DataKeyVault::VaultPathForProfile(new_dir_.string()), "new");
  auto imported = LinkDeviceExchange::Import(new_store, new_vault, *json, "link-pin-1234", kNow + 1000);
  ASSERT_TRUE(imported) << imported.error().message;
  EXPECT_EQ(imported->identity.account_id, old_account);
  EXPECT_EQ(imported->identity.relay_user_id, "relay:alice");
  EXPECT_NE(imported->identity.peer_id, old_peer);
  EXPECT_EQ(imported->identity.kem_public_key_b64, saved->kem_public_key_b64);
  EXPECT_EQ(imported->identity.kem_private_key_b64, saved->kem_private_key_b64);
  EXPECT_TRUE(imported->identity.registered);
  EXPECT_TRUE(new_vault.Exists());

  DataKeyVault reopen(DataKeyVault::VaultPathForProfile(new_dir_.string()), "new");
  ASSERT_TRUE(reopen.Unlock("link-pin-1234"));
  auto round_dek = reopen.Dek();
  ASSERT_TRUE(round_dek);
  EXPECT_EQ(*round_dek, dek);
}

TEST_F(LinkDeviceExchangeTest, ImportOntoExistingVaultKeepsNewDevicePeerId) {
  const ByteVector shared = MakeDek();
  IdentityStore old_store(old_dir_.string(), "old");
  ASSERT_TRUE(old_store.SetDek(shared));
  auto old_id = old_store.LoadOrCreate();
  ASSERT_TRUE(old_id);
  LocalIdentity seeded = *old_id;
  seeded.relay_user_id = "relay:alice";
  seeded.nickname = "alice";
  seeded.registered = true;
  auto saved = old_store.Update(seeded);
  ASSERT_TRUE(saved);
  constexpr int64_t kNow = 1'700'000'000'000;
  auto json = LinkDeviceExchange::ExportJson(*saved, shared, {}, kNow);
  ASSERT_TRUE(json) << json.error().message;

  ByteVector local_dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < local_dek.size(); ++i) {
    local_dek[i] = static_cast<uint8_t>(0x90 + i);
  }
  IdentityStore new_store(new_dir_.string(), "new");
  ASSERT_TRUE(new_store.SetDek(local_dek));
  auto new_id = new_store.LoadOrCreate();
  ASSERT_TRUE(new_id);
  const std::string new_peer = new_id->peer_id;
  EXPECT_NE(new_peer, saved->peer_id);

  DataKeyVault new_vault(DataKeyVault::VaultPathForProfile(new_dir_.string()), "new");
  ASSERT_TRUE(new_vault.CreateWithDek("device-pin", local_dek));
  auto imported = LinkDeviceExchange::Import(new_store, new_vault, *json, "device-pin", kNow + 1000);
  ASSERT_TRUE(imported) << imported.error().message;
  EXPECT_EQ(imported->identity.account_id, saved->account_id);
  EXPECT_EQ(imported->identity.peer_id, new_peer);
  EXPECT_EQ(imported->identity.kem_public_key_b64, saved->kem_public_key_b64);
  EXPECT_EQ(imported->identity.kem_private_key_b64, saved->kem_private_key_b64);
  EXPECT_NE(imported->identity.kem_public_key_b64, new_id->kem_public_key_b64);
  EXPECT_EQ(imported->identity.relay_user_id, "relay:alice");

  DataKeyVault reopen(DataKeyVault::VaultPathForProfile(new_dir_.string()), "new");
  ASSERT_TRUE(reopen.Unlock("device-pin"));
  EXPECT_EQ(*reopen.Dek(), shared);

  ASSERT_TRUE(new_store.SetDek(shared));
  auto reloaded = new_store.Get();
  ASSERT_TRUE(reloaded);
  EXPECT_EQ(reloaded->account_id, saved->account_id);
  EXPECT_EQ(reloaded->peer_id, new_peer);
}

TEST_F(LinkDeviceExchangeTest, CaptureRejectsPrivatePsks) {
  LocalIdentity identity;
  identity.account_id = "account:x";
  identity.account_signing_public_key_b64 = "pk";
  identity.account_signing_private_key_b64 = "sk";
  identity.relay_user_id = "relay:x";
  LinkDevicePublicPsk private_row;
  private_row.key.channel = CryptoChannel::E2e;
  private_row.key.peer_identity_kind = "account";
  private_row.key.peer_identity_value = "account:bob";
  private_row.session_epoch = 1;
  auto captured = LinkDeviceExchange::Capture(identity, MakeDek(), {private_row}, 1);
  EXPECT_FALSE(captured);
}

TEST_F(LinkDeviceExchangeTest, CaptureRejectsMissingAccountKem) {
  LocalIdentity identity;
  identity.account_id = "account:x";
  identity.account_signing_public_key_b64 = "pk";
  identity.account_signing_private_key_b64 = "sk";
  identity.relay_user_id = "relay:x";
  auto captured = LinkDeviceExchange::Capture(identity, MakeDek(), {}, 1);
  ASSERT_FALSE(captured);
  EXPECT_NE(captured.error().message.find("account KEM"), std::string::npos);
}

} // namespace
} // namespace pbr
