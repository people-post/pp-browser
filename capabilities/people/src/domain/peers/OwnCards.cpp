#include "domain/peers/OwnCards.h"

#include "lib/people/CardKeys.h"
#include "lib/people/DevicePeerId.h"

#include <algorithm>
#include <chrono>

namespace pbr {

namespace {

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

} // namespace

OwnCards::OwnCards(IdentityStore& identity, AccountKeyResolver& cards)
    : identity_(identity), cards_(cards), life_(std::make_shared<Life>()) {
  redirectLogger("OwnCards");
  life_->owner = this;
}

OwnCards::~OwnCards() {
  std::lock_guard guard(life_->mutex); // an answer being taken in finishes first; a later one finds no owner
  life_->owner = nullptr;
}

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
  if (keys_ && keys_of_ == std::make_pair(identity->account_signing_public_key_b64, identity->kem_public_key_b64) &&
      keys_->account_id == identity->account_id) {
    return *keys_;
  }
  auto made = MakeKeyCard(identity->account_id, identity->account_signing_public_key_b64, identity->kem_public_key_b64,
                          [this](const ByteVector& bytes) { return identity_.SignBytes(bytes); });
  if (!made) {
    return made.error();
  }
  keys_ = std::move(made.value());
  keys_of_ = {identity->account_signing_public_key_b64, identity->kem_public_key_b64};
  return *keys_;
}

Roe<std::string> OwnCards::DevicePeerId() {
  auto identity = identity_.Get();
  if (!identity) {
    return identity.error();
  }
  return DevicePeerIdFromKey(identity->public_key_b64);
}

Roe<AccountCardDevice> OwnCards::Self() {
  auto identity = identity_.Get();
  if (!identity) {
    return identity.error();
  }
  auto peer_id = DevicePeerIdFromKey(identity->public_key_b64);
  if (!peer_id) {
    return peer_id.error();
  }
  if (identity->device_kem_public_key_b64.empty()) {
    return Error("device KEM key not set");
  }
  auto signing = TypedKeyB64(pp::BlobKind::kSignaturePublicKey, identity->public_key_b64);
  auto kem = TypedKeyB64(pp::BlobKind::kKemPublicKey, identity->device_kem_public_key_b64);
  if (!signing || !kem) {
    return Error("device keys do not decode");
  }
  return AccountCardDevice{
      .peer_id = *peer_id, .signing_public_key_b64 = std::move(*signing), .kem_public_key_b64 = std::move(*kem)};
}

Roe<std::optional<AccountCard>> OwnCards::NewestLocked(const int64_t now_ms) {
  auto identity = identity_.Get();
  if (!identity) {
    return identity.error();
  }
  auto kept = cards_.Kept(identity->account_id);
  if (!kept) {
    return kept.error();
  }
  std::optional<AccountCard> in_store;
  if (kept->has_value() && (*kept)->newest_card) {
    in_store = (*kept)->newest_card;
  }
  auto beside = identity_.OwnCard();
  if (!beside) {
    log().warning << "own card beside the identity unreadable: " << beside.error().message;
  }
  const std::optional<AccountCard> own = beside ? *beside : std::nullopt;
  if (own && (!in_store || own->issued_at > in_store->issued_at)) {
    // The chat store lost it (Reset chats, the start-up recovery's, a store too old to upgrade): our card goes back
    // into peer_cards, where everything else reads cards. Checked there as every card is.
    auto back = cards_.AddAccountCard(*own, std::max(now_ms, own->issued_at), "self");
    if (back && back->newest_card) {
      return std::optional<AccountCard>(back->newest_card);
    }
    log().warning << "own card not put back into the kept cards: "
                  << (back ? std::string("not taken") : back.error().message);
    return in_store;
  }
  if (in_store && (!own || in_store->issued_at > own->issued_at) && VerifiedLocked(*in_store, now_ms)) {
    // Newer here (issued, a sibling's, a linked device's, a restore's — or the first start of a build that keeps it
    // beside the identity): kept there too.
    if (auto beside_kept = identity_.KeepOwnCard(*in_store); !beside_kept) {
      log().warning << "own card not kept beside the identity: " << beside_kept.error().message;
    }
  }
  return in_store;
}

