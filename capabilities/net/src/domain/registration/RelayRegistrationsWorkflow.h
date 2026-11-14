#pragma once

#include "lib/net/OrgBackendClients.h"
#include "lib/net/OurRelaysStore.h"
#include "lib/people/IdentityStore.h"
#include "common/Module.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Our account on our other relays (multi-relay R2; docs/contracts/SERVICE_ENDPOINTS.md § Several relays): once the
 * account is registered on its preferred relay (the person's act, kept in the identity), it registers on each other
 * relay of ours with the same card — and again when the card changes, when that registration nears its expiry (14
 * days, as the preferred one), or after a failure (a backoff from kFirstRetryMs doubling to kMaxRetryMs). What a
 * relay answered is reported (SetOnResult: the owner keeps it, OurRelaysStore) and given back at the next start
 * (Relay::last): a relay that holds a registration for our current card is not asked again. The preferred relay's
 * registration stays
 * the identity's (RegistrationClient.h); the identity takes only the Brief LLM key the org's relay answers with, when
 * that relay is another of ours (MR006). Thread-safe; RunDue blocks (HTTP).
 */
class RelayRegistrationsWorkflow : public Module {
public:
  static constexpr int64_t kFirstRetryMs = 60 * 1000;
  static constexpr int64_t kMaxRetryMs = 30 * 60 * 1000;

  struct Relay {
    std::string origin;
    std::shared_ptr<IRegistrationClient> client;
    /** What it answered last (kept across restarts); a relay already known here keeps what it answered since. */
    RelayRegistrationRecord last;
  };

  explicit RelayRegistrationsWorkflow(IdentityStore& identity);

  /** Our relays other than the preferred one. A relay still listed keeps what it answered. */
  void SetRelays(std::vector<Relay> relays);
  /** What register/finish carries: our account card (WithAccountCard). */
  void SetCard(std::function<RegistrationPublishOpts()> card) { card_ = std::move(card); }
  /** Every answer (registered or failed), to keep. Called on RunDue's thread. */
  void SetOnResult(std::function<void(const std::string& origin, const RelayRegistrationRecord& record)> on_result) {
    on_result_ = std::move(on_result);
  }
  /** A relay took our registration (its push binding may go now). Called on RunDue's thread. */
  void SetOnRegistered(std::function<void(const std::string& origin)> on_registered) {
    on_registered_ = std::move(on_registered);
  }

  /** The relays due a registration at `now_ms` (none while the account has no registration of its own). */
  std::vector<std::string> Due(int64_t now_ms);
  /** Registers on every relay due (one run at a time: a second call meanwhile does nothing). */
  void RunDue(int64_t now_ms);
  /** That relay holds our registration (this run). */
  bool Registered(const std::string& origin) const;
  /** Our relays (other than the preferred) that hold our registration. */
  std::vector<std::string> RegisteredOrigins() const;
  /** Removes our registration from each relay (profile reset, #328); best effort, as on the preferred. As Revoke. */
  void RevokeAll();
  /**
   * Removes our registration from that relay of ours (it leaves the list; with it its push bindings). Blocks. The
   * relay is leaving from the call on: not due again, not Registered, and what a registration in flight there answers
   * is dropped (not reported, its push not bound); the revoke waits for a RunDue in progress, so it comes after.
   */
  Roe<void> Revoke(const std::string& origin);
  /**
   * The profile closes: from now on nothing is due, RunDue and the revokes do nothing (and touch neither the identity
   * nor the hooks), and once this returns no run or revoke is in progress (it waits for one).
   */
  void Close();

private:
  struct State {
    Relay relay;
    bool registered = false;
    std::string expires_at;
    int64_t card_issued_at = 0;
    int64_t next_try_ms = 0;
    int64_t backoff_ms = 0;
    /** Revoked (it leaves our list): nothing more is asked or kept there. */
    bool leaving = false;
  };

  static bool DueLocked(const State& state, int64_t now_ms, int64_t card_issued_at);
  void Register(const Relay& relay, const std::string& nickname, const RegistrationPublishOpts& publish,
                int64_t now_ms);
  /** The org's relay answered with the Brief LLM key: the identity keeps it. */
  void KeepBriefLlmKey(const RegistrationResult& result);

  IdentityStore& identity_;
  std::function<RegistrationPublishOpts()> card_;
  std::function<void(const std::string&)> on_registered_;
  std::function<void(const std::string&, const RelayRegistrationRecord&)> on_result_;
  mutable std::mutex mutex_;
  std::vector<State> states_;
  /** Held by a RunDue for its whole run (a second one skips); a revoke and Close wait for it. */
  std::mutex run_mutex_;
  std::atomic<bool> closed_{false};
};

} // namespace pbr
