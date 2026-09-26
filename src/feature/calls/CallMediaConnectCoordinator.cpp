#include "feature/calls/CallMediaConnectCoordinator.h"

#include "foundation/runtime/AppRuntime.h"

#include <algorithm>
#include <chrono>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

constexpr int kConnectAttempts = 5;
/** Full OpenChannel + hello; 2.5 s was far too short on Android LAN. */
constexpr int kConnectAttemptTimeoutMs = 15000;
/** Drain abandoned stream callbacks before the next attempt. */
constexpr int kRetryDelayMs = 1500;
/** Inbound hello waits this long for the epoch key (offerer dials before the relay delivers it). */
constexpr int kInboundKeyWaitMs = 8000;
/** Re-ask for the key this often while waiting. */
constexpr int kInboundKeyPollMs = 250;

/** Run `fn` on UI: inline when already there, else posted. */
void OnUi(std::function<void()> fn) {
  if (AppRuntime::CurrentlyOnUI()) {
    fn();
    return;
  }
  AppRuntime::PostUI(std::move(fn));
}

} // namespace

void CallMediaConnectCoordinator::CheckUiThread(const char* what) const {
  // Sequence state is UI-thread only (no lock). A caller on another thread is a bug — make it
  // visible in dogfood logs rather than a silent race.
  if (!AppRuntime::CurrentlyOnUI()) {
    log().error << what << " called off the UI thread — sequence state is UI-only";
  }
}

CallMediaConnectCoordinator::CallMediaConnectCoordinator(ICallMediaTransport& transport,
                                                         PeerReachCoordinator& reach)
    : transport_(transport), reach_(reach), alive_(std::make_shared<std::atomic<bool>>(true)),
      inbound_key_wait_ms_(kInboundKeyWaitMs), attempt_timeout_ms_(kConnectAttemptTimeoutMs) {
  redirectLogger("CallMediaConnect");
}

CallMediaConnectCoordinator::~CallMediaConnectCoordinator() {
  alive_->store(false, std::memory_order_release);
  {
    std::lock_guard lock(inbound_mu_);
    shut_down_.store(true, std::memory_order_release);
  }
  inbound_cv_.notify_all();
  Abort();
  // Do not ClearInboundHandler here: a replacement owner (CallMediaPlane::BindBridge builds the
  // new bridge before destroying the old one) has already installed its own handler.
}

void CallMediaConnectCoordinator::SetAttemptTimeoutMsForTest(const int timeout_ms) {
  attempt_timeout_ms_ = timeout_ms > 0 ? timeout_ms : kConnectAttemptTimeoutMs;
}

bool CallMediaConnectCoordinator::Current(const uint64_t seq) const {
  return seq_.load(std::memory_order_acquire) == seq && !shut_down_.load(std::memory_order_acquire);
}

bool CallMediaConnectCoordinator::MediaReady() const {
  // A bundle merely in hello / AwaitingMedia (glare) is not a connection — only MediaReady is.
  return transport_.Phase() == CallMediaSessionPhase::MediaReady;
}

const char* CallMediaConnectCoordinator::Role() const {
  return request_.params.offerer ? "offerer" : "answerer";
}

void CallMediaConnectCoordinator::CancelTimers() {
  if (retry_timer_id_ != 0) {
    AppRuntime::CancelCoordinatorTimer(std::exchange(retry_timer_id_, 0));
  }
  if (watchdog_timer_id_ != 0) {
    AppRuntime::CancelCoordinatorTimer(std::exchange(watchdog_timer_id_, 0));
  }
}

void CallMediaConnectCoordinator::Abort() {
  CheckUiThread("Abort");
  seq_.fetch_add(1, std::memory_order_acq_rel);
  CancelTimers();
  // Completes the pending reach inline; its continuation sees the new sequence and returns.
  if (reach_id_ != 0) {
    reach_.Cancel(std::exchange(reach_id_, 0));
  }
  // The cancelled timers / reach were the completers for InFlight — clear it here.
  inflight_.store(false, std::memory_order_release);
}

