#include "domain/peers/OwnCards.h"

#include "foundation/crypto/KemKeyBinding.h"
#include "lib/messaging/PeerRequestAuth.h"

#include <algorithm>

namespace pbr {

Roe<KeyCard> OwnCards::Keys() {
  auto identity = identity_.Get();
  if (!identity) {
    return identity.error();
  }
  if (identity->account_id.empty() || identity->account_signing_public_key_b64.empty() ||
      identity->kem_public_key_b64.empty()) {
    return Error("account keys not set");
  }
  std::lock_guard lock(mutex_);
  if (keys_ && keys_->account_id == identity->account_id &&
      keys_->signing_public_key_b64 == identity->account_signing_public_key_b64 &&
      keys_->kem_public_key_b64 == identity->kem_public_key_b64) {
    return *keys_;
  }
  const auto binding_bytes = KemBindingSignBytes(identity->account_signing_public_key_b64, identity->kem_public_key_b64);
  if (binding_bytes.empty()) {
    return Error("account keys do not decode");
  }
  auto binding = identity_.SignBytes(binding_bytes);
  if (!binding) {
    return binding.error();
  }
  keys_ = KeyCard{.account_id = identity->account_id,
                  .signing_public_key_b64 = identity->account_signing_public_key_b64,
                  .kem_public_key_b64 = identity->kem_public_key_b64,
                  .kem_binding_signature_b64 = *binding};
  return *keys_;
}

Roe<std::string> OwnCards::DevicePeerId() {
  auto identity = identity_.Get();
  if (!identity) {
    return identity.error();
  }
  return PeerRequestAuth::DevicePeerId(identity->public_key_b64);
}

Roe<AccountCard> OwnCards::Card(const std::vector<std::string>& siblings, const int64_t now_ms) {
  auto keys = Keys();
  if (!keys) {
    return keys.error();
  }
  auto self = DevicePeerId();
  if (!self) {
    return self.error();
  }
  auto identity = identity_.Get();
  if (!identity) {
    return identity.error();
  }
  std::vector<std::string> devices{*self};
  for (const std::string& peer_id : siblings) {
    if (devices.size() < kMaxAccountCardDevices && std::find(devices.begin(), devices.end(), peer_id) == devices.end()) {
      devices.push_back(peer_id);
    }
  }
  std::lock_guard lock(mutex_);
  if (card_ && SameKeys(card_->keys, *keys) && card_->nickname == identity->nickname && card_->devices == devices) {
    return *card_;
  }
  auto signed_card = SignAccountCard(
      AccountCard{.keys = *keys, .nickname = identity->nickname, .devices = devices, .issued_at = now_ms},
      [this](const ByteVector& bytes) { return identity_.SignBytes(bytes); });
  if (!signed_card) {
    return signed_card.error();
  }
  card_ = std::move(signed_card.value());
  return *card_;
}

} // namespace pbr
