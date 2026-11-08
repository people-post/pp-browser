#pragma once

#include "common/Error.h"
#include "common/Secret.h"
#include "lib/net/OrgBackendClients.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * At most one run of a task at a time. A trigger while one runs asks for exactly one more after it (`again`); every
 * end of a run (done, failed, threw, discarded) calls Finish with its ticket, which clears both flags and says
 * whether that one more run is due. Reset drops a run that will never end (its work was dropped by a teardown): its
 * late Finish, if any, changes nothing. Thread-safe: the Brief key's run is shared with the 401 renewal, which runs
 * on a turn's thread (B183).
 */
class CoalescedRun {
public:
  using Ticket = uint64_t;
  /**
   * A ticket: start the run now. nullopt: one runs; with `queue_again`, one more run is due when it ends (a waiter
   * that only wants the running one's result passes false).
   */
  std::optional<Ticket> TryStart(bool queue_again = true);
  /** The run of `ticket` ended. True: run once more now (asked for meanwhile). False too for a stale ticket. */
  bool Finish(Ticket ticket);
  /** Forgets the running one (its Finish is then stale) and any `again`. */
  void Reset();
  bool InFlight() const;
  /** Blocks until no run is in flight, at most `timeout`. True: none in flight. Never on the UI thread. */
  bool WaitIdle(std::chrono::milliseconds timeout) const;

private:
  mutable std::mutex mutex_;
  mutable std::condition_variable idle_;
  bool in_flight_ = false;
  bool again_ = false;
  Ticket ticket_ = 0;
};

/** What the registration renewal's network half brought back. */
struct RenewResult {
  /** The finished registration to store; nullopt when nothing was due (or it failed). */
  std::optional<RegistrationResult> registration;
  /** The network half failed, threw, or storing its result failed. */
  std::optional<Error> error;
};

enum class BriefKeyMintOutcome {
  Stored,     // the minted key is in the identity (or a key came meanwhile and was kept)
  MintFailed, // `error` set
  SaveFailed, // `error` set
  Discarded,  // the session changed or messaging is no longer ready: nothing written
};

struct BriefKeyMintEnd {
  BriefKeyMintOutcome outcome = BriefKeyMintOutcome::Discarded;
  std::optional<Error> error;
};

/**
 * The network work of messaging-ready (push device binding, registration renewal, the Brief guest key) off the UI
 * thread (DEV/PLAN-unlock-gate-early-dismiss.md A). Orchestration only; everything it does is a port, so tests drive
 * it without a ChatController.
 *
 * The network half of each runs in `run_worker_then_ui`'s work; its result comes back to the UI thread. There, and
 * only when the session the task started in is still the current one and messaging is still ready, the result is
 * stored: the persist ports read the identity again and change only their own fields. A stale result is dropped
 * and nothing is written. Each task is a CoalescedRun.
 *
 * Every method and every port except the three network ones: UI thread.
 */
class MessagingReadyTasks {
public:
  struct Ports {
    /** Runs `work` on a worker, then `on_ui` on the UI thread (Application: PostWorkerAndReplyOnUI). */
    std::function<void(std::function<void()> work, std::function<void()> on_ui)> run_worker_then_ui;
    /** The current messaging session (ChatController's counter, one up per WireMessagingBindings). */
    std::function<uint64_t()> session;
    /** `started` is still the current session and messaging is ready (ChatController::MessagingReady). */
    std::function<bool(uint64_t started)> still_ready;

    /** Worker: binds this device's push tokens (PushDeviceCoordinator::Register, APNs + VoIP). */
    std::function<void()> register_push;

    /** UI, read before the renewal is sent off: the "renew automatically" preference. */
    std::function<bool()> auto_renew;
    /** Worker: the renewal's network half only (nothing stored). */
    std::function<RenewResult(bool auto_renew)> renew_network;
    /** UI: reads the identity again, applies only the registration fields, saves. */
    std::function<Roe<void>(const RegistrationResult& registration)> renew_persist;
    /** UI: the "registration expires soon" hint and the log line (`auto_renew` as read before the dispatch). */
    std::function<void(const RenewResult& result, bool auto_renew)> renew_hint;

    /** UI: no Brief key is stored (registered or guest): a mint is worth it. */
    std::function<bool()> brief_key_needed;
    /** Worker: mints a guest key at `llm_base_url` (HTTP only). */
    std::function<Roe<SecretText>(const std::string& llm_base_url)> mint_brief_key_network;
    /** UI: reads the identity again and writes only the guest key field (keeps a key that came meanwhile). */
    std::function<Roe<void>(const SecretText& key)> brief_key_persist;
    /** UI: how the mint ended (the hint, the banner), after its run is finished. */
    std::function<void(const BriefKeyMintEnd& end)> brief_key_done;

    /** UI: the identity changed (renewal or key stored): ReloadAgentConfig + RefreshLlmSetupBanner. */
    std::function<void()> after_identity_changed;
  };

  /** `brief_key_run`: the Brief key's run, owned by AgentTurnController and shared with the 401 renewal. */
  MessagingReadyTasks(Ports ports, std::shared_ptr<CoalescedRun> brief_key_run);
  ~MessagingReadyTasks();
  MessagingReadyTasks(const MessagingReadyTasks&) = delete;
  MessagingReadyTasks& operator=(const MessagingReadyTasks&) = delete;

  void SyncPushDevices();
  void RenewRegistration();
  void MintBriefKey(const std::string& llm_base_url);
  /** The last MintBriefKey asked again (a 401 renewal held the run meanwhile and the key may still be missing). */
  void RetryBriefKeyMint();
  /** A new messaging session began: a run of the old one that never ended no longer holds its slot. */
  void OnNewSession();

  bool PushInFlight() const;
  bool RenewInFlight() const;

  struct State;

private:
  std::shared_ptr<State> state_;
};

/**
 * The 401 renewal's turn on the shared Brief key run (B183). While the run is in flight (the unlock's mint, or another
 * turn's renewal) it waits, at most `wait`: that one stores its key and `renew` then finds it and reuses it, so no
 * second key is minted. Then `renew` runs holding the run. Still in flight when the wait is over: `renew` runs anyway,
 * as before the run was shared. `on_again`: a mint was asked for while this one held the run; called on this thread.
 * Blocks: never on the UI thread (pass a zero `wait` there).
 */
template <typename Renew>
auto RunSharedBriefKeyRenewal(CoalescedRun& run, const std::chrono::milliseconds wait, Renew&& renew,
                              const std::function<void()>& on_again = {}) -> decltype(renew()) {
  std::optional<CoalescedRun::Ticket> ticket = run.TryStart(/*queue_again=*/false);
  const auto deadline = std::chrono::steady_clock::now() + wait;
  while (!ticket) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0) {
      break;
    }
    (void)run.WaitIdle(left);
    ticket = run.TryStart(/*queue_again=*/false);
  }
  struct End {
    CoalescedRun& run;
    std::optional<CoalescedRun::Ticket> ticket;
    const std::function<void()>& on_again;
    ~End() {
      if (ticket && run.Finish(*ticket) && on_again) {
        try {
          on_again();
        } catch (...) {
        }
      }
    }
  } end{run, ticket, on_again};
  return renew();
}

} // namespace pbr