void CallMediaConnectCoordinator::Shutdown() {
  {
    std::lock_guard lock(inbound_mu_);
    shut_down_.store(true, std::memory_order_release);
  }
  inbound_cv_.notify_all();
  Abort();
  // Drop the raw-`this` handler so late hellos cannot reach a destroyed owner.
  if (inbound_installed_) {
    transport_.ClearInboundHandler();
    inbound_installed_ = false;
  }
}

void CallMediaConnectCoordinator::SetInboundKeyWaitMsForTest(const int wait_ms) {
  inbound_key_wait_ms_.store(wait_ms > 0 ? wait_ms : kInboundKeyWaitMs, std::memory_order_release);
}

void CallMediaConnectCoordinator::SetInboundPorts(CallMediaInboundPorts ports) {
  inbound_ports_ = std::move(ports);
  inbound_installed_ = true;
  transport_.SetInboundHandler(
      [this, alive = alive_](CallMediaDirectConnectParams& params, CallMediaDirectCallbacks& cbs) {
        if (alive->load(std::memory_order_acquire)) {
          HandleInboundHello(params, cbs);
        }
      });
}

void CallMediaConnectCoordinator::NotifyKeyAvailable() {
  {
    std::lock_guard lock(inbound_mu_);
    ++key_notices_;
  }
  inbound_cv_.notify_all();
}

void CallMediaConnectCoordinator::HandleInboundHello(CallMediaDirectConnectParams& params,
                                                     CallMediaDirectCallbacks& cbs) {
  log().info << "Inbound hello call_id=" << params.call_id << " epoch=" << params.media_epoch
             << " peer=" << (params.peer_key.empty() ? "(empty)" : params.peer_key);
  const CallMediaInboundPorts& ports = inbound_ports_;
  if (!ports.session_open || !ports.session_open(params.call_id)) {
    log().warning << "Inbound rejected: no open session call_id=" << params.call_id;
    return;
  }
  if (!WaitForInboundKey(params)) {
    return;
  }
  if (params.media_key.empty()) {
    // Leaving the key empty makes the transport NACK the hello; the offerer retries.
    log().info << "Inbound rejected: media key not ready call_id=" << params.call_id;
    return;
  }
  CallMediaInboundHello hello;
  hello.call_id = params.call_id;
  hello.media_epoch = params.media_epoch;
  hello.peer_id = params.peer_key;
  if (ports.on_accepted) {
    cbs = ports.on_accepted(hello);
  }
}

bool CallMediaConnectCoordinator::WaitForInboundKey(CallMediaDirectConnectParams& params) {
  const CallMediaInboundPorts& ports = inbound_ports_;
  const auto key_ready = [&ports, &params]() -> bool {
    if (!ports.load_key) {
      return false;
    }
    if (auto key = ports.load_key(params.call_id, params.media_epoch)) {
      params.media_key = std::move(*key);
      return true;
    }
    return false;
  };
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(inbound_key_wait_ms_.load(std::memory_order_acquire));
  std::unique_lock lock(inbound_mu_);
  while (!shut_down_.load(std::memory_order_acquire)) {
    if (!ports.session_open(params.call_id)) {
      return false;  // ended while we waited
    }
    if (key_ready()) {
      return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return true;  // no key: caller NACKs
    }
    if (ports.request_key) {
      ports.request_key(params.call_id);
    }
    const auto slice = std::min(deadline, std::chrono::steady_clock::now() +
                                              std::chrono::milliseconds(kInboundKeyPollMs));
    const uint64_t seen = key_notices_;
    inbound_cv_.wait_until(lock, slice, [this, seen]() {
      return shut_down_.load(std::memory_order_acquire) || key_notices_ != seen;
    });
  }
  return false;
}

