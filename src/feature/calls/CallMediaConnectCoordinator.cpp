#include "feature/calls/CallMediaConnectCoordinator.h"
#include "feature/calls/CallsThread.h"

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
/** Re-ask for the key this often while a hello is parked. */
constexpr int kInboundKeyPollMs = 250;

/** Run `fn` on the calls owner: inline when already there, else posted. */
void OnOwner(std::function<void()> fn) {
  if (CallsThread::IsCurrent()) {
    fn();
    return;
  }
  CallsThread::Post(std::move(fn));
}

} // namespace

void CallMediaConnectCoordinator::CheckUiThread(const char* what) const {
  // Sequence state is calls-owner only (no lock). A caller on another thread is a bug — make it
  // visible in dogfood logs rather than a silent race.
  if (!CallsThread::IsCurrent()) {
    log().error << what << " called off the calls owner — sequence state is owner-only";
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
  shut_down_.store(true, std::memory_order_release);
  RejectPendingHellos("coordinator gone");
  Abort();
  // Do not ClearInboundHandler here: a replacement path may already have installed its own. Ours
  // stays harmless until then — the alive flag makes a late hello a NACK (the offerer retries).
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
  shut_down_.store(true, std::memory_order_release);
  RejectPendingHellos("shutting down");
  Abort();
  // Drop the raw-`this` handler so late hellos cannot reach a destroyed owner.
  if (inbound_installed_) {
    transport_.ClearInboundHandler();
    inbound_installed_ = false;
  }
}

void CallMediaConnectCoordinator::SetInboundKeyWaitMsForTest(const int wait_ms) {
  inbound_key_wait_ms_ = wait_ms > 0 ? wait_ms : kInboundKeyWaitMs;
}

void CallMediaConnectCoordinator::SetInboundPorts(CallMediaInboundPorts ports) {
  inbound_ports_ = std::move(ports);
  inbound_installed_ = true;
  // Called on the transport's IO hop: hand the hello to the calls owner and return.
  transport_.SetInboundHandler([this, alive = alive_](CallMediaDirectConnectParams params,
                                                      CallMediaInboundAnswer answer) {
    CallsThread::Post([this, alive, params = std::move(params), answer = std::move(answer)]() mutable {
      if (!alive->load(std::memory_order_acquire)) {
        answer(std::move(params), {});  // owner gone: NACK, the offerer retries
        return;
      }
      HandleInboundHello(std::move(params), std::move(answer));
    });
  });
}

void CallMediaConnectCoordinator::NotifyKeyAvailable() {
  OnOwner([this, alive = alive_]() {
    if (alive->load(std::memory_order_acquire)) {
      RecheckPendingHellos(/*ask_again=*/false);
    }
  });
}

void CallMediaConnectCoordinator::HandleInboundHello(CallMediaDirectConnectParams params,
                                                     CallMediaInboundAnswer answer) {
  PBR_ASSERT_ON_OWNER(OwnerThreadId::MediaSessions);
  log().info << "Inbound hello call_id=" << params.call_id << " epoch=" << params.media_epoch
             << " peer=" << (params.peer_key.empty() ? "(empty)" : params.peer_key);
  const CallMediaInboundPorts& ports = inbound_ports_;
  if (shut_down_.load(std::memory_order_acquire) || !ports.session_open || !ports.session_open(params.call_id)) {
    log().warning << "Inbound rejected: no open session call_id=" << params.call_id;
    answer(std::move(params), {});
    return;
  }
  if (TryLoadInboundKey(params)) {
    AcceptInboundHello(std::move(params), answer);
    return;
  }
  if (ports.request_key) {
    ports.request_key(params.call_id);
  }
  pending_hellos_.push_back(PendingHello{std::move(params), std::move(answer),
                                         std::chrono::steady_clock::now() +
                                             std::chrono::milliseconds(inbound_key_wait_ms_)});
  ArmKeyPoll();
}

bool CallMediaConnectCoordinator::TryLoadInboundKey(CallMediaDirectConnectParams& params) const {
  if (!inbound_ports_.load_key) {
    return false;
  }
  if (auto key = inbound_ports_.load_key(params.call_id, params.media_epoch)) {
    params.media_key = std::move(*key);
    return true;
  }
  return false;
}

void CallMediaConnectCoordinator::AcceptInboundHello(CallMediaDirectConnectParams params,
                                                     const CallMediaInboundAnswer& answer) {
  CallMediaDirectCallbacks cbs;
  if (inbound_ports_.on_accepted) {
    CallMediaInboundHello hello;
    hello.call_id = params.call_id;
    hello.media_epoch = params.media_epoch;
    hello.peer_id = params.peer_key;
    cbs = inbound_ports_.on_accepted(hello);
  }
  answer(std::move(params), std::move(cbs));
}

void CallMediaConnectCoordinator::RecheckPendingHellos(const bool ask_again) {
  PBR_ASSERT_ON_OWNER(OwnerThreadId::MediaSessions);
  const auto now = std::chrono::steady_clock::now();
  std::vector<PendingHello> still;
  auto parked = std::move(pending_hellos_);
  pending_hellos_.clear();
  for (auto& hello : parked) {
    const CallMediaInboundPorts& ports = inbound_ports_;
    if (!ports.session_open || !ports.session_open(hello.params.call_id)) {
      hello.answer(std::move(hello.params), {});  // ended while parked
      continue;
    }
    if (TryLoadInboundKey(hello.params)) {
      AcceptInboundHello(std::move(hello.params), hello.answer);
      continue;
    }
    if (now >= hello.deadline) {
      // No key in time: the transport NACKs the hello; the offerer retries.
      log().info << "Inbound rejected: media key not ready call_id=" << hello.params.call_id;
      hello.answer(std::move(hello.params), {});
      continue;
    }
    if (ask_again && ports.request_key) {
      ports.request_key(hello.params.call_id);
    }
    still.push_back(std::move(hello));
  }
  // Appended, not assigned: an answer runs inline and could, in principle, park another hello.
  for (auto& hello : still) {
    pending_hellos_.push_back(std::move(hello));
  }
}

void CallMediaConnectCoordinator::ArmKeyPoll() {
  if (key_poll_timer_id_ != 0 || pending_hellos_.empty()) {
    return;
  }
  key_poll_timer_id_ = AppRuntime::ScheduleCoordinatorOneShot(
      std::chrono::milliseconds(kInboundKeyPollMs), [this, alive = alive_]() {
        CallsThread::Post([this, alive]() {
          if (!alive->load(std::memory_order_acquire)) {
            return;
          }
          key_poll_timer_id_ = 0;
          RecheckPendingHellos(/*ask_again=*/true);
          ArmKeyPoll();
        });
      });
}

void CallMediaConnectCoordinator::RejectPendingHellos(const char* why) {
  if (key_poll_timer_id_ != 0) {
    AppRuntime::CancelCoordinatorTimer(std::exchange(key_poll_timer_id_, 0));
  }
  auto parked = std::move(pending_hellos_);
  pending_hellos_.clear();
  for (auto& hello : parked) {
    log().info << "Inbound rejected (" << why << ") call_id=" << hello.params.call_id;
    hello.answer(std::move(hello.params), {});
  }
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
    // Calls owner — the Coordinator can lag Pause/Resume.
    OnOwner([this, alive, seq, attempt, reached = std::move(reached)]() mutable {
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
        // Calls owner — not the Coordinator (Pause / backlog can drop the timeout → stuck Connecting).
        OnOwner([this, alive, seq, attempt, connected = std::move(connected)]() mutable {
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
        CallsThread::Post([this, alive, seq, attempt]() {
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
        CallsThread::Post([this, alive, seq, attempt]() {
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
  CallsThread::Post([this, alive = alive_, seq, done = hooks_.on_finished, result = std::move(result)]() mutable {
    if (!alive->load(std::memory_order_acquire) || !Current(seq) || !done) {
      return;
    }
    done(std::move(result));
  });
}

} // namespace pbr
