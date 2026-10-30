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

namespace {

bool Lists(const std::vector<AccountCardDevice>& devices, const std::string& peer_id) {
  return std::any_of(devices.begin(), devices.end(),
                     [&peer_id](const AccountCardDevice& d) { return d.peer_id == peer_id; });
}

} // namespace

Roe<AccountCard> OwnCards::IssueLocked(const std::optional<AccountCard>& base, std::vector<AccountCardDevice> devices,
                                       const std::string& nickname, const int64_t now_ms, bool& issued) {
  issued = false;
  auto keys = Keys();
  if (!keys) {
    return keys.error();
  }
  if (devices.size() > kMaxAccountCardDevices) {
    return Error("This account already has " + std::to_string(kMaxAccountCardDevices) + " devices");
  }
  if (base && SameKeys(base->keys, *keys) && base->nickname == nickname && base->devices == devices) {
    return *base;  // it says it all: the card stands
  }
  // Newer than any card of the account this device knows (or knew before a restore: the floor), whatever its
  // clock says.
  int64_t issued_at = now_ms;
  if (base) {
    issued_at = std::max(issued_at, base->issued_at + 1);
  }
  if (const int64_t floor = cards_.CardFloor(keys->account_id); floor > 0) {
    issued_at = std::max(issued_at, floor + 1);
  }
  auto signed_card = SignAccountCard(
      AccountCard{.keys = *keys, .nickname = nickname, .devices = std::move(devices), .issued_at = issued_at},
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
  bool intent_done = false;
  Roe<AccountCard> card = [&]() -> Roe<AccountCard> {
    std::lock_guard lock(mutex_);
    auto newest = NewestLocked();
    if (!newest) {
      return newest.error();
    }
    auto self = Self();
    if (!self) {
      return self.error();
    }
    std::vector<AccountCardDevice> devices;
    if (identity->card_intent == kCardIntentFresh || !*newest) {
      devices = {*self};  // a new account, or restored: a fresh list, this device its master
    } else if (Lists((*newest)->devices, self->peer_id)) {
      for (const AccountCardDevice& device : (*newest)->devices) {
        devices.push_back(device.peer_id == self->peer_id ? *self : device);  // the list as it is, this entry now
      }
    } else if (identity->card_intent == kCardIntentJoin) {
      devices = (*newest)->devices;  // linked: at the end of the master's list
      devices.push_back(*self);
    } else {
      return Error(kSignedOutMessage);
    }
    intent_done = !identity->card_intent.empty();
    return IssueLocked(*newest, std::move(devices), identity->nickname, now_ms, issued);
  }();
  if (card && intent_done) {
    LocalIdentity done = *identity;
    done.card_intent.clear();
    if (auto updated = identity_.Update(done); !updated) {
      return updated.error();
    }
  }
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
    if (!*now_newest) {
      return Error("own card not kept");
    }
    // The newest card is the account's word on its devices (M021 d4: only the master changes the list). This
    // device only brings its own entry up to date there; a device it does not list is signed out — or still
    // joining / starting a fresh list, which its next Card() does.
    auto self = Self();
    if (!self) {
      return self.error();
    }
    const AccountCard& newest_card = **now_newest;
    if (!Lists(newest_card.devices, self->peer_id) || !identity->card_intent.empty()) {
      return newest_card;
    }
    std::vector<AccountCardDevice> devices;
    for (const AccountCardDevice& device : newest_card.devices) {
      devices.push_back(device.peer_id == self->peer_id ? *self : device);
    }
    // The master expecting a join takes a device that only this older card lists as the one joining.
    const bool master = !devices.empty() && devices.front().peer_id == self->peer_id;
    if (master && !(newest_card == card) && now_ms < join_expected_until_ms_) {
      for (const AccountCardDevice& device : card.devices) {
        if (!Lists(devices, device.peer_id)) {
          devices.push_back(device);
        }
      }
    }
    return IssueLocked(newest_card, std::move(devices), newest_card.nickname, now_ms, issued);
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

Roe<OwnDeviceList> OwnCards::Devices() {
  auto identity = identity_.Get();
  if (!identity) {
    return identity.error();
  }
  auto self = Self();
  if (!self) {
    return self.error();
  }
  std::lock_guard lock(mutex_);
  auto newest = NewestLocked();
  if (!newest) {
    return newest.error();
  }
  OwnDeviceList list;
  list.self_peer_id = self->peer_id;
  if (*newest) {
    list.devices = (*newest)->devices;
  }
  list.signed_out = identity->card_intent.empty() && *newest && !Lists(list.devices, self->peer_id);
  return list;
}

bool OwnCards::SignedOut() {
  auto list = Devices();
  return list && list->signed_out;
}

Roe<AccountCard> OwnCards::ChangeList(const std::string& peer_id, const bool make_master, const int64_t now_ms) {
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
    auto self = Self();
    if (!self) {
      return self.error();
    }
    if (!*newest || (*newest)->devices.empty() || (*newest)->devices.front().peer_id != self->peer_id) {
      return Error("Only your main device can change your devices");
    }
    std::vector<AccountCardDevice> devices = (*newest)->devices;
    const auto target = std::find_if(devices.begin(), devices.end(),
                                     [&peer_id](const AccountCardDevice& d) { return d.peer_id == peer_id; });
    if (target == devices.end() || peer_id == self->peer_id) {
      return Error("That device is not another device of this account");
    }
    if (make_master) {
      std::rotate(devices.begin(), target, target + 1);  // to the front; the rest keep their order
    } else {
      devices.erase(target);
    }
    return IssueLocked(*newest, std::move(devices), (*newest)->nickname, now_ms, issued);
  }();
  if (card && issued && on_changed_) {
    on_changed_(*card);
  }
  return card;
}

void OwnCards::ExpectJoin(const int64_t until_ms) {
  std::lock_guard lock(mutex_);
  join_expected_until_ms_ = std::max(join_expected_until_ms_, until_ms);
}

Roe<AccountCard> OwnCards::RemoveDevice(const std::string& peer_id, const int64_t now_ms) {
  return ChangeList(peer_id, /*make_master=*/false, now_ms);
}

Roe<AccountCard> OwnCards::MakeMaster(const std::string& peer_id, const int64_t now_ms) {
  return ChangeList(peer_id, /*make_master=*/true, now_ms);
}

} // namespace pbr