void CallMediaConnectCoordinator::Start(CallMediaConnectRequest request, CallMediaConnectHooks hooks) {
  CheckUiThread("Start");
  Abort();
  if (AppRuntime::IsShuttingDown() || shut_down_.load(std::memory_order_acquire)) {
    log().debug << "Start rejected: shutting down call_id=" << request.params.call_id;
    if (request.callbacks.on_failed) {
      request.callbacks.on_failed("shutdown in progress");
    }
    return;
  }
  request_ = std::move(request);
  hooks_ = std::move(hooks);
  fresh_link_next_ = false;
  attempt_reused_link_ = false;
  attempt_current_ = 0;
  inflight_.store(true, std::memory_order_release);
  // V049 / B31: simultaneous open — both roles dial immediately. Carrier / home NAT only pass
  // flows the inside host started. Amp A026 elects one PeerLink; the transport claims one stream.
  reach_.AbandonDial(request_.params.peer_key);
  log().info << "Connect start call_id=" << request_.params.call_id << " peer=" << request_.params.peer_key
             << " role=" << Role();
  BeginAttempt(seq_.load(std::memory_order_acquire), 1);
}

void CallMediaConnectCoordinator::BeginAttempt(const uint64_t seq, const int attempt) {
  if (!Current(seq)) {
    return;
  }
  if (MediaReady()) {
    Finish(seq, {});
    return;
  }
  PeerReachRequest reach = request_.reach;
  if (attempt > 1) {
    reach.exclude_direct = false;  // one escalation per sequence
  }
  reach.fresh_link = std::exchange(fresh_link_next_, false);
  attempt_reused_link_ = false;
  reach_id_ = reach_.Ensure(std::move(reach), [this, alive = alive_, seq, attempt](Roe<PeerReachResult> reached) {
    // UI — the Coordinator can lag Pause/Resume.
    OnUi([this, alive, seq, attempt, reached = std::move(reached)]() mutable {
      if (alive->load(std::memory_order_acquire) && Current(seq)) {
        reach_id_ = 0;
        OnLinkReady(seq, attempt, std::move(reached));
      }
    });
  });
}

void CallMediaConnectCoordinator::OnLinkReady(const uint64_t seq, const int attempt,
                                              Roe<PeerReachResult> reached) {
  if (!reached) {
    Finish(seq, Roe<void>(reached.error()));
    return;
  }
  attempt_reused_link_ = reached->reused_link;
  if (hooks_.on_link_ready) {
    hooks_.on_link_ready(reached->kind);
  }
  if (Current(seq)) {
    OpenBundle(seq, attempt);
  }
}

void CallMediaConnectCoordinator::OpenBundle(const uint64_t seq, const int attempt) {
  if (MediaReady()) {
    Finish(seq, {});
    return;
  }
  reach_.AbandonDial(request_.params.peer_key);
  if (hooks_.before_attempt) {
    hooks_.before_attempt(request_.params);
    if (!Current(seq)) {
      return;
    }
  }
  if (!transport_.IsActive()) {
    transport_.Detach();
  }
  attempt_current_ = attempt;
  log().info << "ConnectAsync attempt=" << attempt << "/" << kConnectAttempts
             << " call_id=" << request_.params.call_id << " peer=" << request_.params.peer_key
             << " role=" << Role() << " timeout_ms=" << attempt_timeout_ms_
             << " media_key_len=" << request_.params.media_key.size();
  // Armed before ConnectAsync: a synchronous completion must find (and cancel) it.
  ArmWatchdog(seq, attempt);
  transport_.ConnectAsync(
      request_.params, request_.callbacks,
      [this, alive = alive_, seq, attempt](Roe<void> connected) {
        // UI — not the Coordinator (Pause / backlog can drop the timeout → stuck Connecting).
        OnUi([this, alive, seq, attempt, connected = std::move(connected)]() mutable {
          if (!alive->load(std::memory_order_acquire)) {
            return;
          }
          log().info << "ConnectAsync done attempt=" << attempt << " ok=" << (connected ? 1 : 0)
                     << (connected ? "" : (" err=" + connected.error().message));
          OnAttemptFinished(seq, attempt, std::move(connected));
        });
      },
      attempt_timeout_ms_);
}

