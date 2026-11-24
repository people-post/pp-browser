// Linking a device by the sealed QR handshake (multi-device M027) and restoring from the phrase (M023 §8): the main
// device seals its seed to the new device's one-time key; the new device opens it, derives the account from the seed,
// keeps its own device keys and makes its own DEK, and joins the list (a restore starts a fresh one).
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/DataKeyVault.h"
#include "foundation/crypto/LinkHandshake.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/crypto/RecoverySeed.h"
#include "lib/people/IdentityStore.h"
#include "lib/people/LinkDeviceExchange.h"
#include "lib/people/LinkOffer.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <optional>

namespace pbr {
namespace {

constexpr int64_t kNow = 1'700'000'000'000;

ByteVector MakeDek(const uint8_t base = 0x31) {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(base + i);
  }
  return dek;
}

class LinkDeviceExchangeTest : public ::testing::Test {
protected:
  void SetUp() override {
    old_dir_ =
        std::filesystem::temp_directory_path() / ("pp_link_old_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    new_dir_ =
        std::filesystem::temp_directory_path() / ("pp_link_new_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(old_dir_);
    std::filesystem::remove_all(new_dir_);
    std::filesystem::create_directories(old_dir_);
    std::filesystem::create_directories(new_dir_);
  }

  void TearDown() override {
    std::filesystem::remove_all(old_dir_);
    std::filesystem::remove_all(new_dir_);
  }

  /** The main device: a registered account whose keys derive from its recovery seed (as every new account's). */
  LocalIdentity MainDevice(IdentityStore& store) {
    EXPECT_TRUE(store.SetDek(MakeDek()));
    LocalIdentity identity = store.LoadOrCreate().value();
    identity.registered = true;
    identity.nickname = "alice";
    return store.Update(identity).value();
  }

  std::filesystem::path old_dir_;
  std::filesystem::path new_dir_;
};

TEST_F(LinkDeviceExchangeTest, TheSealedSeedBringsTheAccountWithThisDevicesOwnKeysAndDek) {
  IdentityStore old_store(old_dir_.string(), "old");
  const LocalIdentity main = MainDevice(old_store);
  ASSERT_FALSE(main.recovery_seed_b64.empty());

  LinkOffer offer;
  auto shown = offer.Begin(kNow);
  ASSERT_TRUE(shown) << shown.error().message;
  auto reply = LinkDeviceExchange::SealReply(main, shown->offer, std::nullopt, kNow);
  ASSERT_TRUE(reply) << reply.error().message;
  auto bundle = offer.Open(*reply, kNow + 1000);
  ASSERT_TRUE(bundle) << bundle.error().message;
  EXPECT_EQ(bundle->intent, LinkIntent::Join);
  EXPECT_EQ(bundle->expires_at_ms, kNow + kLinkWindowMs);

  IdentityStore new_store(new_dir_.string(), "new");
  DataKeyVault new_vault(DataKeyVault::VaultPathForProfile(new_dir_.string()), "new");
  auto imported =
      LinkDeviceExchange::ImportAccount(new_store, new_vault, bundle->seed, kCardIntentJoin, "", "link-pin-1234");
  ASSERT_TRUE(imported) << imported.error().message;
  EXPECT_EQ(imported->account_id, main.account_id) << "the seed alone names the account";
  EXPECT_EQ(imported->account_signing_public_key_b64, main.account_signing_public_key_b64);
  EXPECT_EQ(imported->account_signing_private_key_b64, main.account_signing_private_key_b64);
  EXPECT_EQ(imported->kem_public_key_b64, main.kem_public_key_b64);
  EXPECT_EQ(imported->kem_private_key_b64, main.kem_private_key_b64);
  EXPECT_EQ(imported->recovery_seed_b64, main.recovery_seed_b64) << "the new device can show the same phrase";
  EXPECT_NE(imported->peer_id, main.peer_id) << "a device of its own";
  EXPECT_NE(imported->device_kem_public_key_b64, main.device_kem_public_key_b64);
  EXPECT_TRUE(imported->registered);
  EXPECT_EQ(imported->card_intent, kCardIntentJoin) << "it appends itself to the main device's list";
  EXPECT_EQ(imported->nickname, "user") << "no nickname rides: the card brings the account's";

  DataKeyVault reopen(DataKeyVault::VaultPathForProfile(new_dir_.string()), "new");
  ASSERT_TRUE(reopen.Unlock("link-pin-1234"));
  EXPECT_NE(*reopen.Dek(), MakeDek()) << "its own DEK: nothing sealed under the main device's crosses";
}

TEST_F(LinkDeviceExchangeTest, ARestoreFromThePhraseIsTheSameImportOnAFreshList) {
  const ByteVector seed = GenerateRecoverySeed();
  IdentityStore store(new_dir_.string(), "new");
  DataKeyVault vault(DataKeyVault::VaultPathForProfile(new_dir_.string()), "new");
  auto restored = LinkDeviceExchange::ImportAccount(store, vault, seed, kCardIntentFresh, "alice", "pin-1234");
  ASSERT_TRUE(restored) << restored.error().message;
  const auto signing = MlDsa::GenerateKeyPairFromSeed(DeriveRecoveryKeys(seed)->account_ml_dsa_seed).value();
  EXPECT_EQ(restored->account_id, AccountIdFromMlDsaPublicKey(signing.public_key).value());
  EXPECT_EQ(restored->card_intent, kCardIntentFresh) << "the restored install signs a fresh device list";
  EXPECT_EQ(restored->nickname, "alice") << "the locker's nickname";
  EXPECT_EQ(restored->recovery_seed_b64.Reveal(), Base64Encode(seed));
}

TEST_F(LinkDeviceExchangeTest, AnExistingVaultKeepsItsDekAndThisDevicesPeerId) {
  IdentityStore old_store(old_dir_.string(), "old");
  const LocalIdentity main = MainDevice(old_store);
  const ByteVector local_dek = MakeDek(0x90);
  IdentityStore new_store(new_dir_.string(), "new");
  ASSERT_TRUE(new_store.SetDek(local_dek));
  const LocalIdentity before = new_store.LoadOrCreate().value();
  DataKeyVault new_vault(DataKeyVault::VaultPathForProfile(new_dir_.string()), "new");
  ASSERT_TRUE(new_vault.CreateWithDek("device-pin", local_dek));

  auto seed = Base64Decode(main.recovery_seed_b64.Reveal()).value();
  auto imported = LinkDeviceExchange::ImportAccount(new_store, new_vault, seed, kCardIntentJoin, "", "ignored");
  ASSERT_TRUE(imported) << imported.error().message;
  EXPECT_EQ(imported->account_id, main.account_id);
  EXPECT_EQ(imported->peer_id, before.peer_id);
  DataKeyVault reopen(DataKeyVault::VaultPathForProfile(new_dir_.string()), "new");
  ASSERT_TRUE(reopen.Unlock("device-pin")) << "the vault's PIN stays";
  EXPECT_EQ(*reopen.Dek(), local_dek) << "and its DEK";
}

TEST_F(LinkDeviceExchangeTest, OnlyARegisteredAccountWithItsSeedSealsAReply) {
  IdentityStore old_store(old_dir_.string(), "old");
  const LocalIdentity main = MainDevice(old_store);
  LinkOffer offer;
  const ByteVector bytes = offer.Begin(kNow).value().offer;

  LocalIdentity unregistered = main;
  unregistered.registered = false;
  EXPECT_FALSE(LinkDeviceExchange::SealReply(unregistered, bytes, std::nullopt, kNow));

  LocalIdentity no_seed = main;
  no_seed.recovery_seed_b64 = SecretText();
  auto refused = LinkDeviceExchange::SealReply(no_seed, bytes, std::nullopt, kNow);
  ASSERT_FALSE(refused) << "an account minted before M023 s1 has nothing a link could carry";
  EXPECT_NE(refused.error().message.find("recovery phrase"), std::string::npos);

  LocalIdentity other_seed = main;
  other_seed.recovery_seed_b64 = Base64Encode(GenerateRecoverySeed());
  EXPECT_FALSE(LinkDeviceExchange::SealReply(other_seed, bytes, std::nullopt, kNow))
      << "a seed that derives another account would link the new device to it";
}

TEST(LinkOfferTest, TheOneTimeKeyIsGoneOnceForgottenOrItsWindowPassed) {
  LinkOffer offer;
  const auto first = offer.Begin(kNow).value();
  EXPECT_EQ(first.code, LinkConfirmationCode(first.offer));
  EXPECT_EQ(FindLinkPayload(kLinkOfferTextPrefix, first.text), first.offer);
  ASSERT_TRUE(offer.Current(kNow + 1));

  LinkBundle bundle;
  bundle.seed = GenerateRecoverySeed();
  bundle.expires_at_ms = kNow + kLinkWindowMs;
  const ByteVector reply = SealLinkReply(first.offer, bundle).value();

  // A newer offer replaces the first: a reply to the first no longer opens.
  const auto second = offer.Begin(kNow + 2).value();
  EXPECT_NE(second.offer, first.offer);
  auto stale = offer.Open(reply, kNow + 3);
  ASSERT_FALSE(stale);
  EXPECT_TRUE(IsLinkError(stale.error(), LinkError::NotForThisDevice));
  EXPECT_TRUE(offer.Current(kNow + 3)) << "a reply that does not open keeps the key for the right one";

  const ByteVector right = SealLinkReply(second.offer, bundle).value();
  EXPECT_FALSE(offer.Open(right, kNow + 2 + kLinkWindowMs)) << "15 minutes later";
  EXPECT_FALSE(offer.Current(kNow + 3)) << "the key was wiped then";
  auto gone = offer.Open(right, kNow + 4);
  ASSERT_FALSE(gone);
  EXPECT_TRUE(IsLinkError(gone.error(), LinkError::Expired));

  const auto third = offer.Begin(kNow + 5).value();
  offer.Forget();
  EXPECT_FALSE(offer.Current(kNow + 6));
  EXPECT_FALSE(offer.Open(SealLinkReply(third.offer, bundle).value(), kNow + 6));
}

} // namespace
} // namespace pbr
