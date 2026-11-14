#pragma once

#include "lib/net/OrgBackendClients.h"
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
 * relay answered is kept in memory only: a start registers on each again. The preferred relay's registration stays
 * the identity's (RegistrationClient.h); nothing here writes the identity. Thread-safe; RunDue blocks (HTTP).
 */
class RelayRegistrationsWorkflow : public Module {
public:
  static constexpr int64_t kFirstRetryMs = 60 * 1000;
  static constexpr int64_t kMaxRetryMs = 30 * 60 * 1000;

  struct Relay {
    std::string origin;
    std::shared_ptr<IRegistrationClient> client;
  };

  explicit RelayRegistrationsWorkflow(IdentityStore& identity);

  /** Our relays other than the preferred one. A relay still listed keeps what it answered. */
  void SetRelays(std::vector<Relay> relays);
  /** What register/finish carries: our account card (WithAccountCard). */
  void SetCard(std::function<RegistrationPublishOpts()> card) { card_ = std::move(card); }
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
  /** Removes our registration from each relay (profile reset, #328); best effort, as on the preferred. */
  void RevokeAll();

private:
  struct State {
    Relay relay;
    bool registered = false;
    std::string expires_at;
    int64_t card_issued_at = 0;
    int64_t next_try_ms = 0;
    int64_t backoff_ms = 0;
  };

  static bool DueLocked(const State& state, int64_t now_ms, int64_t card_issued_at);
  void Register(const Relay& relay, const std::string& nickname, const RegistrationPublishOpts& publish,
                int64_t now_ms);

  IdentityStore& identity_;
  std::function<RegistrationPublishOpts()> card_;
  std::function<void(const std::string&)> on_registered_;
  mutable std::mutex mutex_;
  std::vector<State> states_;
  std::atomic<bool> running_{false};
};

} // namespace pbr
