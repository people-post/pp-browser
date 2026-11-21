#include "domain/registration/RelayRegistrationsWorkflow.h"

#include "domain/registration/RegistrationClient.h"
#include "lib/net/RelayAudience.h"
#include "lib/net/RelayOrigins.h"
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
    } else if (relay.last.state == RelayRegistrationRecord::State::Registered) {
      state.registered = true; // as it answered before this start: due again for a newer card or near its end
      state.expires_at = relay.last.expires_at;
      state.card_issued_at = relay.last.card_issued_at;
      state.registration_id = relay.last.registration_id;
    }
    state.relay = std::move(relay);
    next.push_back(std::move(state));
  }
  states_ = std::move(next);
}

bool RelayRegistrationsWorkflow::DueLocked(const State& state, const int64_t now_ms, const int64_t card_issued_at) {
  if (state.leaving || now_ms < state.next_try_ms) {
    return false;
  }
  return !state.registered || NearsItsEnd(state.expires_at, now_ms) ||
         (card_issued_at > 0 && card_issued_at != state.card_issued_at);
}

std::vector<std::string> RelayRegistrationsWorkflow::Due(const int64_t now_ms) {
  if (closed_.load(std::memory_order_acquire)) {
    return {};
  }
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
  const std::unique_lock running(run_mutex_, std::try_to_lock);
  if (!running.owns_lock() || closed_.load(std::memory_order_acquire)) {
    return;
  }
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
    if (state == states_.end() || state->leaving) {
      if (state != states_.end() && ok && !result->registration_id.empty()) {
        state->registration_id = result->registration_id; // the one the revoke after this run removes
      }
      log().info << "registration on " << NormalizeRegistrationBackend(relay.origin)
                 << " answered after it left our relays; dropped";
      return; // no longer ours: revoked there after this answer (Revoke waits for this run)
    }
    if (ok) {
      state->registered = true;
      state->expires_at = result->expires_at;
      state->card_issued_at = CardIssuedAt(publish);
      if (!result->registration_id.empty()) {
        state->registration_id = result->registration_id;
      }
      state->backoff_ms = 0;
      state->next_try_ms = 0;
    } else if (!result && IsPermanentRelayRefusal(result.error())) {
      state->next_try_ms = now_ms + kRefusedRetryMs; // until one side is updated: once a day (T17)
    } else {
      state->backoff_ms = state->backoff_ms == 0 ? kFirstRetryMs : std::min(state->backoff_ms * 2, kMaxRetryMs);
      state->next_try_ms = now_ms + state->backoff_ms;
    }
  }
  const std::string reason = ok ? std::string() : (result ? result->message : result.error().message);
  // The page words the code; the log keeps what the relay or the network said.
  const std::string code =
      ok ? std::string() : (result ? kRegistrationRefused : RelayRegistrationFailureCode(result.error()));
  if (on_result_) {
    on_result_(
        relay.origin,
        ok ? RelayRegistrationRecord{.state = RelayRegistrationRecord::State::Registered,
                                     .expires_at = result->expires_at,
                                     .card_issued_at = CardIssuedAt(publish),
                                     .registration_id = result->registration_id,
                                     .at_ms = now_ms}
           : RelayRegistrationRecord{.state = RelayRegistrationRecord::State::Failed, .reason = code, .at_ms = now_ms});
  }
  if (!ok) {
    log().warning << "registration on " << NormalizeRegistrationBackend(relay.origin) << " failed: " << reason;
    return;
  }
  log().info << "registered on " << NormalizeRegistrationBackend(relay.origin);
  KeepBriefLlmKey(*result);
  if (on_registered_) {
    on_registered_(relay.origin);
  }
}

void RelayRegistrationsWorkflow::KeepBriefLlmKey(const RegistrationResult& result) {
  // Only the org's relay answers with one (HttpRegistrationClient::SetTakesBriefLlmKey, MR006): the key is the
  // org's, wherever our preferred relay is.
  if (result.llm_token.empty()) {
    return;
  }
  if (auto kept = identity_.Modify([&result](LocalIdentity& identity) {
        if (identity.brief_llm_api_key == result.llm_token &&
            identity.brief_llm_token_expires_at == result.llm_token_expires_at) {
          return false;
        }
        identity.brief_llm_api_key = result.llm_token;
        identity.brief_llm_token_expires_at = result.llm_token_expires_at;
        identity.brief_llm_guest_api_key.clear();
        return true;
      });
      !kept) {
    log().warning << "Brief LLM key not kept: " << kept.error().message;
  }
}

