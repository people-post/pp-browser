#include "base/crypto/CryptoConstants.h"
#include "base/crypto/CryptoUtil.h"
#include "base/crypto/DataKeyVault.h"
#include "base/messaging/SqliteThreadStore.h"
#include "base/people/IdentityStore.h"
#include "feature/messaging/LinkDeviceCoordinator.h"
#include "feature/messaging/SqlitePskSessionStore.h"

#include "common/Utilities.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <memory>

namespace pbr {
namespace {

ByteVector MakeDek() {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0x31 + i);
  }
  return dek;
}

class LinkDeviceCoordinatorTest : public ::testing::Test {
protected:
  void SetUp() override {
    const auto suffix = util::GenerateUuid();
    old_dir_ = std::filesystem::temp_directory_path() / ("pp_link_coord_old_" + suffix);
    new_dir_ = std::filesystem::temp_directory_path() / ("pp_link_coord_new_" + suffix);
    std::filesystem::remove_all(old_dir_);
    std::filesystem::remove_all(new_dir_);
    std::filesystem::create_directories(old_dir_);
    std::filesystem::create_directories(new_dir_);
  }

  void TearDown() override {
    new_psks_.reset();
    new_threads_.reset();
    old_psks_.reset();
    old_threads_.reset();
    std::filesystem::remove_all(old_dir_);
    std::filesystem::remove_all(new_dir_);
  }

  std::filesystem::path old_dir_;
  std::filesystem::path new_dir_;
  std::unique_ptr<SqliteThreadStore> old_threads_;
  std::unique_ptr<SqlitePskSessionStore> old_psks_;
  std::unique_ptr<SqliteThreadStore> new_threads_;
  std::unique_ptr<SqlitePskSessionStore> new_psks_;
};

TEST_F(LinkDeviceCoordinatorTest, ExportOmitsPrivatePsksAndImportAppliesPublic) {
  const ByteVector shared = MakeDek();
  old_threads_ = std::make_unique<SqliteThreadStore>(old_dir_.string());
  ASSERT_TRUE(old_threads_->ListThreads());
  old_psks_ = std::make_unique<SqlitePskSessionStore>(old_threads_->ProfileDbPath(), "old");
  ASSERT_TRUE(old_psks_->SetDek(shared));

  IdentityStore old_identity(old_dir_.string(), "old");
  ASSERT_TRUE(old_identity.SetDek(shared));
  auto old_id = old_identity.LoadOrCreate();
  ASSERT_TRUE(old_id);
  LocalIdentity seeded = *old_id;
  seeded.relay_user_id = "relay:alice";
  seeded.nickname = "alice";
  seeded.registered = true;
  auto saved = old_identity.Update(seeded);
  ASSERT_TRUE(saved);

  DirectChatTarget public_target;
  public_target.peer_identity_kind = "account";
  public_target.peer_identity_value = "account:bob";
  public_target.channel = ThreadChannel::E2ePublic;
  DirectChatTarget private_target = public_target;
  private_target.channel = ThreadChannel::E2e;
  ASSERT_TRUE(old_threads_->FindOrCreateDirectThread(public_target, "contact-bob", "Bob"));
  ASSERT_TRUE(old_threads_->FindOrCreateDirectThread(private_target, "contact-bob", "Bob"));

  auto public_psk = old_psks_->GenerateMasterPsk();
  ASSERT_TRUE(public_psk);
  PskSessionRecord public_row;
  public_row.key.peer_identity_kind = "account";
  public_row.key.peer_identity_value = "account:bob";
  public_row.key.channel = CryptoChannel::E2ePublic;
  public_row.session_epoch = 3;
  public_row.master_psk_b64 = Base64Encode(*public_psk);
  public_row.psk_verified_at = 99;
  ASSERT_TRUE(old_psks_->Save(public_row));

  auto private_psk = old_psks_->GenerateMasterPsk();
  ASSERT_TRUE(private_psk);
  PskSessionRecord private_row;
  private_row.key.peer_identity_kind = "account";
  private_row.key.peer_identity_value = "account:bob";
  private_row.key.channel = CryptoChannel::E2e;
  private_row.session_epoch = 1;
  private_row.master_psk_b64 = Base64Encode(*private_psk);
  ASSERT_TRUE(old_psks_->Save(private_row));

  DataKeyVault old_vault(DataKeyVault::VaultPathForProfile(old_dir_.string()), "old");
  ASSERT_TRUE(old_vault.CreateWithDek("old-pin", shared));
  constexpr int64_t kNow = 1'700'000'000'000;
  auto json = LinkDeviceCoordinator::ExportJson(old_identity, old_vault, *old_psks_, kNow);
  ASSERT_TRUE(json) << json.error().message;

  auto collected = LinkDeviceCoordinator::CollectPublicPsks(*old_psks_);
  ASSERT_TRUE(collected);
  ASSERT_EQ(collected->size(), 1u);
  EXPECT_EQ(collected->front().key.channel, CryptoChannel::E2ePublic);
  EXPECT_EQ(collected->front().session_epoch, 3u);

  ByteVector local_dek(kDataEncryptionKeySize, 0x44);
  IdentityStore new_identity(new_dir_.string(), "new");
  ASSERT_TRUE(new_identity.SetDek(local_dek));
  auto new_id = new_identity.LoadOrCreate();
  ASSERT_TRUE(new_id);
  const std::string new_peer = new_id->peer_id;
  DataKeyVault new_vault(DataKeyVault::VaultPathForProfile(new_dir_.string()), "new");
  ASSERT_TRUE(new_vault.CreateWithDek("device-pin", local_dek));

  new_threads_ = std::make_unique<SqliteThreadStore>(new_dir_.string());
  ASSERT_TRUE(new_threads_->ListThreads());
  new_psks_ = std::make_unique<SqlitePskSessionStore>(new_threads_->ProfileDbPath(), "new");
  ASSERT_TRUE(new_psks_->SetDek(shared));

  auto imported =
      LinkDeviceCoordinator::Import(new_identity, new_vault, *new_psks_, nullptr, *json, "device-pin", kNow + 1000);
  ASSERT_TRUE(imported) << imported.error().message;
  EXPECT_EQ(imported->identity.account_id, saved->account_id);
  EXPECT_EQ(imported->identity.peer_id, new_peer);
  EXPECT_EQ(imported->identity.kem_public_key_b64, saved->kem_public_key_b64);
  EXPECT_EQ(imported->identity.kem_private_key_b64, saved->kem_private_key_b64);
  EXPECT_NE(imported->identity.kem_public_key_b64, new_id->kem_public_key_b64);
  ASSERT_EQ(imported->public_psks.size(), 1u);

  ChatTargetKey public_key;
  public_key.peer_identity_kind = "account";
  public_key.peer_identity_value = "account:bob";
  public_key.channel = CryptoChannel::E2ePublic;
  auto loaded_public = new_psks_->Load(public_key);
  ASSERT_TRUE(loaded_public);
  ASSERT_TRUE(loaded_public->has_value());
  EXPECT_EQ(*loaded_public->value().master_psk_b64, Base64Encode(*public_psk));
  EXPECT_EQ(loaded_public->value().session_epoch, 3u);

  ChatTargetKey private_key = public_key;
  private_key.channel = CryptoChannel::E2e;
  auto loaded_private = new_psks_->Load(private_key);
  ASSERT_TRUE(loaded_private);
  EXPECT_FALSE(loaded_private->has_value());
}

} // namespace
} // namespace pbr
