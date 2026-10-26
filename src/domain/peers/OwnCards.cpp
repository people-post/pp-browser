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
  std::lock_guard lock(keys_mutex_);
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

Roe<AccountCardDevice> OwnCards::Self() {
  auto identity = identity_.Get();
  if (!identity) {
    return identity.error();
  }
  auto peer_id = PeerRequestAuth::DevicePeerId(identity->public_key_b64);
  if (!peer_id) {
    return peer_id.error();
  }
  if (identity->device_kem_public_key_b64.empty()) {
    return Error("device KEM key not set");
  }
  return AccountCardDevice{.peer_id = *peer_id,
                           .signing_public_key_b64 = identity->public_key_b64,
                           .kem_public_key_b64 = identity->device_kem_public_key_b64};
}

Roe<std::optional<AccountCard>> OwnCards::NewestLocked() {
  auto identity = identity_.Get();
  if (!identity) {
    return identity.error();
  }
  auto kept = cards_.Kept(identity->account_id);
  if (!kept) {
    return kept.error();
  }
  if (!kept->has_value() || !(*kept)->newest_card) {
    return std::optional<AccountCard>{};
  }
  return std::optional<AccountCard>((*kept)->newest_card);
}

Roe<AccountCard> OwnCards::IssueLocked(const std::optional<AccountCard>& base, const std::vector<AccountCardDevice>& also,
                                       const std::string& nickname, const int64_t now_ms, bool& issued) {
  issued = false;
  auto keys = Keys();
  if (!keys) {
    return keys.error();
  }
  auto self = Self();
  if (!self) {
    return self.error();
  }
  // The newest card's devices in its order, this device as it is now (first when it is new to the list), then
  // any other known one — at most the cap. Nothing new: the same list, so the card stands.
  std::vector<AccountCardDevice> devices;
  if (base) {
    for (const AccountCardDevice& device : base->devices) {
      devices.push_back(device.peer_id == self->peer_id ? *self : device);
    }
  }
  const auto listed = [&devices](const std::string& peer_id) {
    return std::any_of(devices.begin(), devices.end(),
                       [&peer_id](const AccountCardDevice& d) { return d.peer_id == peer_id; });
  };
  if (!listed(self->peer_id)) {
    devices.insert(devices.begin(), *self);
  }
  for (const AccountCardDevice& device : also) {
    if (!listed(device.peer_id)) {
      devices.push_back(device);
    }
  }
  if (devices.size() > kMaxAccountCardDevices) {
    devices.resize(kMaxAccountCardDevices);
  }
  if (base && SameKeys(base->keys, *keys) && base->nickname == nickname && base->devices == devices) {
    return *base;
  }
  // Newer than any card the account has issued that this device knows, whatever its clock says.
  const int64_t issued_at = std::max(now_ms, base ? base->issued_at + 1 : now_ms);
  auto signed_card = SignAccountCard(
      AccountCard{.keys = *keys, .nickname = nickname, .devices = devices, .issued_at = issued_at},
      [this](const ByteVector& bytes) { return identity_.SignBytes(bytes); });
  if (!signed_card) {
    return signed_card.error();
  }
  if (auto kept = cards_.AddAccountCard(*signed_card, std::max(now_ms, issued_at), "self"); !kept) {
    return kept.error();
  }
  issued = true;
  return signed_card;
}

Roe<AccountCard> OwnCards::Card(const int64_t now_ms) {
  auto identity = identity_.Get();
  if (!identity) {
    return identity.error();
  }
  bool issued = false;
  Roe<AccountCard> card = [&]() -> Roe<AccountCard> {
    std::lock_guard lock(mutex_);
    auto newest = NewestLocked();
    if (!newest) {
      return newest.error();
    }
    return IssueLocked(*newest, {}, identity->nickname, now_ms, issued);
  }();
  if (card && issued && on_changed_) {
    on_changed_(*card);
  }
  return card;
}

Roe<AccountCard> OwnCards::Learn(const AccountCard& card, const int64_t now_ms) {
  auto identity = identity_.Get();
  if (!identity) {
    return identity.error();
  }
  if (card.keys.account_id != identity->account_id) {
    return Error("account card: not our account");
  }
  if (auto checked = VerifyAccountCard(card, now_ms); !checked) {
    return checked.error();
  }
  bool issued = false;
  std::optional<AccountCard> ours;
  Roe<AccountCard> result = [&]() -> Roe<AccountCard> {
    std::lock_guard lock(mutex_);
    auto newest = NewestLocked();
    if (!newest) {
      return newest.error();
    }
    ours = *newest;
    if (auto kept = cards_.AddAccountCard(card, now_ms, "sibling"); !kept) {
      return kept.error();
    }
    auto now_newest = NewestLocked();
    if (!now_newest) {
      return now_newest.error();
    }
    // Whichever is newer is the base; the other one's devices are kept too (see the header).
    const std::vector<AccountCardDevice> other =
        ours && *now_newest && now_newest->value() == card ? ours->devices : card.devices;
    const std::string nickname = *now_newest ? (*now_newest)->nickname : identity->nickname;
    return IssueLocked(*now_newest, other, nickname, now_ms, issued);
  }();
  if (!result) {
    return result;
  }
  if (identity->nickname != result->nickname) {
    LocalIdentity renamed = *identity;
    renamed.nickname = result->nickname;
    if (auto updated = identity_.Update(renamed); !updated) {
      return updated.error();
    }
  }
  if ((issued || !ours || !(*ours == *result)) && on_changed_) {
    on_changed_(*result);
  }
  return result;
}

} // namespace pbr