bool RelayRegistrationsWorkflow::Lost(const std::string& origin, const int64_t now_ms) {
  std::lock_guard lock(mutex_);
  const auto state = std::find_if(states_.begin(), states_.end(),
                                  [&origin](const State& s) { return SameRelay(s.relay.origin, origin); });
  if (state == states_.end() || state->leaving || !state->registered) {
    return false;
  }
  state->registered = false;
  state->next_try_ms = state->loss.Lost(now_ms);
  log().warning << "registration on " << NormalizeRegistrationBackend(state->relay.origin) << " lost; registering again"
                << (state->loss.backoff_ms > 0 ? " after a backoff" : "");
  return true;
}

void RelayRegistrationsWorkflow::MakeDue(const std::string& origin) {
  std::lock_guard lock(mutex_);
  for (State& state : states_) {
    if (SameRelay(state.relay.origin, origin) && !state.leaving) {
      state.registered = false;
      state.next_try_ms = 0;
      state.backoff_ms = 0;
    }
  }
}

bool RelayRegistrationsWorkflow::Registered(const std::string& origin) const {
  std::lock_guard lock(mutex_);
  return std::any_of(states_.begin(), states_.end(),
                     [&origin](const State& s) { return s.registered && !s.leaving && s.relay.origin == origin; });
}

std::vector<std::string> RelayRegistrationsWorkflow::RegisteredOrigins() const {
  std::lock_guard lock(mutex_);
  std::vector<std::string> out;
  for (const State& state : states_) {
    if (state.registered && !state.leaving) {
      out.push_back(state.relay.origin);
    }
  }
  return out;
}

Roe<void> RelayRegistrationsWorkflow::Revoke(const std::string& origin) {
  std::shared_ptr<IRegistrationClient> client;
  std::string registration_id;
  {
    std::lock_guard lock(mutex_);
    const auto state =
        std::find_if(states_.begin(), states_.end(), [&origin](const State& s) { return s.relay.origin == origin; });
    if (state == states_.end()) {
      return Error("not one of our other relays");
    }
    client = state->relay.client;
    state->registered = false;
    state->leaving = true; // from now on: an answer in flight there is dropped
  }
  // After a run in progress: its registration there, if any, is the one this removes (read its id after it).
  const std::lock_guard after_run(run_mutex_);
  if (closed_.load(std::memory_order_acquire)) {
    return Error("closed");
  }
  {
    std::lock_guard lock(mutex_);
    const auto state =
        std::find_if(states_.begin(), states_.end(), [&origin](const State& s) { return s.relay.origin == origin; });
    if (state != states_.end()) {
      registration_id = state->registration_id;
    }
  }
  return RevokeRegistrationWithIdentity(*client, identity_, registration_id);
}

void RelayRegistrationsWorkflow::Rejoin(const std::string& origin) {
  std::lock_guard lock(mutex_);
  for (State& state : states_) {
    if (SameRelay(state.relay.origin, origin) && state.leaving) {
      state.leaving = false;
      state.registered = false;
      state.next_try_ms = 0;
      state.backoff_ms = 0;
    }
  }
}

void RelayRegistrationsWorkflow::RevokeAll() {
  {
    std::lock_guard lock(mutex_);
    for (State& state : states_) {
      state.registered = false;
      state.leaving = true;
    }
  }
  const std::lock_guard after_run(run_mutex_);
  if (closed_.load(std::memory_order_acquire)) {
    return;
  }
  std::vector<std::pair<Relay, std::string>> relays; // with the registration id each answered (after any run)
  {
    std::lock_guard lock(mutex_);
    for (const State& state : states_) {
      relays.emplace_back(state.relay, state.registration_id);
    }
  }
  for (const auto& [relay, registration_id] : relays) {
    if (auto revoked = RevokeRegistrationWithIdentity(*relay.client, identity_, registration_id); !revoked) {
      log().warning << "revoke on " << NormalizeRegistrationBackend(relay.origin)
                    << " failed: " << revoked.error().message;
    }
  }
}

void RelayRegistrationsWorkflow::Close() {
  closed_.store(true, std::memory_order_release);
  const std::lock_guard after_run(run_mutex_); // a run or a revoke in progress ends first
}

} // namespace pbr
