#include "lib/net/RelayPins.h"

#include "lib/net/RelayOrigins.h"

#include "common/Utilities.h"
#include "foundation/error/AppError.h"

#include <algorithm>
#include <utility>

namespace pbr {

namespace {

/** The audience of one relay of ours: its pin, asked for when there is none. */
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

/**
 * The audience of a relay not ours (a QR locator's): what it presents, never pinned (RelayPins::ForeignAudienceFor).
 * Its refusals of our registration there are not ours to heal: only `wrong_audience` is heard.
 */
class ForeignRelayAudience : public IRelayAudience {
public:
  ForeignRelayAudience(std::shared_ptr<RelayPins> pins, std::string origin, const bool public_only)
      : pins_(std::move(pins)), origin_(std::move(origin)), public_only_(public_only) {}
  Roe<std::string> PeerId() override { return pins_->ForeignAudience(origin_, public_only_); }
  void Refused(const std::string& code) override { pins_->ForeignRefused(origin_, code); }

private:
  std::shared_ptr<RelayPins> pins_;
  std::string origin_;
  bool public_only_ = true;
};

/** Where a relay not ours keeps its fetch state: apart from the same origin's as ours (an Add pins it at once). */
std::string ForeignSlot(const std::string& key) {
  return key + "|foreign";
}

Error IdentityChangedError(const std::string& origin) {
  return AppError::Auth(Err::Auth::RelayIdentityChanged,
                        "relay " + origin + " presented another identity; not accepted yet");
}

Error IdentityTakenError(const std::string& origin, const std::string& holder) {
  return AppError::Auth(Err::Auth::RelayIdentityTaken,
                        "relay_identity_taken: " + origin + " claims the PeerId pinned to " + holder);
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

void RelayPins::SetOnChanged(std::function<void(const std::string& origin)> on_changed) {
  std::lock_guard lock(hooks_mutex_);
  on_changed_ = std::move(on_changed);
}

void RelayPins::SetOnNotRegistered(std::function<void(const std::string& origin)> on_not_registered) {
  std::lock_guard lock(hooks_mutex_);
  on_not_registered_ = std::move(on_not_registered);
}

int64_t RelayPins::Now() const {
  std::function<int64_t()> clock;
  {
    std::lock_guard lock(hooks_mutex_);
    clock = now_ms_;
  }
  return clock ? clock() : util::NowUnixMs();
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

std::shared_ptr<IRelayAudience> RelayPins::ForeignAudienceFor(const std::string& origin, const bool public_only) {
  // One per client (the sender keeps a client per relay it sends into): what it presented is the engine's.
  return std::make_shared<ForeignRelayAudience>(shared_from_this(), RelayOriginKey(origin), public_only);
}

std::optional<std::string> RelayPins::PinnedPeerId(const std::string& origin) const {
  const auto pin = store_.PinFor(origin);
  if (!pin || pin->Changed()) {
    return std::nullopt;
  }
  return pin->peer_id;
}

std::optional<RelayPin> RelayPins::PinFor(const std::string& origin) const {
  return store_.PinFor(origin);
}

bool RelayPins::IdentityChanged(const std::string& origin) const {
  const auto pin = store_.PinFor(origin);
  return pin && pin->Changed();
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

void RelayPins::NoteFetchFailed(const std::string& slot, const int64_t max_ms) {
  const int64_t now = Now();
  std::lock_guard lock(mutex_);
  FetchState& state = fetches_[slot];
  state.backoff_ms = state.backoff_ms == 0 ? std::min(kFirstRetryMs, max_ms) : std::min(state.backoff_ms * 2, max_ms);
  state.next_try_ms = now + state.backoff_ms;
}

bool RelayPins::PublicOnly(const std::string& key, const bool asked) const {
  if (asked) {
    return true;
  }
  std::lock_guard lock(mutex_);
  return public_only_.contains(key);
}

Roe<PinAnswer> RelayPins::AskAndPresent(const std::string& origin, std::string& presented, const bool public_only) {
  Fetch fetch;
  {
    std::lock_guard lock(hooks_mutex_);
    fetch = fetch_;
  }
  auto identity = fetch ? fetch(origin, PublicOnly(RelayOriginKey(origin), public_only))
                        : Roe<RelayIdentity>(Error("no relay identity fetch"));
  if (!identity) {
    return identity.error();
  }
  presented = identity->peer_id;
  return store_.Present(origin, identity->peer_id, Now());
}

Roe<std::string> RelayPins::Audience(const std::string& origin, const bool public_only) {
  return AudienceAsking(origin, public_only, true);
}

Roe<std::string> RelayPins::AudienceAsking(const std::string& origin, const bool public_only,
                                           const bool within_backoff) {
  const std::string key = RelayOriginKey(origin);
  if (const auto pin = store_.PinFor(key)) {
    if (pin->Changed()) {
      return IdentityChangedError(key);
    }
    return pin->peer_id;
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
  auto answer = AskAndPresent(key, presented, public_only);
  if (!answer) {
    NoteFetchFailed(key, kMaxOursRetryMs);
    log().warning << "relay " << key << " not pinned: " << answer.error().message;
    return answer.error();
  }
  switch (answer->outcome) {
    case PinOutcome::Pinned:
      log().info << "relay " << key << " pinned to " << presented;
      {
        std::lock_guard lock(mutex_);
        fetches_.erase(key);
      }
      Changed(key);
      return presented;
    case PinOutcome::Same:
      return presented;
    case PinOutcome::Changed:
      Changed(key);
      return IdentityChangedError(key);
    case PinOutcome::Taken:
      NoteFetchFailed(key, kMaxOursRetryMs);
      log().warning << "relay " << key << " claims the PeerId of " << answer->holder << "; never signed for";
      return IdentityTakenError(key, answer->holder);
    case PinOutcome::Full:
      NoteFetchFailed(key, kMaxOursRetryMs);
      return AppError::Network(Err::Network::RelayIdentityUnavailable,
                               "relay " + key + " not pinned: the pins are full of kept relays");
  }
  return IdentityChangedError(key);
}

bool RelayPins::EnsurePinned(const std::vector<std::string>& origins) {
  bool pinned = false;
  for (const std::string& origin : origins) {
    if (store_.PinFor(origin)) {
      continue;
    }
    pinned = AudienceAsking(origin, false, false).isOk() || pinned;
  }
  return pinned;
}

void RelayPins::Refused(const std::string& origin, const std::string& code, const bool public_only) {
  const std::string key = RelayOriginKey(origin);
  if (code == kNotRegisteredCode || code == kRegistrationExpiredCode) {
    std::function<void(const std::string&)> callback;
    {
      std::lock_guard lock(hooks_mutex_);
      callback = on_not_registered_;
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
  auto answer = AskAndPresent(key, presented, public_only);
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
  if (answer->outcome == PinOutcome::Changed) {
    const auto pin = store_.PinFor(key);
    log().warning << "relay " << key << " identity changed: pinned " << (pin ? pin->peer_id : std::string())
                  << ", presented " << presented << "; nothing is signed for it until accepted";
  }
  Changed(key);
}

Roe<std::string> RelayPins::ForeignAudience(const std::string& origin, const bool public_only) {
  const std::string key = RelayOriginKey(origin);
  if (store_.PinFor(key)) {
    return Audience(key, public_only); // a relay of ours (or the org's) the locator names: its pin
  }
  const std::string slot = ForeignSlot(key);
  std::string kept;
  {
    const int64_t now = Now();
    std::lock_guard lock(mutex_);
    if (const auto it = foreign_.find(key); it != foreign_.end()) {
      kept = it->second;
    } else if (const auto state = fetches_.find(slot); state != fetches_.end() && now < state->second.next_try_ms) {
      return AppError::Network(Err::Network::RelayIdentityUnavailable,
                               "relay " + key + " named no identity at the last ask; asked again later");
    }
  }
  if (kept.empty()) {
    Fetch fetch;
    {
      std::lock_guard lock(hooks_mutex_);
      fetch = fetch_;
    }
    auto identity = fetch ? fetch(key, public_only) : Roe<RelayIdentity>(Error("no relay identity fetch"));
    if (!identity) {
      NoteFetchFailed(slot, kMaxRetryMs);
      log().info << "relay " << key << " (not ours) names no identity: " << identity.error().message;
      return identity.error();
    }
    kept = identity->peer_id;
  }
  // Checked at every use: a pin of ours taken since wins over what a relay not ours presented.
  if (const auto holder = store_.HolderOf(kept); holder && *holder != key) {
    NoteFetchFailed(slot, kMaxRetryMs);
    std::lock_guard lock(mutex_);
    foreign_.erase(key);
    log().warning << "relay " << key << " (not ours) claims the PeerId of " << *holder << "; never signed for";
    return IdentityTakenError(key, *holder);
  }
  std::lock_guard lock(mutex_);
  if (!foreign_.contains(key) && foreign_.size() >= kMaxForeign) {
    foreign_.erase(foreign_.begin()); // any one: asked again at its next send
  }
  foreign_[key] = kept;
  if (const auto state = fetches_.find(slot); state != fetches_.end()) {
    state->second.backoff_ms = 0; // answered: no backoff (its wrong_audience throttle stays)
    state->second.next_try_ms = 0;
  }
  return kept;
}

void RelayPins::ForeignRefused(const std::string& origin, const std::string& code) {
  if (code != kWrongAudienceCode) {
    return; // our registration at a relay not ours is not ours to heal
  }
  const std::string key = RelayOriginKey(origin);
  const int64_t now = Now();
  std::lock_guard lock(mutex_);
  FetchState& state = fetches_[ForeignSlot(key)];
  if (state.wrong_audience_at_ms > 0 && now - state.wrong_audience_at_ms < kWrongAudienceRefetchMs) {
    return; // forgotten a moment ago
  }
  state.wrong_audience_at_ms = now;
  foreign_.erase(key); // asked again at the next send
}

AddIdentityCheck RelayPins::CheckForAdd(const std::string& origin, const bool public_only) {
  const std::string key = RelayOriginKey(origin);
  Fetch fetch;
  {
    std::lock_guard lock(hooks_mutex_);
    fetch = fetch_;
  }
  auto identity = fetch ? fetch(key, public_only) : Roe<RelayIdentity>(Error("no relay identity fetch"));
  if (!identity) {
    log().info << "relay " << key << " names no identity: " << identity.error().message;
    return AddIdentityCheck{.result = AddIdentityCheck::Result::Unavailable};
  }
  const std::string& peer_id = identity->peer_id;
  if (const auto holder = store_.HolderOf(peer_id); holder && *holder != key) {
    return AddIdentityCheck{.result = AddIdentityCheck::Result::Taken, .peer_id = peer_id, .holder = *holder};
  }
  if (const auto pin = store_.PinFor(key); pin && pin->peer_id != peer_id) {
    return AddIdentityCheck{.result = AddIdentityCheck::Result::ChangedSinceSeen, .peer_id = peer_id};
  }
  const PinAnswer answer = store_.Present(key, peer_id, Now());
  switch (answer.outcome) {
    case PinOutcome::Pinned:
      Changed(key);
      return AddIdentityCheck{.result = AddIdentityCheck::Result::Ok, .peer_id = peer_id, .pinned_now = true};
    case PinOutcome::Same:
      return AddIdentityCheck{.result = AddIdentityCheck::Result::Ok, .peer_id = peer_id};
    case PinOutcome::Changed: // pinned with another between the checks above and now
      return AddIdentityCheck{.result = AddIdentityCheck::Result::ChangedSinceSeen, .peer_id = peer_id};
    case PinOutcome::Taken:
      return AddIdentityCheck{.result = AddIdentityCheck::Result::Taken, .peer_id = peer_id, .holder = answer.holder};
    case PinOutcome::Full:
      break;
  }
  log().warning << "relay " << key << " not added: the pins are full of kept relays";
  return AddIdentityCheck{.result = AddIdentityCheck::Result::Unavailable};
}

Roe<void> RelayPins::Accept(const std::string& origin) {
  const auto pin = store_.PinFor(origin);
  if (!pin || !pin->Changed()) {
    return Error("relay_identity_not_changed");
  }
  if (auto repinned = Repin(origin, pin->presented_peer_id); !repinned) {
    return repinned;
  }
  log().info << "relay " << pin->origin << " re-pinned to " << pin->presented_peer_id << " (accepted)";
  return {};
}

Roe<void> RelayPins::Repin(const std::string& origin, const std::string& peer_id) {
  const PinAnswer answer = store_.Repin(origin, peer_id, Now());
  if (answer.outcome == PinOutcome::Taken) {
    return IdentityTakenError(RelayOriginKey(origin), answer.holder);
  }
  if (answer.outcome == PinOutcome::Full) {
    return AppError::Network(Err::Network::RelayIdentityUnavailable,
                             "relay " + RelayOriginKey(origin) + " not pinned: the pins are full of kept relays");
  }
  {
    std::lock_guard lock(mutex_);
    fetches_.erase(RelayOriginKey(origin));
  }
  Changed(RelayOriginKey(origin));
  return {};
}

void RelayPins::TakeOwnCard(const std::vector<OwnCardRelay>& relays) {
  if (!store_.TakeOwnCard(relays, Now())) {
    return;
  }
  for (const OwnCardRelay& relay : relays) {
    Changed(RelayOriginKey(relay.origin));
  }
}

void RelayPins::Drop(const std::string& origin) {
  if (!store_.PinFor(origin)) {
    return;
  }
  store_.Drop(origin);
  Changed(RelayOriginKey(origin));
}

} // namespace pbr
