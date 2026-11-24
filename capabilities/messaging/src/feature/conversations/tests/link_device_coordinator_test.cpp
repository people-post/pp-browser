#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/DataKeyVault.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "lib/people/IdentityStore.h"
#include "lib/people/LinkDeviceCoordinator.h"
#include "lib/people/LinkOffer.h"

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
  std::unique_ptr<InMemoryDeliveryStore> old_threads_;
  std::unique_ptr<InMemoryPeerSessions> old_psks_;
};

TEST_F(LinkDeviceCoordinatorTest, TheReplySealsTheSeedAloneNoSessionsAndTheNewDeviceKeepsItsOwnDek) {
  const ByteVector main_dek = MakeDek();
  old_threads_ = std::make_unique<InMemoryDeliveryStore>();
  ASSERT_TRUE(old_threads_->ListThreads());
  old_psks_ = std::make_unique<InMemoryPeerSessions>(*old_threads_);
  ASSERT_TRUE(old_psks_->SetDek(main_dek));

  IdentityStore old_identity(old_dir_.string(), "old");
  ASSERT_TRUE(old_identity.SetDek(main_dek));
  auto old_id = old_identity.LoadOrCreate();
  ASSERT_TRUE(old_id);
  LocalIdentity seeded = *old_id;
  seeded.registered = true;
  seeded.nickname = "alice";
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
  PeerSessionRecord public_row;
  public_row.key.peer_identity_kind = "account";
  public_row.key.peer_identity_value = "account:bob";
  public_row.key.channel = CryptoChannel::E2ePublic;
  public_row.session_epoch = 3;
  public_row.master_psk_b64 = Base64Encode(*public_psk);
  ASSERT_TRUE(old_psks_->Save(public_row));

  constexpr int64_t kNow = 1'700'000'000'000;
  LinkOffer offer;
  const ByteVector offer_bytes = offer.Begin(kNow).value().offer;
  auto reply = LinkDeviceCoordinator::SealReply(old_identity, offer_bytes, std::nullopt, kNow);
  ASSERT_TRUE(reply) << reply.error().message;
  auto bundle = offer.Open(*reply, kNow + 1000);
  ASSERT_TRUE(bundle) << bundle.error().message;
  EXPECT_EQ(Base64Encode(bundle->seed), saved->recovery_seed_b64.Reveal()) << "the seed, and nothing of the sessions";
  EXPECT_FALSE(bundle->relay.has_value());

  IdentityStore new_identity(new_dir_.string(), "new");
  DataKeyVault new_vault(DataKeyVault::VaultPathForProfile(new_dir_.string()), "new");
  auto imported = LinkDeviceCoordinator::ImportAccount(new_identity, new_vault, nullptr, bundle->seed, kCardIntentJoin,
                                                       "", "device-pin");
  ASSERT_TRUE(imported) << imported.error().message;
  EXPECT_EQ(imported->account_id, saved->account_id);
  EXPECT_NE(imported->peer_id, saved->peer_id);
  EXPECT_EQ(imported->kem_public_key_b64, saved->kem_public_key_b64);
  EXPECT_EQ(imported->kem_private_key_b64, saved->kem_private_key_b64);
  EXPECT_EQ(imported->card_intent, kCardIntentJoin);
  EXPECT_NE(*new_vault.Dek(), main_dek) << "a DEK of its own, wrapped under this device's PIN";
}

} // namespace
} // namespace pbr