void OwnCards::Reconcile(const int64_t now_ms) {
  std::lock_guard lock(mutex_);
  (void)NewestLocked(now_ms);
}

namespace {

bool Lists(const std::vector<AccountCardDevice>& devices, const std::string& peer_id) {
  return std::any_of(devices.begin(), devices.end(),
                     [&peer_id](const AccountCardDevice& d) { return d.peer_id == peer_id; });
}

} // namespace

bool OwnCards::SetRelays(const std::vector<AccountCardRelay>& given) {
  std::vector<AccountCardRelay> relays;
  for (const AccountCardRelay& relay : given) {
    relays.push_back(relay);
    if (!VerifyCardRelays(relays)) {
      relays.pop_back(); // outside the bounds (not https, too long, no relay id, a duplicate, a fifth): not on the card
    }
  }
  std::lock_guard lock(mutex_);
  if (relays_ == relays) {
    return false;
  }
  relays_ = std::move(relays);
  return true;
}

std::vector<AccountCardRelay> OwnCards::RelaysLocked(const std::vector<AccountCardDevice>& devices,
                                                     const std::string& self,
                                                     const std::optional<AccountCard>& newest) const {
  const bool master = !devices.empty() && devices.front().peer_id == self;
  return master || !newest ? relays_ : newest->relays;
}

bool OwnCards::VerifiedLocked(const AccountCard& base, const int64_t now_ms) {
  if (!base.card_signature_b64.empty() && base.card_signature_b64 == verified_signature_) {
    return true;
  }
  // A kept card whose signature is over other bytes (one stepped up from version 2 by the profile.db ladder) is
  // re-issued, its list as it is.
  if (!VerifyAccountCard(base, std::max(now_ms, base.issued_at))) {
    return false;
  }
  verified_signature_ = base.card_signature_b64;
  return true;
}

