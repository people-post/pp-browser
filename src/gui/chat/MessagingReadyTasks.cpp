#include "gui/chat/MessagingReadyTasks.h"

#include "common/Logger.h"

#include <exception>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

logging::Logger& log() {
  static logging::Logger logger = logging::getLogger("MessagingReadyTasks");
  return logger;
}

} // namespace

// --- CoalescedRun --------------------------------------------------------------------------------

std::optional<CoalescedRun::Ticket> CoalescedRun::TryStart(const bool queue_again) {
  std::lock_guard lock(mutex_);
  if (in_flight_) {
    if (queue_again) {
      again_ = true;
    }
    return std::nullopt;
  }
  in_flight_ = true;
  again_ = false;
  return ++ticket_;
}

bool CoalescedRun::Finish(const Ticket ticket) {
  bool again = false;
  {
    std::lock_guard lock(mutex_);
    if (!in_flight_ || ticket != ticket_) {
      return false; // a run Reset dropped: the slot is someone else's now
    }
    in_flight_ = false;
    again = again_;
    again_ = false;
  }
  idle_.notify_all();
  return again;
}

void CoalescedRun::Reset() {
  {
    std::lock_guard lock(mutex_);
    in_flight_ = false;
    again_ = false;
    ++ticket_;
  }
  idle_.notify_all();
}

bool CoalescedRun::InFlight() const {
  std::lock_guard lock(mutex_);
  return in_flight_;
}

bool CoalescedRun::WaitIdle(const std::chrono::milliseconds timeout) const {
  std::unique_lock lock(mutex_);
  return idle_.wait_for(lock, timeout, [this] { return !in_flight_; });
}

// --- MessagingReadyTasks ---------------------------------------------------------------------------

struct MessagingReadyTasks::State {
  Ports ports;
  /** The owner is gone: a result that comes back finishes its run and does nothing else. */
  bool closed = false;
  std::shared_ptr<CoalescedRun> push_run = std::make_shared<CoalescedRun>();
  std::shared_ptr<CoalescedRun> renew_run = std::make_shared<CoalescedRun>();
  std::shared_ptr<CoalescedRun> brief_key_run;
  std::string brief_key_base_url; // the last MintBriefKey's, for a run asked again
};

