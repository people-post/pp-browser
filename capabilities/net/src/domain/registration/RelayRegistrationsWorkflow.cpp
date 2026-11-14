#include "domain/registration/RelayRegistrationsWorkflow.h"

#include "domain/registration/RegistrationClient.h"
#include "lib/people/RegistrationStatus.h"

#include <algorithm>
#include <utility>

namespace pbr {

namespace {

int64_t CardIssuedAt(const RegistrationPublishOpts& publish) {
  return publish.account_card ? publish.account_card->getIf<int64_t>("issued_at").value_or(0) : 0;
}

/** That relay's registration nears its end (or has none): the preferred relay's rule (ShouldRenewRegistration). */
bool NearsItsEnd(const std::string& expires_at, const int64_t now_ms) {
  LocalIdentity probe;
  probe.registered = true;
  probe.registration_expires_at = expires_at;
  return ShouldRenewRegistration(probe, now_ms);
}

} // namespace

RelayRegistrationsWorkflow::RelayRegistrationsWorkflow(IdentityStore& identity) : identity_(identity) {
  redirectLogger("RelayRegistrations");
}

void RelayRegistrationsWorkflow::SetRelays(std::vector<Relay> relays) {
  std::lock_guard lock(mutex_);
  std::vector<State> next;
  for (Relay& relay : relays) {
    if (!relay.client || relay.origin.empty()) {
      continue;
    }
    State state;
    const auto known = std::find_if(states_.begin(), states_.end(),
                                    [&relay](const State& s) { return s.relay.origin == relay.origin; });
    if (known != states_.end()) {
      state = *known;
    }
    state.relay = std::move(relay);
    next.push_back(std::move(state));
  }
  states_ = std::move(next);
}

bool RelayRegistrationsWorkflow::DueLocked(const State& state, const int64_t now_ms, const int64_t card_issued_at) {
  if (now_ms < state.next_try_ms) {
    return false;
  }
  return !state.registered || NearsItsEnd(state.expires_at, now_ms) ||
         (card_issued_at > 0 && card_issued_at != state.card_issued_at);
}

std::vector<std::string> RelayRegistrationsWorkflow::Due(const int64_t now_ms) {
  auto identity = identity_.Get();
  if (!identity || !identity->registered || identity->account_id.empty()) {
    return {}; // registering is the person's act, on the preferred relay first
  }
  const int64_t card_issued_at = card_ ? CardIssuedAt(card_()) : 0;
  std::lock_guard lock(mutex_);
  std::vector<std::string> due;
  for (const State& state : states_) {
    if (DueLocked(state, now_ms, card_issued_at)) {
      due.push_back(state.relay.origin);
    }
  }
  return due;
}

void RelayRegistrationsWorkflow::RunDue(const int64_t now_ms) {
  if (running_.exchange(true)) {
    return;
  }
  struct Running {
    std::atomic<bool>& flag;
    ~Running() { flag = false; }
  } running{running_};
  const std::vector<std::string> due = Due(now_ms);
  if (due.empty()) {
    return;
  }
  auto identity = identity_.Get();
  if (!identity) {
    return;
  }
  const RegistrationPublishOpts publish = card_ ? card_() : RegistrationPublishOpts{};
  std::vector<Relay> relays;
  {
    std::lock_guard lock(mutex_);
    for (const State& state : states_) {
      if (std::find(due.begin(), due.end(), state.relay.origin) != due.end()) {
        relays.push_back(state.relay);
      }
    }
  }
  for (const Relay& relay : relays) {
    Register(relay, identity->nickname, publish, now_ms);
  }
}

void RelayRegistrationsWorkflow::Register(const Relay& relay, const std::string& nickname,
                                          const RegistrationPublishOpts& publish, const int64_t now_ms) {
  // The network half only: the identity keeps the preferred relay's registration, not this one's.
  auto result = RenewNetworkOnly(*relay.client, identity_, nickname, {}, publish);
  const bool ok = result && result->success;
  {
    std::lock_guard lock(mutex_);
    const auto state = std::find_if(states_.begin(), states_.end(),
                                    [&relay](const State& s) { return s.relay.origin == relay.origin; });
    if (state == states_.end()) {
      return; // no longer ours
    }
    if (ok) {
      state->registered = true;
      state->expires_at = result->expires_at;
      state->card_issued_at = CardIssuedAt(publish);
      state->backoff_ms = 0;
      state->next_try_ms = 0;
    } else {
      state->backoff_ms = state->backoff_ms == 0 ? kFirstRetryMs : std::min(state->backoff_ms * 2, kMaxRetryMs);
      state->next_try_ms = now_ms + state->backoff_ms;
    }
  }
  if (!ok) {
    log().warning << "registration on " << NormalizeRegistrationBackend(relay.origin)
                  << " failed: " << (result ? result->message : result.error().message);
    return;
  }
  log().info << "registered on " << NormalizeRegistrationBackend(relay.origin);
  if (on_registered_) {
    on_registered_(relay.origin);
  }
}

bool RelayRegistrationsWorkflow::Registered(const std::string& origin) const {
  std::lock_guard lock(mutex_);
  return std::any_of(states_.begin(), states_.end(),
                     [&origin](const State& s) { return s.registered && s.relay.origin == origin; });
}

std::vector<std::string> RelayRegistrationsWorkflow::RegisteredOrigins() const {
  std::lock_guard lock(mutex_);
  std::vector<std::string> out;
  for (const State& state : states_) {
    if (state.registered) {
      out.push_back(state.relay.origin);
    }
  }
  return out;
}

void RelayRegistrationsWorkflow::RevokeAll() {
  std::vector<Relay> relays;
  {
    std::lock_guard lock(mutex_);
    for (State& state : states_) {
      relays.push_back(state.relay);
      state.registered = false;
    }
  }
  for (const Relay& relay : relays) {
    if (auto revoked = RevokeRegistrationWithIdentity(*relay.client, identity_); !revoked) {
      log().warning << "revoke on " << NormalizeRegistrationBackend(relay.origin)
                    << " failed: " << revoked.error().message;
    }
  }
}

} // namespace pbr
