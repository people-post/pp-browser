#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/DataKeyVault.h"
#include "lib/messaging/SqliteThreadStore.h"
#include "lib/people/IdentityStore.h"
#include "lib/people/LinkDeviceCoordinator.h"
#include "lib/messaging/SqlitePskSessionStore.h"

#include "common/Utilities.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include "common/PbrCompat.h"

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
    old_psks_.reset();
    old_threads_.reset();
    std::filesystem::remove_all(old_dir_);
    std::filesystem::remove_all(new_dir_);
  }

  std::filesystem::path old_dir_;
  std::filesystem::path new_dir_;
  std::unique_ptr<SqliteThreadStore> old_threads_;
  std::unique_ptr<SqlitePskSessionStore> old_psks_;
};

TEST_F(LinkDeviceCoordinatorTest, TheBundleCarriesTheAccountAndItsCardButNoSessions) {
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

  // The old device's account-level public session with Bob: its own, never copied (M021).
  DirectChatTarget public_target;
  public_target.peer_identity_kind = "account";
  public_target.peer_identity_value = "account:bob";
  public_target.channel = ThreadChannel::E2ePublic;
  ASSERT_TRUE(old_threads_->FindOrCreateDirectThread(public_target, "contact-bob", "Bob"));
  auto public_psk = old_psks_->GenerateMasterPsk();
  ASSERT_TRUE(public_psk);
  PskSessionRecord public_row;
  public_row.key.peer_identity_kind = "account";
  public_row.key.peer_identity_value = "account:bob";
  public_row.key.channel = CryptoChannel::E2ePublic;
  public_row.session_epoch = 3;
  public_row.master_psk_b64 = Base64Encode(*public_psk);
  ASSERT_TRUE(old_psks_->Save(public_row));

  DataKeyVault old_vault(DataKeyVault::VaultPathForProfile(old_dir_.string()), "old");
  ASSERT_TRUE(old_vault.CreateWithDek("old-pin", shared));
  constexpr int64_t kNow = 1'700'000'000'000;
  const std::string card = R"({"card_version":4})";
  EXPECT_FALSE(LinkDeviceCoordinator::ExportJson(old_identity, old_vault, "", kNow)) << "no card, no link";
  auto json = LinkDeviceCoordinator::ExportJson(old_identity, old_vault, card, kNow);
  ASSERT_TRUE(json) << json.error().message;
  EXPECT_EQ(json->find(*public_row.master_psk_b64), std::string::npos) << "the session stays on the old device";

  ByteVector local_dek(kDataEncryptionKeySize, 0x44);
  IdentityStore new_identity(new_dir_.string(), "new");
  ASSERT_TRUE(new_identity.SetDek(local_dek));
  auto new_id = new_identity.LoadOrCreate();
  ASSERT_TRUE(new_id);
  const std::string new_peer = new_id->peer_id;
  DataKeyVault new_vault(DataKeyVault::VaultPathForProfile(new_dir_.string()), "new");
  ASSERT_TRUE(new_vault.CreateWithDek("device-pin", local_dek));

  auto imported = LinkDeviceCoordinator::Import(new_identity, new_vault, nullptr, *json, "device-pin", kNow + 1000);
  ASSERT_TRUE(imported) << imported.error().message;
  EXPECT_EQ(imported->identity.account_id, saved->account_id);
  EXPECT_EQ(imported->identity.peer_id, new_peer);
  EXPECT_EQ(imported->identity.kem_public_key_b64, saved->kem_public_key_b64);
  EXPECT_EQ(imported->identity.kem_private_key_b64, saved->kem_private_key_b64);
  EXPECT_NE(imported->identity.kem_public_key_b64, new_id->kem_public_key_b64);
  EXPECT_EQ(imported->account_card_json, std::optional<std::string>(card)) << "the new device signs itself in";
  EXPECT_EQ(*new_vault.Dek(), shared) << "the shared DEK, wrapped under this device's PIN";
}

} // namespace
} // namespace pbr
