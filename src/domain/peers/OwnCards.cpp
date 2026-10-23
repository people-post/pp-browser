#include "domain/peers/OwnKeyCard.h"

#include "foundation/crypto/KemKeyBinding.h"

namespace pbr {

Roe<KeyCard> OwnKeyCard::Get() {
  auto identity = identity_.Get();
  if (!identity) {
    return identity.error();
  }
  if (identity->account_id.empty() || identity->account_signing_public_key_b64.empty() ||
      identity->kem_public_key_b64.empty()) {
    return Error("account keys not set");
  }
  std::lock_guard lock(mutex_);
  if (card_ && card_->account_id == identity->account_id &&
      card_->signing_public_key_b64 == identity->account_signing_public_key_b64 &&
      card_->kem_public_key_b64 == identity->kem_public_key_b64) {
    return *card_;
  }
  const auto binding_bytes = KemBindingSignBytes(identity->account_signing_public_key_b64, identity->kem_public_key_b64);
  if (binding_bytes.empty()) {
    return Error("account keys do not decode");
  }
  auto binding = identity_.SignBytes(binding_bytes);
  if (!binding) {
    return binding.error();
  }
  card_ = KeyCard{.account_id = identity->account_id,
                  .signing_public_key_b64 = identity->account_signing_public_key_b64,
                  .kem_public_key_b64 = identity->kem_public_key_b64,
                  .kem_binding_signature_b64 = *binding};
  return *card_;
}

} // namespace pbr
