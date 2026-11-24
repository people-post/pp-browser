#include "lib/people/LinkDeviceExchange.h"

#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/HybridKem.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/crypto/RecoverySeed.h"

#include <sodium.h>

#include <optional>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

/** The account keys a seed derives, as the identity keeps them (base64). */
struct SeedAccount {
  std::string account_id;
  std::string signing_public_b64;
  SecretText signing_secret_b64;
  std::string kem_public_b64;
  SecretText kem_secret_b64;
};

Roe<SeedAccount> AccountFromSeed(const ByteVector& seed) {
  auto keys = DeriveRecoveryKeys(seed);
  if (!keys) {
    return keys.error();
  }
  auto signing = MlDsa::GenerateKeyPairFromSeed(keys->account_ml_dsa_seed);
  if (!signing) {
    return signing.error();
  }
  auto kem = HybridKem::GenerateKeyPairFromSeed(keys->account_ml_kem_coins);
  if (!kem) {
    return kem.error();
  }
  auto account_id = AccountIdFromMlDsaPublicKey(signing->public_key);
  if (!account_id) {
    return account_id.error();
  }
  SeedAccount account{.account_id = std::move(*account_id),
                      .signing_public_b64 = Base64Encode(signing->public_key),
                      .signing_secret_b64 = Base64Encode(signing->secret_key),
                      .kem_public_b64 = Base64Encode(kem->public_key),
                      .kem_secret_b64 = Base64Encode(kem->private_key)};
  sodium_memzero(signing->secret_key.data(), signing->secret_key.size());
  sodium_memzero(kem->private_key.data(), kem->private_key.size());
  return account;
}

/** The identity to bring the account onto: the one loaded under the vault there is, or a new one under a fresh DEK. */
Roe<LocalIdentity> IdentityUnderOwnDek(IdentityStore& identity, DataKeyVault& vault, const SecretText& pin) {
  if (vault.Exists()) {
    if (!vault.IsUnlocked()) {
      return Error("Unlock this profile first");
    }
    return identity.Get(); // its DEK stays: nothing another device sealed comes here
  }
  EnsureSodiumInit();
  ByteVector dek(kDataEncryptionKeySize);
  randombytes_buf(dek.data(), dek.size());
  auto created = vault.CreateWithDek(pin.Reveal(), dek);
  auto set = created ? identity.SetDek(dek) : Roe<void>(created.error());
  sodium_memzero(dek.data(), dek.size());
  if (!set) {
    return set.error();
  }
  return identity.LoadOrCreate();
}

} // namespace

Roe<ByteVector> LinkDeviceExchange::SealReply(const LocalIdentity& identity, const std::span<const uint8_t> offer,
                                              std::optional<RelayAddress> relay, const int64_t now_ms,
                                              const int64_t ttl_ms) {
  if (identity.account_id.empty() || identity.account_signing_public_key_b64.empty()) {
    return Error("Local account identity unavailable");
  }
  if (!identity.registered) {
    return Error("Local account not registered on the relay");
  }
  if (identity.recovery_seed_b64.empty()) {
    return Error("This account has no recovery phrase, so it cannot link a device");
  }
  if (now_ms <= 0 || ttl_ms <= 0) {
    return Error("Invalid link lifetime");
  }
  LinkBundle bundle;
  auto seed = Base64Decode(identity.recovery_seed_b64.Reveal());
  if (!seed) {
    return seed.error();
  }
  bundle.seed = std::move(*seed);
  // The seed must be this account's: a seed that derives other keys would link the new device to another account.
  auto derived = AccountFromSeed(bundle.seed);
  if (!derived) {
    return derived.error();
  }
  if (derived->account_id != identity.account_id) {
    return Error("This account's recovery seed does not derive its keys");
  }
  bundle.intent = LinkIntent::Join;
  bundle.expires_at_ms = now_ms + ttl_ms;
  bundle.relay = std::move(relay);
  return SealLinkReply(offer, bundle);
}

Roe<LocalIdentity> LinkDeviceExchange::ImportAccount(IdentityStore& identity, DataKeyVault& vault,
                                                     const ByteVector& seed, const std::string_view card_intent,
                                                     const std::string& nickname, const SecretText& pin) {
  if (card_intent != kCardIntentJoin && card_intent != kCardIntentFresh) {
    return Error("Unknown card intent");
  }
  auto account = AccountFromSeed(seed);
  if (!account) {
    return account.error();
  }
  auto loaded = IdentityUnderOwnDek(identity, vault, pin);
  if (!loaded) {
    return loaded.error();
  }
  // This device's own keys stay (its PeerId and device KEM, M021 §1); the account is the seed's.
  LocalIdentity next = *loaded;
  next.account_id = account->account_id;
  next.account_signing_public_key_b64 = account->signing_public_b64;
  next.account_signing_private_key_b64 = account->signing_secret_b64;
  next.kem_public_key_b64 = account->kem_public_b64;
  next.kem_private_key_b64 = account->kem_secret_b64;
  next.recovery_seed_b64 = Base64Encode(seed);
  if (!nickname.empty()) {
    next.nickname = nickname;
  }
  next.registered = true;
  next.card_intent = std::string(card_intent);
  return identity.Update(next);
}

} // namespace pbr
