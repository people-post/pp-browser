#include "lib/net/RelayPins.h"

#include "lib/net/RelayOrigins.h"

#include "common/Utilities.h"
#include "foundation/error/AppError.h"

#include <algorithm>
#include <utility>

namespace pbr {

namespace {

/** The audience of one relay of ours (or the org's origin): its held certificate's transport PeerId. */
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
                        "relay " + origin + " does not prove the relay ID its address names; not used");
}

Error NotExpectedError(const std::string& origin) {
  return AppError::Network(Err::Network::RelayIdentityUnavailable,
                           "relay " + origin + ": its address names no relay ID (a relay link names it)");
}

Error FullError(const std::string& origin) {
  return AppError::Network(Err::Network::RelayIdentityUnavailable,
                           "relay " + origin + " not pinned: the pins are full of kept relays");
}

Error LaterError(const std::string& origin) {
  return AppError::Network(Err::Network::RelayIdentityUnavailable,
                           "relay " + origin + " served no certificate at the last ask; asked again later");
}

std::string ContactSlot(const std::string& key, const RelayId& relay_id) {
  return "contact|" + key + "|" + relay_id.ToString();
}

} // namespace

/** The audience of a contact's relay: the PeerId its certificate proves (RelayPins::ContactAudience). */
class ContactRelayAudience : public IRelayAudience {
public:
  ContactRelayAudience(std::shared_ptr<RelayPins> pins, std::string origin, const RelayId& relay_id,
                       const bool public_only)
      : pins_(std::move(pins)), origin_(std::move(origin)), relay_id_(relay_id), public_only_(public_only) {}
  Roe<std::string> PeerId() override { return pins_->ContactPeerId(origin_, relay_id_, public_only_); }
  void Refused(const std::string& code) override { pins_->RefusedContact(origin_, relay_id_, code, public_only_); }

private:
  std::shared_ptr<RelayPins> pins_;
  std::string origin_;
  RelayId relay_id_;
  bool public_only_ = true;
};

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