void CallMediaConnectCoordinator::ArmWatchdog(const uint64_t seq, const int attempt) {
  // CallMediaLeg TickDeadlines may not fire while OpenChannel / dial is stuck on a nested
  // MeshRuntime::Pump (dogfood 19f845 / 612b). B42: the watchdog fails only this attempt — it
  // goes through the normal failure path, so remaining attempts still run.
  if (watchdog_timer_id_ != 0) {
    AppRuntime::CancelCoordinatorTimer(watchdog_timer_id_);
  }
  watchdog_timer_id_ = AppRuntime::ScheduleCoordinatorOneShot(
      std::chrono::milliseconds(attempt_timeout_ms_ + 1000), [this, alive = alive_, seq, attempt]() {
        if (!alive->load(std::memory_order_acquire) || !Current(seq) || !InFlight()) {
          return;
        }
        AppRuntime::PostUI([this, alive, seq, attempt]() {
          if (!alive->load(std::memory_order_acquire) || !Current(seq) || !InFlight() ||
              attempt_current_ != attempt) {
            return;
          }
          log().warning << "ConnectAsync watchdog call_id=" << request_.params.call_id
                        << " peer=" << request_.params.peer_key << " attempt=" << attempt;
          watchdog_timer_id_ = 0;
          // Detach may not deliver on_finished if Mesh IO is wedged — force attempt progress.
          transport_.Detach();
          OnAttemptFinished(seq, attempt, Error("amp call-media connect timed out (watchdog)"));
        });
      });
}

void CallMediaConnectCoordinator::OnAttemptFinished(const uint64_t seq, const int attempt,
                                                    Roe<void> connected) {
  if (!Current(seq)) {
    // Superseded sequence: drop its bundle (Abort / restart already settled InFlight) — unless
    // a newer sequence is connecting the same call and may adopt it.
    if (!InFlight() || transport_.ActiveParams().call_id != request_.params.call_id) {
      transport_.Detach();
    }
    return;
  }
  // B42: a late completion of an attempt the watchdog already moved past must not re-finish.
  if (attempt != attempt_current_) {
    return;
  }
  if (watchdog_timer_id_ != 0) {
    AppRuntime::CancelCoordinatorTimer(std::exchange(watchdog_timer_id_, 0));
  }
  if (connected || MediaReady()) {
    log().info << "Connect ok call_id=" << request_.params.call_id << " role=" << Role();
    Finish(seq, {});
    return;
  }
  log().warning << "Connect failed attempt=" << attempt << " err=" << connected.error().message;
  reach_.AbandonDial(request_.params.peer_key);
  // B39: the attempt ran on an already-"connected" link, or failed like a dead link — the next
  // reach drops it and redials fresh DialBook candidates instead of reusing the stale PeerLink.
  if (attempt_reused_link_ || connected.error().message.find("transport failed") != std::string::npos) {
    fresh_link_next_ = true;
    log().info << "next attempt needs a fresh link peer=" << request_.params.peer_key << " attempt=" << attempt;
  }
  if (attempt >= kConnectAttempts) {
    Finish(seq, std::move(connected));
    return;
  }
  retry_timer_id_ = AppRuntime::ScheduleCoordinatorOneShot(
      std::chrono::milliseconds(kRetryDelayMs), [this, alive = alive_, seq, attempt]() {
        AppRuntime::PostUI([this, alive, seq, attempt]() {
          if (!alive->load(std::memory_order_acquire) || !Current(seq)) {
            return;
          }
          retry_timer_id_ = 0;
          BeginAttempt(seq, attempt + 1);
        });
      });
}

void CallMediaConnectCoordinator::Finish(const uint64_t seq, Roe<void> result) {
  if (!Current(seq) || !InFlight()) {
    return;  // superseded, or already finished (duplicate completion)
  }
  inflight_.store(false, std::memory_order_release);
  CancelTimers();
  if (!result) {
    log().info << "Connect give up call_id=" << request_.params.call_id << " role=" << Role()
               << " err=" << result.error().message;
  }
  // Posted, not inline: the handler may restart or stop the call. Skipped if Abort / Start ran
  // since — a stale give-up must not tear down a newer session.
  AppRuntime::PostUI([this, alive = alive_, seq, done = hooks_.on_finished, result = std::move(result)]() mutable {
    if (!alive->load(std::memory_order_acquire) || !Current(seq) || !done) {
      return;
    }
    done(std::move(result));
  });
}

} // namespace pbr