Roe<AccountCard> OwnCards::IssueLocked(const std::optional<AccountCard>& base, CardContent content,
                                       const int64_t now_ms, bool& issued) {
  issued = false;
  auto keys = Keys();
  if (!keys) {
    return keys.error();
  }
  if (content.devices.size() > kMaxAccountCardDevices) {
    return Error("This account already has " + std::to_string(kMaxAccountCardDevices) + " devices");
  }
  if (base && SameKeys(base->keys, *keys) && base->nickname == content.nickname && base->devices == content.devices &&
      base->relays == content.relays && base->initiation_floor == content.initiation_floor &&
      VerifiedLocked(*base, now_ms)) {
    return *base; // it says it all: the card stands
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
  auto signed_card = SignAccountCard(AccountCard{.keys = *keys,
                                                 .nickname = std::move(content.nickname),
                                                 .devices = std::move(content.devices),
                                                 .relays = std::move(content.relays),
                                                 .initiation_floor = content.initiation_floor,
                                                 .issued_at = issued_at},
                                     [this](const ByteVector& bytes) { return identity_.SignBytes(bytes); });
  if (!signed_card) {
    return signed_card.error();
  }
  if (auto kept = cards_.AddAccountCard(*signed_card, std::max(now_ms, issued_at), "self"); !kept) {
    return kept.error();
  }
  if (auto beside = identity_.KeepOwnCard(*signed_card); !beside) {
    log().warning << "own card not kept beside the identity: " << beside.error().message; // the next read retries
  }
  verified_signature_ = signed_card->card_signature_b64;
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
  bool ask = false;
  bool recovery_changed = false;
  Roe<AccountCard> card = [&]() -> Roe<AccountCard> {
    std::lock_guard lock(mutex_);
    auto newest = NewestLocked(now_ms);
    if (!newest) {
      return newest.error();
    }
    auto self = Self();
    if (!self) {
      return self.error();
    }
    std::vector<AccountCardDevice> devices;
    if (identity->card_intent == kCardIntentFresh) {
      devices = {*self}; // a new account, restored, or the person chose a new list: this device its master
    } else if (!*newest) {
      // Our card is not kept here (none beside the identity either): it is asked of our relays, never replaced by a
      // fresh list — that would sign out every other device of the account.
      recovery_changed = recovery_ == OwnCardRecovery::None;
      if (recovery_changed) {
        recovery_ = OwnCardRecovery::Getting;
      }
      ask = RecoveryDueLocked(now_ms);
      recovery_in_flight_ = recovery_in_flight_ || ask;
      return Error(recovery_ == OwnCardRecovery::NotFound ? kNoOwnCardMessage : kAwaitingOwnCardMessage);
    } else if (Lists((*newest)->devices, self->peer_id)) {
      for (const AccountCardDevice& device : (*newest)->devices) {
        devices.push_back(device.peer_id == self->peer_id ? *self : device); // the list as it is, this entry now
      }
    } else if (identity->card_intent == kCardIntentJoin) {
      devices = (*newest)->devices; // linked: at the end of the master's list
      devices.push_back(*self);
    } else {
      return Error(kSignedOutMessage);
    }
    const bool master = devices.front().peer_id == self->peer_id;
    const bool stepped = !master && *newest && !VerifiedLocked(**newest, now_ms);
    if (stepped && (recovery_ != OwnCardRecovery::NotFound || relays_.empty())) {
      // A card stepped up on disk (profile.db 7 → 8: its relays gone, its signature over the old bytes). Keeping the
      // card signed is any listed device's (M021, amended by M026); the list stays the main device's. The main device
      // may have issued a newer card this device has not heard of: our relays are asked for it first, and the same
      // list is re-issued only once every relay answered without one (and this device knows its relays).
      recovery_changed = recovery_ == OwnCardRecovery::None;
      if (recovery_changed) {
        recovery_ = OwnCardRecovery::Getting;
      }
      ask = RecoveryDueLocked(now_ms);
      recovery_in_flight_ = recovery_in_flight_ || ask;
      return Error(kAwaitingMainCardMessage);
    }
    recovery_ = OwnCardRecovery::None;
    intent_done = !identity->card_intent.empty();
    // The stepped card's relays went with the step: this device's own (pinned) relays take their place.
    std::vector<AccountCardRelay> relays = stepped ? relays_ : RelaysLocked(devices, self->peer_id, *newest);
    return IssueLocked(*newest,
                       CardContent{.devices = std::move(devices),
                                   .nickname = identity->nickname,
                                   .relays = std::move(relays),
                                   .initiation_floor = identity->initiation_floor},
                       now_ms, issued);
  }();
  if (recovery_changed) {
    NotifyRecoveryChanged();
  }
  if (ask) {
    StartRecovery(identity->account_id);
  }
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

Roe<AccountCard> OwnCards::StartFreshList(const int64_t now_ms) {
  {
    std::lock_guard lock(mutex_);
    auto newest = NewestLocked(now_ms);
    if (!newest) {
      return newest.error();
    }
    if (*newest) {
      return Error("This device already has your device list");
    }
    if (recovery_ != OwnCardRecovery::NotFound) {
      // Only once every relay answered without our card: a relay that did not may still keep it.
      return Error("Your device list may still be found: try again when all your relays answer");
    }
  }
  auto marked = identity_.Modify([](LocalIdentity& identity) {
    identity.card_intent = kCardIntentFresh;
    return true;
  });
  if (!marked) {
    return marked.error();
  }
  auto card = Card(now_ms);
  NotifyRecoveryChanged();
  return card;
}

void OwnCards::SetOwnCardLookup(OwnCardLookup lookup) {
  std::lock_guard lock(mutex_);
  lookup_ = std::move(lookup);
}

void OwnCards::SetOnRecoveryChanged(std::function<void()> changed) {
  std::lock_guard lock(mutex_);
  on_recovery_changed_ = std::move(changed);
}

void OwnCards::NotifyRecoveryChanged() {
  std::function<void()> changed;
  {
    std::lock_guard lock(mutex_);
    changed = on_recovery_changed_;
  }
  if (changed) {
    changed();
  }
}

bool OwnCards::RecoveryDueLocked(const int64_t now_ms) const {
  return lookup_ && !recovery_in_flight_ && recovery_ != OwnCardRecovery::NotFound && now_ms >= recovery_retry_at_ms_;
}

void OwnCards::RetryRecovery(const int64_t now_ms) {
  {
    std::lock_guard lock(mutex_);
    if (recovery_ == OwnCardRecovery::None || recovery_in_flight_) {
      return; // a card is kept, or the question is out already
    }
    recovery_retry_at_ms_ = 0;
    if (recovery_ == OwnCardRecovery::NotFound) {
      recovery_ = OwnCardRecovery::Getting;
    }
  }
  (void)Card(now_ms);
}

void OwnCards::StartRecovery(const std::string& account_id) {
  OwnCardLookup lookup;
  {
    std::lock_guard lock(mutex_);
    lookup = lookup_;
  }
  if (!lookup) {
    std::lock_guard lock(mutex_);
    recovery_in_flight_ = false;
    return;
  }
  log().info << "no card of our own that verifies: asking our relays for it";
  lookup(account_id, [life = life_](const OwnCardAnswers& answers) {
    std::lock_guard guard(life->mutex);
    if (life->owner) {
      life->owner->OnRecoveryAnswers(answers, NowMs());
    }
  });
}

std::optional<AccountCard> OwnCards::OursAmong(const OwnCardAnswers& answers, const int64_t now_ms) {
  auto identity = identity_.Get();
  if (!identity) {
    return std::nullopt;
  }
  auto signing = TypedKeyB64(pp::BlobKind::kSignaturePublicKey, identity->account_signing_public_key_b64);
  auto kem = TypedKeyB64(pp::BlobKind::kKemPublicKey, identity->kem_public_key_b64);
  if (!signing || !kem) {
    return std::nullopt;
  }
  const KeyCard ours{.account_id = identity->account_id,
                     .signing_public_key_b64 = std::move(*signing),
                     .kem_public_key_b64 = std::move(*kem)};
  std::optional<AccountCard> best;
  for (const Object& json : answers.cards) {
    auto card = AccountCardFromJson(json);
    if (!card) {
      log().warning << "a relay's card of ours does not read: " << card.error().message;
      continue;
    }
    // Ours: our Account ID and our account keys, signed by our account key. Anything else is refused.
    if (!SameKeys(card->keys, ours)) {
      log().warning << "a relay served a card of another account (or other keys) as ours: refused";
      continue;
    }
    if (auto checked = VerifyAccountCard(*card, now_ms); !checked) {
      log().warning << "a relay's card of ours does not verify: " << checked.error().message;
      continue;
    }
    if (!best || card->issued_at > best->issued_at) {
      best = std::move(*card);
    }
  }
  return best;
}

void OwnCards::OnRecoveryAnswers(const OwnCardAnswers& answers, const int64_t now_ms) {
  const std::optional<AccountCard> found = OursAmong(answers, now_ms);
  bool recovered = false;
  bool not_found = false;
  {
    std::lock_guard lock(mutex_);
    recovery_in_flight_ = false;
    if (found) {
      auto kept = cards_.AddAccountCard(*found, std::max(now_ms, found->issued_at), kOwnCardRecoveredSource);
      if (kept) {
        (void)NewestLocked(now_ms); // beside the identity too
        recovered = true;
      } else {
        log().warning << "our card from a relay not kept: " << kept.error().message;
      }
    }
    if (recovered) {
      recovery_ = OwnCardRecovery::None;
      recovery_backoff_ms_ = 0;
      recovery_retry_at_ms_ = 0;
    } else if (answers.answered > 0 && answers.unreached == 0 && !found) {
      recovery_ = OwnCardRecovery::NotFound; // every relay answered, none keeps it: the person decides (StartFreshList)
      not_found = true;
    } else {
      // Not every relay reached, or the card found was refused (older than a floor we know): asked again later.
      recovery_backoff_ms_ =
          recovery_backoff_ms_ == 0 ? kRecoveryFirstRetryMs : std::min(recovery_backoff_ms_ * 2, kRecoveryMaxRetryMs);
      recovery_retry_at_ms_ = now_ms + recovery_backoff_ms_;
      recovery_ = OwnCardRecovery::Getting;
    }
  }
  log().info << "our card from our relays: "
             << (recovered ? "recovered"
                 : found   ? "not kept"
                           : "none")
             << " (" << answers.answered << " answered, " << answers.unreached << " unreached)";
  if (recovered || not_found) {
    // On as usual: listed, its entry brought up to date; left out, signed out (this device was taken off). None found
    // while a stepped card is kept: that list is re-issued here (Card); with no card kept, Card only says so.
    (void)Card(now_ms);
  }
  NotifyRecoveryChanged();
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
    auto newest = NewestLocked(now_ms);
    if (!newest) {
      return newest.error();
    }
    ours = *newest;
    if (auto kept = cards_.AddAccountCard(card, now_ms, "sibling"); !kept) {
      return kept.error();
    }
    auto now_newest = NewestLocked(now_ms);
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
    // The nickname, relays and floor are the account's: the newest card's stand (the master's next Card() brings
    // the relays to its own).
    return IssueLocked(newest_card,
                       CardContent{.devices = std::move(devices),
                                   .nickname = newest_card.nickname,
                                   .relays = newest_card.relays,
                                   .initiation_floor = newest_card.initiation_floor},
                       now_ms, issued);
  }();
  if (!result) {
    return result;
  }
  if (identity->nickname != result->nickname || identity->initiation_floor != result->initiation_floor) {
    LocalIdentity renamed = *identity;
    renamed.nickname = result->nickname;
    renamed.initiation_floor = result->initiation_floor;
    if (auto updated = identity_.Update(renamed); !updated) {
      return updated.error();
    }
  }
  if ((issued || !ours || !(*ours == *result)) && on_changed_) {
    on_changed_(*result);
  }
  if (on_joined_ && ours && !result->devices.empty()) {
    auto self = Self();
    const bool master = self && result->devices.front().peer_id == self->peer_id;
    const bool grew = std::any_of(result->devices.begin(), result->devices.end(),
                                  [&](const AccountCardDevice& d) { return !Lists(ours->devices, d.peer_id); });
    if (master && grew) {
      on_joined_(*result);
    }
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
  const int64_t now_ms = NowMs();
  bool ask = false;
  OwnDeviceList list;
  {
    std::lock_guard lock(mutex_);
    auto newest = NewestLocked(now_ms);
    if (!newest) {
      return newest.error();
    }
    list.self_peer_id = self->peer_id;
    if (*newest) {
      list.devices = (*newest)->devices;
    } else if (identity->card_intent != kCardIntentFresh) {
      // Asked for while it is being got back (Me → My devices): the question goes now when it is due.
      list.recovery = recovery_ == OwnCardRecovery::NotFound ? OwnCardRecovery::NotFound : OwnCardRecovery::Getting;
      ask = RecoveryDueLocked(now_ms);
      recovery_in_flight_ = recovery_in_flight_ || ask;
      if (recovery_ == OwnCardRecovery::None) {
        recovery_ = OwnCardRecovery::Getting;
      }
    }
    list.signed_out = identity->card_intent.empty() && *newest && !Lists(list.devices, self->peer_id);
  }
  if (ask) {
    StartRecovery(identity->account_id);
  }
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
    auto newest = NewestLocked(now_ms);
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
      std::rotate(devices.begin(), target, target + 1); // to the front; the rest keep their order
    } else {
      devices.erase(target);
    }
    std::vector<AccountCardRelay> relays = RelaysLocked(devices, self->peer_id, *newest);
    return IssueLocked(*newest,
                       CardContent{.devices = std::move(devices),
                                   .nickname = (*newest)->nickname,
                                   .relays = std::move(relays),
                                   .initiation_floor = (*newest)->initiation_floor},
                       now_ms, issued);
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