void RelayPins::ChangedRelay(const RelayId& relay_id) const {
  for (const std::string& origin : store_.OriginsOf(relay_id)) {
    Changed(origin);
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

std::shared_ptr<IRelayAudience> RelayPins::ContactAudience(const std::string& origin, const RelayId& relay_id,
                                                           const bool public_only) {
  const std::string slot = ContactSlot(RelayOriginKey(origin), relay_id) + (public_only ? "|public" : "");
  std::lock_guard lock(mutex_);
  if (auto kept = audiences_[slot].lock()) {
    return kept;
  }
  std::erase_if(audiences_, [](const auto& entry) { return entry.second.expired(); });
  auto audience =
      std::make_shared<ContactRelayAudience>(shared_from_this(), RelayOriginKey(origin), relay_id, public_only);
  audiences_[slot] = audience;
  return audience;
}

std::optional<RelayId> RelayPins::PinnedRelayId(const std::string& origin) const {
  const auto pin = store_.PinFor(origin);
  if (!pin || pin->Mismatched()) {
    return std::nullopt;
  }
  return pin->relay_id;
}

std::optional<RelayPin> RelayPins::PinFor(const std::string& origin) const {
  return store_.PinFor(origin);
}

bool RelayPins::IdentityMismatch(const std::string& origin) const {
  const auto pin = store_.PinFor(origin);
  return pin && pin->Mismatched();
}

std::optional<RelayCertificate> RelayPins::CertificateFor(const RelayId& relay_id) const {
  return store_.CertificateFor(relay_id);
}

std::optional<RelayCertificate> RelayPins::Usable(const std::string& origin, const RelayId& relay_id) const {
  auto certificate = store_.CertificateFor(relay_id);
  if (!certificate || !certificate->Lists(origin)) {
    return std::nullopt;
  }
  if (certificate->body.expires_at != 0 && certificate->body.expires_at <= Now()) {
    return std::nullopt; // asked again: a newer one may be out
  }
  return certificate;
}

std::optional<std::string> RelayPins::HeldAudience(const std::string& origin) const {
  const auto relay_id = PinnedRelayId(origin);
  const auto certificate = relay_id ? Usable(origin, *relay_id) : std::nullopt;
  if (!certificate) {
    return std::nullopt;
  }
  return certificate->audience_peer_id;
}

std::vector<RelayAddress> RelayPins::PinnedRelays(const std::vector<std::string>& origins) const {
  std::vector<RelayAddress> relays;
  for (const std::string& origin : origins) {
    if (auto relay_id = PinnedRelayId(origin)) {
      relays.push_back(RelayAddress{.origin = origin, .relay_id = *relay_id});
    }
  }
  return relays;
}

CardRelayCheck RelayPins::CheckCardRelay(const std::string& origin, const RelayId& relay_id) const {
  if (const auto pin = store_.PinFor(origin); pin && pin->relay_id != relay_id) {
    return CardRelayCheck::Mismatch;
  }
  if (const auto certificate = store_.CertificateFor(relay_id); certificate && !certificate->Lists(origin)) {
    return CardRelayCheck::Mismatch; // that relay's certificate says it does not answer there
  }
  return CardRelayCheck::Usable;
}

void RelayPins::NoteFetchFailed(const std::string& slot) {
  const int64_t now = Now();
  std::lock_guard lock(mutex_);
  FetchState& state = fetches_[slot];
  state.backoff_ms = state.backoff_ms == 0 ? std::min(kFirstRetryMs, kMaxOursRetryMs)
                                           : std::min(state.backoff_ms * 2, kMaxOursRetryMs);
  state.next_try_ms = now + state.backoff_ms;
}

bool RelayPins::MayFetch(const std::string& slot) const {
  const int64_t now = Now();
  std::lock_guard lock(mutex_);
  const auto it = fetches_.find(slot);
  return it == fetches_.end() || now >= it->second.next_try_ms;
}

bool RelayPins::PublicOnly(const std::string& key, const bool asked) const {
  if (asked) {
    return true;
  }
  std::lock_guard lock(mutex_);
  return public_only_.contains(key);
}

bool RelayPins::WrongAudienceMayAsk(const std::string& slot) {
  const int64_t now = Now();
  std::lock_guard lock(mutex_);
  FetchState& state = fetches_[slot];
  if (state.wrong_audience_at_ms > 0 && now - state.wrong_audience_at_ms < kWrongAudienceRefetchMs) {
    return false; // asked a moment ago
  }
  state.wrong_audience_at_ms = now;
  return true;
}

RelayPins::Asked RelayPins::Ask(const std::string& origin, const RelayId& expected, const bool public_only) {
  const std::string key = RelayOriginKey(origin);
  Asked asked;
  auto identity = FetchIdentity(key, PublicOnly(key, public_only));
  if (!identity) {
    asked.error = identity.error();
    return asked;
  }
  if (!expected.empty() && identity->relay_id != expected) {
    asked.outcome = Asked::Outcome::Mismatch;
    asked.mismatch = RelayMismatch::OtherRelay;
    asked.presented = identity->relay_id;
    return asked;
  }
  const RelayId against = expected.empty() ? identity->relay_id : expected;
  const RelayCertificateCheck check = ReceiveRelayCertificateB64(identity->certificate_b64, against, Now());
  if (check.verdict == RelayCertificateVerdict::NeedsUpdate) {
    log().warning << "relay " << key << " serves a certificate this app cannot read: " << check.detail;
    asked.error = check.ToError(); // a newer relay: not a mismatch, and nothing changes
    return asked;
  }
  if (!check.accepted()) {
    log().warning << "relay " << key << ": " << check.detail;
    asked.outcome = Asked::Outcome::Mismatch;
    asked.mismatch = check.verdict == RelayCertificateVerdict::OtherRelay ? RelayMismatch::OtherRelay
                                                                          : RelayMismatch::NoValidCertificate;
    return asked;
  }
  const RelayCertificate& certificate = *check.certificate;
  if (!certificate.Lists(key)) {
    asked.outcome = Asked::Outcome::Mismatch;
    asked.mismatch = RelayMismatch::OriginNotListed;
    asked.presented = certificate.relay_id;
    return asked;
  }
  const RelayCertificateOffer offer = store_.Offer(certificate, Now());
  const auto held = store_.CertificateFor(certificate.relay_id);
  switch (offer) {
    case RelayCertificateOffer::Adopted:
      log().info << "relay " << key << " (" << certificate.relay_id.ToString() << "): certificate "
                 << certificate.body.sequence << " kept, audience " << certificate.audience_peer_id;
      asked.adopted = true;
      break;
    case RelayCertificateOffer::Lower:
      log().warning << "relay " << key << " serves certificate " << certificate.body.sequence << ", below the "
                    << (held ? held->body.sequence : 0) << " held: the held one stays";
      break;
    case RelayCertificateOffer::Conflict:
      log().error << "relay " << key << " serves another certificate at the sequence held ("
                  << certificate.body.sequence << "): refused, the first stays";
      break;
    case RelayCertificateOffer::Same:
      break;
  }
  if (!held || !held->Lists(key)) {
    asked.outcome = Asked::Outcome::Mismatch; // the higher certificate held no longer lists this origin
    asked.mismatch = RelayMismatch::OriginNotListed;
    asked.presented = certificate.relay_id;
    return asked;
  }
  asked.outcome = Asked::Outcome::Proved;
  asked.presented = certificate.relay_id;
  asked.held = held;
  return asked;
}

bool RelayPins::Note(const std::string& key, const RelayPin& pin, const Asked& asked) {
  const int64_t now = Now();
  if (asked.outcome == Asked::Outcome::Proved) {
    if (store_.NoteCheck(key, RelayMismatch::None, RelayId{}, now)) {
      log().info << "relay " << key << " proves its address again (" << pin.relay_id.ToString() << ")";
      return true;
    }
    return false;
  }
  if (!store_.NoteCheck(key, asked.mismatch, asked.presented, now)) {
    return false; // seen so already (a restart): said once
  }
  log().error << "relay " << key << " does not prove " << pin.relay_id.ToString() << " as its address names ("
              << RelayMismatchName(asked.mismatch)
              << (asked.presented.empty() ? std::string() : ": " + asked.presented.ToString()) << "): not used";
  return true;
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
    if (const auto usable = Usable(key, pin->relay_id)) {
      return usable->audience_peer_id;
    }
    if (within_backoff && !MayFetch(key)) {
      return LaterError(key);
    }
    const Asked asked = Ask(key, pin->relay_id, public_only);
    switch (asked.outcome) {
      case Asked::Outcome::Proved: {
        {
          std::lock_guard lock(mutex_);
          fetches_.erase(key);
          checked_.insert(key);
        }
        if (Note(key, *pin, asked) || asked.adopted) {
          Changed(key);
        }
        return asked.held->audience_peer_id;
      }
      case Asked::Outcome::Mismatch:
        if (Note(key, *pin, asked)) {
          Changed(key);
        }
        return IdentityMismatchError(key);
      case Asked::Outcome::Unavailable:
        break;
    }
    NoteFetchFailed(key);
    log().warning << "relay " << key << ": no certificate held, none read now: " << asked.error->message;
    return *asked.error;
  }
  if (!FirstUse(key)) {
    return NotExpectedError(key); // MR008: only an address names a relay's id
  }
  if (within_backoff && !MayFetch(key)) {
    return LaterError(key);
  }
  const Asked asked = Ask(key, RelayId{}, public_only);
  if (asked.outcome != Asked::Outcome::Proved) {
    NoteFetchFailed(key);
    if (asked.outcome == Asked::Outcome::Mismatch) {
      log().warning << "relay " << key << " not pinned: " << RelayMismatchName(asked.mismatch);
      return IdentityMismatchError(key);
    }
    log().warning << "relay " << key << " not pinned: " << asked.error->message;
    return *asked.error;
  }
  if (store_.Expect(key, asked.presented, Now()) == PinOutcome::Full) {
    NoteFetchFailed(key);
    return FullError(key);
  }
  // First use (a developer build's bare origin, or the org's relay in a build that names none): its own certificate,
  // which a relay link would have named instead.
  log().warning << "relay " << key << " pinned to " << asked.presented.ToString()
                << " on first use: its address names no relay ID";
  {
    std::lock_guard lock(mutex_);
    fetches_.erase(key);
    checked_.insert(key);
  }
  Changed(key);
  return asked.held->audience_peer_id;
}

Roe<std::string> RelayPins::ContactPeerId(const std::string& origin, const RelayId& relay_id, const bool public_only) {
  const std::string key = RelayOriginKey(origin);
  if (key.empty() || relay_id.empty()) {
    return AppError::Network(Err::Network::RelayIdentityUnavailable, "no relay ID given for " + key);
  }
  if (const auto usable = Usable(key, relay_id)) {
    return usable->audience_peer_id;
  }
  const std::string slot = ContactSlot(key, relay_id);
  if (!MayFetch(slot)) {
    return LaterError(key);
  }
  const Asked asked = Ask(key, relay_id, public_only);
  switch (asked.outcome) {
    case Asked::Outcome::Proved: {
      std::lock_guard lock(mutex_);
      fetches_.erase(slot);
    }
      if (asked.adopted) {
        ChangedRelay(relay_id); // ours too, at another origin
      }
      return asked.held->audience_peer_id;
    case Asked::Outcome::Mismatch:
      NoteFetchFailed(slot);
      log().warning << "a contact's relay " << key << " does not prove " << relay_id.ToString() << " ("
                    << RelayMismatchName(asked.mismatch) << "): not used";
      return IdentityMismatchError(key);
    case Asked::Outcome::Unavailable:
      break;
  }
  NoteFetchFailed(slot);
  return *asked.error;
}

Roe<void> RelayPins::Expect(const std::string& origin, const RelayId& relay_id) {
  const std::string key = RelayOriginKey(origin);
  if (key.empty() || relay_id.empty()) {
    return NotExpectedError(key);
  }
  switch (store_.Expect(key, relay_id, Now())) {
    case PinOutcome::Full:
      return FullError(key);
    case PinOutcome::Pinned: {
      std::lock_guard lock(mutex_);
      fetches_.erase(key);
      checked_.erase(key); // a new expectation: its certificate is checked again
    }
      Changed(key);
      return {};
    case PinOutcome::Same:
      break;
  }
  return {};
}

Roe<RelayCertificateOffer> RelayPins::OfferCertificate(const RelayId& relay_id, const std::string& certificate_b64,
                                                       const std::string_view channel) {
  if (relay_id.empty() || certificate_b64.empty()) {
    return Error("no relay ID or no certificate");
  }
  const RelayCertificateCheck check = ReceiveRelayCertificateB64(certificate_b64, relay_id, Now());
  if (!check.accepted()) {
    log().warning << "the certificate " << channel << " gives for " << relay_id.ToString()
                  << " is refused: " << check.detail;
    return check.ToError();
  }
  const RelayCertificateOffer offer = store_.Offer(*check.certificate, Now());
  switch (offer) {
    case RelayCertificateOffer::Adopted:
      log().info << relay_id.ToString() << ": certificate " << check.certificate->body.sequence << " from " << channel
                 << " kept, audience " << check.certificate->audience_peer_id;
      ChangedRelay(relay_id);
      break;
    case RelayCertificateOffer::Conflict:
      log().error << relay_id.ToString() << ": " << channel << " gives another certificate at the sequence held ("
                  << check.certificate->body.sequence << "): refused, the first stays";
      break;
    case RelayCertificateOffer::Lower:
    case RelayCertificateOffer::Same:
      break;
  }
  return offer;
}

bool RelayPins::CheckPinned(const std::string& key, const RelayPin& pin) {
  const Asked asked = Ask(key, pin.relay_id, false);
  if (asked.outcome == Asked::Outcome::Unavailable) {
    log().info << "relay " << key << " not checked now: " << asked.error->message;
    return false; // asked again at the next pass
  }
  {
    std::lock_guard lock(mutex_);
    if (asked.outcome == Asked::Outcome::Proved) {
      checked_.insert(key);
    } else {
      checked_.erase(key);
    }
  }
  const bool changed = Note(key, pin, asked) || asked.adopted;
  if (changed) {
    Changed(key);
  }
  return changed;
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
  if (code != kWrongAudienceCode || !WrongAudienceMayAsk(key)) {
    return;
  }
  const auto pin = store_.PinFor(key);
  if (!pin) {
    return;
  }
  const Asked asked = Ask(key, pin->relay_id, public_only);
  switch (asked.outcome) {
    case Asked::Outcome::Unavailable:
      log().warning << "relay " << key
                    << " answered wrong_audience and serves no certificate now: " << asked.error->message;
      return;
    case Asked::Outcome::Proved:
      if (asked.adopted) {
        log().info << "relay " << key << " answered wrong_audience: its newer certificate names "
                   << asked.held->audience_peer_id << ", signed for from now on";
      } else {
        log().warning << "relay " << key << " answered wrong_audience for the PeerId its certificate names ("
                      << asked.held->audience_peer_id << "): treated as a failure of that relay";
      }
      if (Note(key, *pin, asked) || asked.adopted) {
        Changed(key);
      }
      return;
    case Asked::Outcome::Mismatch: {
      std::lock_guard lock(mutex_);
      checked_.erase(key);
    }
      if (Note(key, *pin, asked)) {
        Changed(key);
      }
      return;
  }
}

void RelayPins::RefusedContact(const std::string& origin, const RelayId& relay_id, const std::string& code,
                               const bool public_only) {
  const std::string key = RelayOriginKey(origin);
  if (code != kWrongAudienceCode || !WrongAudienceMayAsk(ContactSlot(key, relay_id))) {
    return;
  }
  const Asked asked = Ask(key, relay_id, public_only);
  if (asked.outcome == Asked::Outcome::Proved && asked.adopted) {
    log().info << "a contact's relay " << key << " answered wrong_audience: its newer certificate names "
               << asked.held->audience_peer_id;
    ChangedRelay(relay_id);
    return;
  }
  log().warning << "a contact's relay " << key << " answered wrong_audience"
                << (asked.outcome == Asked::Outcome::Proved ? " for the PeerId its certificate names" : "");
}

AddIdentityCheck RelayPins::CheckForAdd(const std::string& origin, const RelayId& relay_id, const bool public_only) {
  using Result = AddIdentityCheck::Result;
  const std::string key = RelayOriginKey(origin);
  const auto held_elsewhere = [this, &key](const RelayId& id) -> std::optional<std::string> {
    for (const std::string& holder : store_.OriginsOf(id)) {
      if (holder != key) {
        return holder;
      }
    }
    return std::nullopt;
  };
  if (!relay_id.empty()) {
    // What its link names must not contradict what this origin is expected to prove, and must not be a relay of ours
    // at another address already, before it is asked at all.
    if (const auto pin = store_.PinFor(key); pin && pin->relay_id != relay_id) {
      return AddIdentityCheck{.result = Result::Mismatch, .relay_id = relay_id};
    }
    if (const auto holder = held_elsewhere(relay_id)) {
      return AddIdentityCheck{.result = Result::Taken, .relay_id = relay_id, .holder = *holder};
    }
  }
  const Asked asked = Ask(key, relay_id, public_only);
  if (asked.outcome == Asked::Outcome::Unavailable) {
    log().info << "relay " << key << " proves no identity: " << asked.error->message;
    return AddIdentityCheck{.result = Result::Unavailable};
  }
  if (asked.outcome == Asked::Outcome::Mismatch) {
    log().warning << "relay " << key << " does not prove "
                  << (relay_id.empty() ? std::string("a relay ID") : relay_id.ToString()) << " ("
                  << RelayMismatchName(asked.mismatch) << "); not added";
    return AddIdentityCheck{.result = Result::Mismatch, .relay_id = relay_id.empty() ? asked.presented : relay_id};
  }
  const RelayId& proved = asked.presented;
  if (relay_id.empty()) {
    if (const auto holder = held_elsewhere(proved)) {
      return AddIdentityCheck{.result = Result::Taken, .relay_id = proved, .holder = *holder};
    }
    if (const auto pin = store_.PinFor(key); pin && pin->relay_id != proved) {
      return AddIdentityCheck{.result = Result::Mismatch, .relay_id = proved};
    }
  }
  // The link's relay id (or, for a developer build's bare origin, the certificate's: first use) is expected from now.
  switch (store_.Expect(key, proved, Now())) {
    case PinOutcome::Pinned: {
      std::lock_guard lock(mutex_);
      checked_.insert(key);
    }
      if (relay_id.empty()) {
        log().warning << "relay " << key << " pinned to " << proved.ToString()
                      << " on first use: added without a relay link";
      }
      Changed(key);
      return AddIdentityCheck{.result = Result::Ok, .relay_id = proved, .pinned_now = true};
    case PinOutcome::Same:
      if (store_.NoteCheck(key, RelayMismatch::None, RelayId{}, Now())) {
        Changed(key);
      }
      return AddIdentityCheck{.result = Result::Ok, .relay_id = proved};
    case PinOutcome::Full:
      break;
  }
  log().warning << "relay " << key << " not added: the pins are full of kept relays";
  return AddIdentityCheck{.result = Result::Unavailable};
}

void RelayPins::TakeOwnCard(const std::vector<RelayAddress>& relays) {
  if (!store_.TakeOwnCard(relays, Now())) {
    return;
  }
  for (const RelayAddress& relay : relays) {
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
