#include "lib/net/RelayPins.h"

#include "lib/net/RelayOrigins.h"

#include "common/Utilities.h"
#include "foundation/crypto/PeerIdForm.h"
#include "foundation/error/AppError.h"

#include <algorithm>
#include <utility>

namespace pbr {

namespace {

/** The audience of one relay of ours (or the org's origin): its expected PeerId. */
class PinnedRelayAudience : public IRelayAudience {
public:
  PinnedRelayAudience(std::shared_ptr<RelayPins> pins, std::string origin, const bool public_only)
      : pins_(std::move(pins)), origin_(std::move(origin)), public_only_(public_only) {}
  Roe<std::string> PeerId() override { return pins_->Audience(origin_, public_only_); }
  void Refused(const std::string& code) override { pins_->Refused(origin_, code, public_only_); }

private:
  std::shared_ptr<RelayPins> pins_;
  std::string origin_;
  bool public_only_ = false;
};

Error IdentityMismatchError(const std::string& origin) {
  return AppError::Auth(Err::Auth::RelayIdentityMismatch,
                        "relay " + origin + " answers to another PeerId than its address names; not used");
}

Error IdentityTakenError(const std::string& origin, const std::string& holder) {
  return AppError::Auth(Err::Auth::RelayIdentityTaken,
                        "relay_identity_taken: " + origin + " names the PeerId of " + holder);
}

Error NotExpectedError(const std::string& origin) {
  return AppError::Network(Err::Network::RelayIdentityUnavailable,
                           "relay " + origin + ": its address names no relay ID (a relay link names it)");
}

Error FullError(const std::string& origin) {
  return AppError::Network(Err::Network::RelayIdentityUnavailable,
                           "relay " + origin + " not pinned: the pins are full of kept relays");
}

} // namespace

RelayPins::RelayPins(std::string profile_data_dir)
    : store_(std::move(profile_data_dir)), fetch_([](const std::string& base_url, const bool public_only) {
        return FetchRelayIdentity(base_url, public_only);
      }) {
  redirectLogger("RelayPins");
  store_.Load();
}

void RelayPins::SetFetch(Fetch fetch) {
  std::lock_guard lock(hooks_mutex_);
  fetch_ = std::move(fetch);
}

void RelayPins::SetClock(std::function<int64_t()> now_ms) {
  std::lock_guard lock(hooks_mutex_);
  now_ms_ = std::move(now_ms);
}

void RelayPins::SetKeep(std::function<bool(const std::string& origin)> keep) {
  store_.SetKeep(std::move(keep));
}

void RelayPins::SetFirstUse(std::function<bool(const std::string& origin)> first_use) {
  std::lock_guard lock(hooks_mutex_);
  first_use_ = std::move(first_use);
}

void RelayPins::SetOnChanged(std::function<void(const std::string& origin)> on_changed) {
  std::lock_guard lock(hooks_mutex_);
  on_changed_ = std::move(on_changed);
}

void RelayPins::SetOnNotRegistered(std::function<void(const std::string& origin)> on_not_registered) {
  std::lock_guard lock(hooks_mutex_);
  on_not_registered_ = std::move(on_not_registered);
}

void RelayPins::SetOnUpgradeRequired(std::function<void(const std::string& origin)> on_upgrade_required) {
  std::lock_guard lock(hooks_mutex_);
  on_upgrade_required_ = std::move(on_upgrade_required);
}

void RelayPins::SetOnRelayOutdated(std::function<void(const std::string& origin)> on_relay_outdated) {
  std::lock_guard lock(hooks_mutex_);
  on_relay_outdated_ = std::move(on_relay_outdated);
}

int64_t RelayPins::Now() const {
  std::function<int64_t()> clock;
  {
    std::lock_guard lock(hooks_mutex_);
    clock = now_ms_;
  }
  return clock ? clock() : util::NowUnixMs();
}

bool RelayPins::FirstUse(const std::string& origin) const {
  std::function<bool(const std::string&)> first_use;
  {
    std::lock_guard lock(hooks_mutex_);
    first_use = first_use_;
  }
  return first_use && first_use(origin);
}

Roe<RelayIdentity> RelayPins::FetchIdentity(const std::string& origin, const bool public_only) const {
  Fetch fetch;
  {
    std::lock_guard lock(hooks_mutex_);
    fetch = fetch_;
  }
  return fetch ? fetch(origin, public_only) : Roe<RelayIdentity>(Error("no relay identity fetch"));
}

void RelayPins::Changed(const std::string& origin) const {
  std::function<void(const std::string&)> callback;
  {
    std::lock_guard lock(hooks_mutex_);
    callback = on_changed_;
  }
  if (callback) {
    callback(origin);
  }
}

std::shared_ptr<IRelayAudience> RelayPins::AudienceFor(const std::string& origin, const bool public_only) {
  const std::string key = RelayOriginKey(origin);
  const std::string slot = public_only ? key + "|public" : key;
  std::lock_guard lock(mutex_);
  if (public_only) {
    public_only_.insert(key);
  }
  if (auto kept = audiences_[slot].lock()) {
    return kept;
  }
  std::erase_if(audiences_, [](const auto& entry) { return entry.second.expired(); });
  auto audience = std::make_shared<PinnedRelayAudience>(shared_from_this(), key, public_only);
  audiences_[slot] = audience;
  return audience;
}

std::optional<std::string> RelayPins::PinnedPeerId(const std::string& origin) const {
  const auto pin = store_.PinFor(origin);
  if (!pin || pin->Mismatched()) {
    return std::nullopt;
  }
  return pin->peer_id;
}

std::optional<RelayPin> RelayPins::PinFor(const std::string& origin) const {
  return store_.PinFor(origin);
}

bool RelayPins::IdentityMismatch(const std::string& origin) const {
  const auto pin = store_.PinFor(origin);
  return pin && pin->Mismatched();
}

std::vector<AccountCardRelay> RelayPins::PinnedRelays(const std::vector<std::string>& origins) const {
  std::vector<AccountCardRelay> relays;
  for (const std::string& origin : origins) {
    if (auto peer_id = PinnedPeerId(origin)) {
      relays.push_back(AccountCardRelay{.origin = origin, .peer_id = std::move(*peer_id)});
    }
  }
  return relays;
}

CardRelayCheck RelayPins::CheckCardRelay(const std::string& origin, const std::string& peer_id) const {
  if (const auto pin = store_.PinFor(origin); pin && pin->peer_id != peer_id) {
    return CardRelayCheck::Mismatch;
  }
  if (const auto holder = store_.HolderOf(peer_id); holder && *holder != RelayOriginKey(origin)) {
    return CardRelayCheck::Mismatch;
  }
  return CardRelayCheck::Usable;
}

void RelayPins::NoteFetchFailed(const std::string& key) {
  const int64_t now = Now();
  std::lock_guard lock(mutex_);
  FetchState& state = fetches_[key];
  state.backoff_ms = state.backoff_ms == 0 ? std::min(kFirstRetryMs, kMaxOursRetryMs)
                                           : std::min(state.backoff_ms * 2, kMaxOursRetryMs);
  state.next_try_ms = now + state.backoff_ms;
}

bool RelayPins::PublicOnly(const std::string& key, const bool asked) const {
  if (asked) {
    return true;
  }
  std::lock_guard lock(mutex_);
  return public_only_.contains(key);
}

Roe<PinAnswer> RelayPins::AskAndPresent(const std::string& origin, std::string& presented, const bool public_only,
                                        const bool first_use) {
  auto identity = FetchIdentity(origin, PublicOnly(RelayOriginKey(origin), public_only));
  if (!identity) {
    return identity.error();
  }
  presented = identity->peer_id;
  return store_.Present(origin, identity->peer_id, Now(), first_use);
}

Roe<std::string> RelayPins::Audience(const std::string& origin, const bool public_only) {
  return AudienceAsking(origin, public_only, true);
}

Roe<std::string> RelayPins::AudienceAsking(const std::string& origin, const bool public_only,
                                           const bool within_backoff) {
  const std::string key = RelayOriginKey(origin);
  if (const auto pin = store_.PinFor(key)) {
    if (pin->Mismatched()) {
      return IdentityMismatchError(key);
    }
    return pin->peer_id;
  }
  if (!FirstUse(key)) {
    return NotExpectedError(key); // MR008: only an address names a relay's PeerId
  }
  if (within_backoff) {
    const int64_t now = Now();
    std::lock_guard lock(mutex_);
    if (const auto it = fetches_.find(key); it != fetches_.end() && now < it->second.next_try_ms) {
      return AppError::Network(Err::Network::RelayIdentityUnavailable,
                               "relay " + key + " named no identity at the last ask; asked again later");
    }
  }
  std::string presented;
  auto answer = AskAndPresent(key, presented, public_only, true);
  if (!answer) {
    NoteFetchFailed(key);
    log().warning << "relay " << key << " not pinned: " << answer.error().message;
    return answer.error();
  }
  switch (answer->outcome) {
    case PinOutcome::Pinned:
      // First use (a developer build's bare origin, or the org's relay in a build that ships no PeerId): its own
      // word, which a relay link would have named instead.
      log().warning << "relay " << key << " pinned to " << presented << " on first use: its address names no relay ID";
      {
        std::lock_guard lock(mutex_);
        fetches_.erase(key);
        checked_.insert(key);
      }
      Changed(key);
      return presented;
    case PinOutcome::Same:
      return presented;
    case PinOutcome::Mismatch:
      Changed(key);
      return IdentityMismatchError(key);
    case PinOutcome::Taken:
      NoteFetchFailed(key);
      log().warning << "relay " << key << " claims the PeerId of " << answer->holder << "; never signed for";
      return IdentityTakenError(key, answer->holder);
    case PinOutcome::Full:
      NoteFetchFailed(key);
      return FullError(key);
    case PinOutcome::NotExpected:
      break;
  }
  return NotExpectedError(key);
}

Roe<void> RelayPins::Expect(const std::string& origin, const std::string& peer_id) {
  const std::string key = RelayOriginKey(origin);
  if (key.empty() || !IsMlDsaPeerIdForm(peer_id)) {
    return NotExpectedError(key);
  }
  const PinAnswer answer = store_.Expect(key, peer_id, Now());
  switch (answer.outcome) {
    case PinOutcome::Taken:
      log().warning << "relay " << key << " is named " << peer_id << ", the PeerId of " << answer.holder
                    << "; not used";
      return IdentityTakenError(key, answer.holder);
    case PinOutcome::Full:
      return FullError(key);
    case PinOutcome::Pinned: {
      std::lock_guard lock(mutex_);
      fetches_.erase(key);
      checked_.erase(key); // a new expectation: its identity is checked again
    }
      Changed(key);
      return {};
    case PinOutcome::Same:
    case PinOutcome::Mismatch:
    case PinOutcome::NotExpected:
      break;
  }
  return {};
}

bool RelayPins::CheckPinned(const std::string& key, const RelayPin& pin) {
  std::string presented;
  auto answer = AskAndPresent(key, presented, false, false);
  if (!answer) {
    log().info << "relay " << key << " not checked now: " << answer.error().message;
    return false; // asked again at the next pass
  }
  if (answer->outcome == PinOutcome::Same) {
    {
      std::lock_guard lock(mutex_);
      checked_.insert(key);
    }
    if (pin.Mismatched()) {
      log().info << "relay " << key << " answers to its address again (" << pin.peer_id << ")";
      Changed(key);
      return true;
    }
    return false;
  }
  if (answer->outcome != PinOutcome::Mismatch) {
    return false;
  }
  if (pin.presented_peer_id == presented) {
    return false; // seen so already (a restart): said once
  }
  log().error << "relay " << key << " answers to " << presented << ", not to " << pin.peer_id
              << " as its address names: not used";
  Changed(key);
  return true;
}

bool RelayPins::CheckIdentities(const std::vector<std::string>& origins) {
  bool changed = false;
  for (const std::string& origin : origins) {
    const std::string key = RelayOriginKey(origin);
    const auto pin = store_.PinFor(key);
    if (!pin) {
      if (FirstUse(key)) {
        changed = AudienceAsking(key, false, false).isOk() || changed;
      }
      continue;
    }
    {
      std::lock_guard lock(mutex_);
      if (checked_.contains(key) && !pin->Mismatched()) {
        continue; // checked this run already
      }
    }
    changed = CheckPinned(key, *pin) || changed;
  }
  return changed;
}

void RelayPins::Refused(const std::string& origin, const std::string& code, const bool public_only) {
  const std::string key = RelayOriginKey(origin);
  const bool lost = code == kNotRegisteredCode || code == kRegistrationExpiredCode;
  const bool outdated = code == kRelayUpgradeRequiredCode;
  if (lost || outdated || code == kClientUpgradeRequiredCode) {
    std::function<void(const std::string&)> callback;
    {
      std::lock_guard lock(hooks_mutex_);
      callback = lost ? on_not_registered_ : (outdated ? on_relay_outdated_ : on_upgrade_required_);
    }
    if (callback) {
      callback(key);
    }
    return;
  }
  if (code != kWrongAudienceCode) {
    return;
  }
  {
    const int64_t now = Now();
    std::lock_guard lock(mutex_);
    FetchState& state = fetches_[key];
    if (state.wrong_audience_at_ms > 0 && now - state.wrong_audience_at_ms < kWrongAudienceRefetchMs) {
      return; // asked a moment ago
    }
    state.wrong_audience_at_ms = now;
  }
  std::string presented;
  auto answer = AskAndPresent(key, presented, public_only, false);
  if (!answer) {
    log().warning << "relay " << key
                  << " answered wrong_audience and names no identity now: " << answer.error().message;
    return;
  }
  if (answer->outcome == PinOutcome::Same) {
    log().warning << "relay " << key << " answered wrong_audience for the PeerId it presents (" << presented
                  << "): treated as a failure of that relay";
    return;
  }
  if (answer->outcome == PinOutcome::Mismatch) {
    const auto pin = store_.PinFor(key);
    log().error << "relay " << key << " answers to " << presented << ", not to " << (pin ? pin->peer_id : std::string())
                << " as its address names: not used";
    {
      std::lock_guard lock(mutex_);
      checked_.erase(key);
    }
    Changed(key);
  }
}

AddIdentityCheck RelayPins::CheckForAdd(const std::string& origin, const std::string& peer_id, const bool public_only) {
  using Result = AddIdentityCheck::Result;
  const std::string key = RelayOriginKey(origin);
  if (!peer_id.empty()) {
    // What its link names must be free (no other origin of ours or the org's holds it) and not contradict what this
    // origin is expected to answer to, before it is asked at all.
    if (const auto holder = store_.HolderOf(peer_id); holder && *holder != key) {
      return AddIdentityCheck{.result = Result::Taken, .peer_id = peer_id, .holder = *holder};
    }
    if (const auto pin = store_.PinFor(key); pin && pin->peer_id != peer_id) {
      return AddIdentityCheck{.result = Result::Mismatch, .peer_id = peer_id};
    }
  }
  auto identity = FetchIdentity(key, public_only);
  if (!identity) {
    log().info << "relay " << key << " names no identity: " << identity.error().message;
    return AddIdentityCheck{.result = Result::Unavailable};
  }
  const std::string& presented = identity->peer_id;
  if (!peer_id.empty() && presented != peer_id) {
    log().warning << "relay " << key << " answers to " << presented << ", not to " << peer_id
                  << " as its link names; not added";
    return AddIdentityCheck{.result = Result::Mismatch, .peer_id = peer_id};
  }
  if (const auto holder = store_.HolderOf(presented); holder && *holder != key) {
    return AddIdentityCheck{.result = Result::Taken, .peer_id = presented, .holder = *holder};
  }
  if (const auto pin = store_.PinFor(key); pin && pin->peer_id != presented) {
    return AddIdentityCheck{.result = Result::Mismatch, .peer_id = presented};
  }
  // The link's PeerId (or, for a developer build's bare origin, the answer's: first use) is expected from now on.
  const PinAnswer answer = store_.Expect(key, presented, Now());
  switch (answer.outcome) {
    case PinOutcome::Pinned: {
      std::lock_guard lock(mutex_);
      checked_.insert(key);
    }
      if (peer_id.empty()) {
        log().warning << "relay " << key << " pinned to " << presented << " on first use: added without a relay link";
      }
      Changed(key);
      return AddIdentityCheck{.result = Result::Ok, .peer_id = presented, .pinned_now = true};
    case PinOutcome::Same:
      return AddIdentityCheck{.result = Result::Ok, .peer_id = presented};
    case PinOutcome::Taken:
      return AddIdentityCheck{.result = Result::Taken, .peer_id = presented, .holder = answer.holder};
    case PinOutcome::Mismatch:
    case PinOutcome::Full:
    case PinOutcome::NotExpected:
      break;
  }
  log().warning << "relay " << key << " not added: the pins are full of kept relays";
  return AddIdentityCheck{.result = Result::Unavailable};
}

void RelayPins::TakeOwnCard(const std::vector<OwnCardRelay>& relays) {
  if (!store_.TakeOwnCard(relays, Now())) {
    return;
  }
  for (const OwnCardRelay& relay : relays) {
    {
      std::lock_guard lock(mutex_);
      checked_.erase(RelayOriginKey(relay.origin));
    }
    Changed(RelayOriginKey(relay.origin));
  }
}

void RelayPins::Drop(const std::string& origin) {
  if (!store_.PinFor(origin)) {
    return;
  }
  store_.Drop(origin);
  {
    std::lock_guard lock(mutex_);
    checked_.erase(RelayOriginKey(origin));
  }
  Changed(RelayOriginKey(origin));
}

} // namespace pbr