namespace {

using State = MessagingReadyTasks::State;

/** What the work part left for the UI part. */
template <typename Result>
struct Box {
  std::optional<Result> value;
  bool threw = false;
};

/**
 * Runs `work` off the UI thread and `on_ui` back on it, under `run`'s `ticket`. `on_ui(box, live)` gets
 * `live` = the session the task started in is still current and messaging is still ready; it returns what is to
 * happen once the run is finished (notifications that may start other runs). The run is finished on every way out:
 * done, failed, threw, stale, owner gone, no runner. A run asked for meanwhile then starts through `again`.
 */
template <typename Result>
void RunOffUi(const std::shared_ptr<State>& state, const std::shared_ptr<CoalescedRun>& run,
              const CoalescedRun::Ticket ticket, std::function<Result()> work,
              std::function<std::function<void()>(Box<Result>& box, bool live)> on_ui,
              void (*again)(const std::shared_ptr<State>&)) {
  const uint64_t session = state->ports.session ? state->ports.session() : 0;
  auto box = std::make_shared<Box<Result>>();
  auto worker_part = [box, work = std::move(work)]() {
    try {
      box->value.emplace(work());
    } catch (const std::exception& e) {
      log().warning << "messaging-ready task threw: " << e.what();
      box->threw = true;
    } catch (...) {
      log().warning << "messaging-ready task threw";
      box->threw = true;
    }
  };
  auto ui_part = [state, run, ticket, session, box, on_ui = std::move(on_ui), again]() {
    bool run_again = false;
    std::function<void()> then;
    {
      struct End {
        CoalescedRun& run;
        CoalescedRun::Ticket ticket;
        bool& again;
        ~End() { again = run.Finish(ticket); }
      } end{*run, ticket, run_again};
      if (!state->closed) {
        const bool live = state->ports.still_ready && state->ports.still_ready(session);
        then = on_ui(*box, live);
      }
    }
    if (state->closed) {
      return;
    }
    if (then) {
      then();
    }
    if (run_again) {
      again(state);
    }
  };
  if (!state->ports.run_worker_then_ui) {
    (void)run->Finish(ticket);
    return;
  }
  try {
    state->ports.run_worker_then_ui(std::move(worker_part), std::move(ui_part));
  } catch (...) {
    log().warning << "messaging-ready task not sent off";
    (void)run->Finish(ticket);
  }
}

void SyncPushDevicesOn(const std::shared_ptr<State>& state) {
  if (state->closed || !state->ports.register_push) {
    return;
  }
  const std::optional<CoalescedRun::Ticket> ticket = state->push_run->TryStart();
  if (!ticket) {
    return; // one more run when this one ends
  }
  RunOffUi<bool>(
      state, state->push_run, *ticket,
      [register_push = state->ports.register_push]() {
        register_push();
        return true;
      },
      // Nothing to store: a binding that failed was only ever logged (PushDeviceCoordinator), and is tried again
      // at the next ready or token change.
      [](Box<bool>&, bool) -> std::function<void()> { return {}; }, &SyncPushDevicesOn);
}

void RenewRegistrationOn(const std::shared_ptr<State>& state) {
  if (state->closed || !state->ports.renew_network) {
    return;
  }
  const std::optional<CoalescedRun::Ticket> ticket = state->renew_run->TryStart();
  if (!ticket) {
    return;
  }
  // Read here, on the UI thread: the worker reads no preference.
  const bool auto_renew = state->ports.auto_renew ? state->ports.auto_renew() : false;
  RunOffUi<RenewResult>(
      state, state->renew_run, *ticket,
      [renew_network = state->ports.renew_network, auto_renew]() { return renew_network(auto_renew); },
      [state, auto_renew](Box<RenewResult>& box, const bool live) -> std::function<void()> {
        if (!live) {
          log().info << "registration renewal result dropped: the messaging session changed";
          return {};
        }
        RenewResult result = box.value ? std::move(*box.value) : RenewResult{.error = Error("renewal threw")};
        bool stored = false;
        if (result.registration && !result.error) {
          Roe<void> saved = state->ports.renew_persist
                                ? state->ports.renew_persist(*result.registration, result.account_id)
                                : Roe<void>(Error("renewal store not wired"));
          if (saved) {
            stored = true;
          } else {
            result.error = saved.error();
          }
        }
        return [state, result = std::move(result), auto_renew, stored]() {
          if (stored && state->ports.after_identity_changed) {
            state->ports.after_identity_changed();
          }
          if (state->ports.renew_hint) {
            state->ports.renew_hint(result, auto_renew);
          }
        };
      },
      &RenewRegistrationOn);
}

void MintBriefKeyOn(const std::shared_ptr<State>& state) {
  if (state->closed || !state->ports.mint_brief_key_network || !state->brief_key_run) {
    return;
  }
  // A run asked again finds the key a renewal stored meanwhile: no second mint.
  if (state->ports.brief_key_needed && !state->ports.brief_key_needed()) {
    return;
  }
  // The account the key is minted for, read now: the key is stored only into that one.
  std::string account_id = state->ports.account_id ? state->ports.account_id() : std::string();
  if (account_id.empty()) {
    return;
  }
  const std::optional<CoalescedRun::Ticket> ticket = state->brief_key_run->TryStart();
  if (!ticket) {
    return;
  }
  RunOffUi<Roe<SecretText>>(
      state, state->brief_key_run, *ticket,
      [mint = state->ports.mint_brief_key_network, base_url = state->brief_key_base_url]() { return mint(base_url); },
      [state, account_id = std::move(account_id)](Box<Roe<SecretText>>& box, const bool live) -> std::function<void()> {
        BriefKeyMintEnd end;
        if (!live) {
          log().info << "Brief guest key dropped: the messaging session changed";
          end.outcome = BriefKeyMintOutcome::Discarded;
        } else if (!box.value) {
          end = {.outcome = BriefKeyMintOutcome::MintFailed, .error = Error("Brief guest key mint threw")};
        } else if (!*box.value) {
          end = {.outcome = BriefKeyMintOutcome::MintFailed, .error = box.value->error()};
        } else {
          Roe<void> saved = state->ports.brief_key_persist ? state->ports.brief_key_persist(**box.value, account_id)
                                                           : Roe<void>(Error("Brief key store not wired"));
          end = saved ? BriefKeyMintEnd{.outcome = BriefKeyMintOutcome::Stored}
                      : BriefKeyMintEnd{.outcome = BriefKeyMintOutcome::SaveFailed, .error = saved.error()};
        }
        // After the run is finished: the banner the end refreshes must not still see a mint in flight.
        return [state, end = std::move(end)]() {
          if (state->ports.brief_key_done) {
            state->ports.brief_key_done(end);
          }
          if (end.outcome == BriefKeyMintOutcome::Stored && state->ports.after_identity_changed) {
            state->ports.after_identity_changed();
          }
        };
      },
      &MintBriefKeyOn);
}

} // namespace

MessagingReadyTasks::MessagingReadyTasks(Ports ports, std::shared_ptr<CoalescedRun> brief_key_run)
    : state_(std::make_shared<State>()) {
  state_->ports = std::move(ports);
  state_->brief_key_run = brief_key_run ? std::move(brief_key_run) : std::make_shared<CoalescedRun>();
}

MessagingReadyTasks::~MessagingReadyTasks() {
  // A result still on its way finishes its run and touches nothing else: the ports point into the owner.
  state_->closed = true;
  state_->ports = {};
}

void MessagingReadyTasks::SyncPushDevices() {
  SyncPushDevicesOn(state_);
}

void MessagingReadyTasks::RenewRegistration() {
  RenewRegistrationOn(state_);
}

void MessagingReadyTasks::MintBriefKey(const std::string& llm_base_url) {
  state_->brief_key_base_url = llm_base_url;
  MintBriefKeyOn(state_);
}

void MessagingReadyTasks::RetryBriefKeyMint() {
  if (!state_->brief_key_base_url.empty()) {
    MintBriefKeyOn(state_);
  }
}

void MessagingReadyTasks::OnMessagingTornDown() {
  state_->push_run->Reset();
  state_->renew_run->Reset();
  state_->brief_key_run->Reset();
}

bool MessagingReadyTasks::PushInFlight() const {
  return state_->push_run->InFlight();
}

bool MessagingReadyTasks::RenewInFlight() const {
  return state_->renew_run->InFlight();
}

} // namespace pbr
