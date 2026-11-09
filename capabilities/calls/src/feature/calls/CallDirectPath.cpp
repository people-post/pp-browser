#include "feature/calls/CallDirectPath.h"

#include "lib/calls/CallDirectPathLogic.h"
#include "mesh/l4/shared/MediaFrame.h"
#include "mesh/types/MediaChannel.h"
#include "lib/calls/CallTxOnlyEscalateLogic.h"
#include "lib/calls/SeedParkOutcome.h"

#include "common/Localized.h"
#include "lib/calls/CallHopAttachLogic.h"
#include "domain/call_media/CallMediaFrameCrypto.h"
#include "common/runtime/AppRuntime.h"
#include "common/media/MediaSessionsOwner.h"
#include "common/Utilities.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <chrono>
#include <thread>
#include <utility>
#include <type_traits>
#include <variant>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {
namespace {

/** B44: how long a failed attempt waits for the peer's in-progress inbound hello to finish. */
constexpr std::chrono::milliseconds kInboundRecoveryGrace{3000};

/** Answerer waits for offerer dial; offerer retries can take ~60s — keep chrome aligned. */
constexpr int64_t kMeshConnectTimeoutMs = 75000;
/** Cover long PollInbox HTTP + offerer MediaKey send/resend window. */
constexpr int kMediaKeyInboxPollRounds = 90;
/** One deferred-key poll round (90 rounds ≈ 90 s). */
constexpr std::chrono::milliseconds kMediaKeyPollRound{1000};
/** Rate-limit PeerId→relay unknown drops (PreferLocal / non-contact dogfood). */
std::atomic<uint32_t> g_inbound_unmapped_audio_drops{0};

/** The 1:1 receive path logs from the transport's I/O thread (no bridge there). */
logging::Logger& DirectReceiveLog() {
  static logging::Logger logger = logging::getLogger("CallDirectPath");
  return logger;
}

} // namespace

CallDirectPath::CallDirectPath(std::string call_id, CallDirectPathQueries& host, const ICallSessionReader& sessions,
                               ICallMediaKeyStore& media_keys, std::shared_ptr<CallEngine> engine,
                               ICallMediaTransport& direct, mesh::PeerReachPorts reach)
    : call_id_(std::move(call_id)),
      host_(host),
      sessions_(sessions),
      media_keys_(media_keys),
      engine_(std::move(engine)),
      direct_(direct),
      reach_(std::move(reach)),
      connect_(direct, reach_),
      media_key_inbox_poll_rounds_(kMediaKeyInboxPollRounds) {
  redirectLogger("CallDirectPath");
}

bool CallDirectPath::ForThisCall(const std::string& call_id, const char* what) const {
  if (call_id == call_id_) {
    return true;
  }
  log().warning << what << " for call_id=" << call_id << " on the path of call_id=" << call_id_ << " (ignored)";
  return false;
}

CallMediaDirectCallbacks CallDirectPath::AcceptInbound(const CallMediaInboundHello& hello) {
  if (!ForThisCall(hello.call_id, "AcceptInbound")) {
    return {}; // the transport NACKs it
  }
  // On the owner, before the bundle's callbacks exist: bind the stream now. Media is read on the
  // transport's I/O, so the first frame must already see this call's stream (never the last one's).
  BindInboundPeer(hello.call_id, hello.peer_id);
  return MakeBundleCallbacks(hello.call_id, /*fixed_stream=*/0, "Inbound call-media");
}

void CallDirectPath::BindInboundPeer(const std::string& call_id, const std::string& inbound_peer_id) {
  // Prefer call-roster Account ID for PublisherStreamIdForIdentity. The hello's peer is the mesh
  // PeerId; hashing that yields a different stream_id than SoftMigrate (account:…). Never use
  // P2pPeerIdentityForCall as the mapping — with N≥2 remotes it returns an arbitrary peer
  // (dogfood: Moto PeerId → wrong person stream).
  std::string identity = inbound_peer_id;
  // Whether the hello's PeerId is known to be this account's (a contact's device, a learned mapping):
  // only then may it stand for the account's answer (PB-018) — a guessed identity just labels the stream.
  bool identity_known = inbound_peer_id.rfind("account:", 0) == 0;
  if (inbound_peer_id.rfind("account:", 0) != 0) {
    if (auto mapped = host_.RelayIdentityForMeshPeerId(call_id, inbound_peer_id);
        mapped && mapped->has_value() && !mapped->value().empty()) {
      identity = mapped->value();
      identity_known = true;
    } else if (!media_peer_identity_.empty() && media_peer_identity_.rfind("account:", 0) == 0) {
      // Last resort for 1:1 before contacts hydrate — only when dialed peer is the sole remote.
      if (auto sole = host_.PeerIdentityForCall(call_id);
          sole && sole->has_value() && sole->value() == media_peer_identity_) {
        identity = media_peer_identity_;
      }
    } else if (!pending_answerer_peer_.empty() && pending_answerer_peer_.rfind("account:", 0) == 0) {
      identity = pending_answerer_peer_;
    }
  }
  if (!identity.empty() && identity.rfind("account:", 0) == 0) {
    media_peer_identity_ = identity;
    receive_->remote_stream.store(PublisherStreamIdForIdentity(identity), std::memory_order_release);
    if (!inbound_peer_id.empty() && inbound_peer_id != identity) {
      log().info << "Inbound call-media mapped PeerId→account stream identity peer_id=" << inbound_peer_id
                 << " account=" << identity;
    }
    // B30: the hello may be the first sign the answerer accepted (the relay's Accept can lag) — from a
    // PeerId we know is theirs. Anyone holding the call id can dial; their relay Accept still decides.
    if (identity_known) {
      reports_.Emit(direct_report::InboundHello{call_id, identity, inbound_peer_id});
    } else {
      log().info << "Inbound call-media hello from an unmapped PeerId " << inbound_peer_id
                 << " is not taken as an accept call_id=" << call_id;
    }
  } else {
    // Do not hash PeerId into a mixer track — SoftMigrate uses Account stream ids. Defer until
    // BeginSession / CallAccept teaches PeerId→Account (moto contact often lacks peer_id).
    receive_->remote_stream.store(0, std::memory_order_release);
    log().warning << "Inbound call-media stream identity not account: peer_id="
                  << (inbound_peer_id.empty() ? "(empty)" : inbound_peer_id)
                  << " — deferring on_audio stream_id until Account identity known";
  }
  inbound_deferred_peer_id_ = (inbound_peer_id.rfind("account:", 0) == 0) ? std::string{} : inbound_peer_id;
}

CallMediaDirectCallbacks CallDirectPath::MakeBundleCallbacks(const std::string& call_id, const uint32_t fixed_stream,
                                                             const char* label) {
  CallMediaDirectCallbacks cbs;
  // Edges: the transport calls these on its I/O thread — they only report.
  cbs.on_connected = [outbox = outbox_, call_id, label]() {
    outbox.Emit(direct_event::BundleConnected{call_id, label});
  };
  cbs.on_media = [gate = receive_, engine = engine_, outbox = outbox_, call_id, fixed_stream](
                     uint8_t channel, uint32_t seq, uint8_t mark, const std::vector<uint8_t>& payload) {
    ReceiveDirectMedia(*gate, *engine, outbox, call_id, fixed_stream, channel, seq, mark, payload);
  };
  cbs.on_failed = [outbox = outbox_, call_id](const CallMediaFailure& failure) {
    outbox.Emit(direct_event::BundleFailed{call_id, failure.message, failure.GetCode()});
  };
  // k4: the call lost its last path — reconnect window (the offerer re-anchors).
  cbs.on_path_lost = [outbox = outbox_, call_id]() { outbox.Emit(direct_event::PathLost{call_id}); };
  // K014: the peer ended the call (bye): no failure, no reconnect.
  cbs.on_remote_ended = [outbox = outbox_, call_id]() { outbox.Emit(direct_event::PeerEnded{call_id}); };
  // k3: the transport moved the call to another path; the path label (UI snapshot) follows.
  cbs.on_path_changed = [outbox = outbox_, call_id](CallMediaLinkKind kind) {
    outbox.Emit(direct_event::PathChanged{call_id, kind});
  };
  return cbs;
}

void CallDirectPath::OnBundleFailed(const std::string& call_id, const std::string& reason, const CallMediaErr code) {
  if (YieldsToHop(call_id, reason, code == CallMediaErr::StreamClosed, DirectMediaReady())) {
    return;
  }
  if (code == CallMediaErr::Dropped) {
    SurfaceDropped(call_id, reason); // K014: the leg had connected — never a connect failure
    return;
  }
  log().warning << "Call-media failed call_id=" << call_id << " reason=" << reason;
  SurfaceConnectFailed(call_id, reason, /*stop_media=*/true);
}

void CallDirectPath::OnPathLost(const std::string& call_id) {
  // The peer releasing its 1:1 stream for the hop (SoftMigrate) closes it as bare as a lost path
  // (K014): when the call moves onto the hop, nothing reconnects. The lost stream is not "up".
  if (YieldsToHop(call_id, "path lost", /*soft_close=*/false, /*direct_ready=*/false)) {
    return;
  }
  Apply(CallDirectPlannerEvent::PathLost, call_id, media_peer_identity_);
}

void CallDirectPath::OnPeerEnded(const std::string& call_id) {
  if (!ForThisCall(call_id, "PeerEnded") || !media_session_) {
    return;
  }
  // K014: the peer hung up. Its call_leave (control lane) ends the call; until then nothing here
  // judges the stream it closed — no reconnect, no connect failure, no "dropped".
  log().info << "call-media: the peer ended the call call_id=" << call_id;
  peer_ended_ = true;
  CancelReanchor();
  CancelDirectUpgrade();
  CancelRelayStandby();
  CancelEscalateReach();
  reports_.Emit(direct_report::PeerEnded{call_id, media_peer_identity_});
  reports_.Emit(direct_report::Changed{call_id_});
}

bool CallDirectPath::YieldsToHop(const std::string& call_id, const std::string& reason, const bool soft_close,
                                 const bool direct_ready) {
  CallMedia* call_media = engine_->Runs() ? CallMediaFor(call_id) : nullptr;
  BundleFailureInput in;
  in.engine_call = engine_->Runs();
  in.engine_running = engine_->Runs() || engine_->RunsAnother();
  in.direct_ready = direct_ready;
  // IsPacketMode() is also true for 1:1 capture — the hop must be attached.
  in.hop_attached = call_media && call_media->HopAttached();
  in.media_connected = direct_connected_at_ms_ > 0 || in.hop_attached;
  in.hop_pending = call_media && (call_media->HopInFlight() || call_media->ExpectsHop());
  in.soft_close = soft_close;
  switch (DecideBundleFailure(in)) {
    case BundleFailureAction::Ignore:
      return true;
    case BundleFailureAction::CarriedByHop:
      log().info << "Ignoring call-media fail after SoftMigrate/SFU call_id=" << call_id << " reason=" << reason;
      direct_.Detach(call_id_);
      ClearMediaFailure();
      reports_.Emit(direct_report::Connected{call_id});
      reports_.Emit(direct_report::Changed{call_id_});
      return true;
    case BundleFailureAction::AwaitHop:
      log().info << "Ignoring call-media fail while awaiting SFU attach call_id=" << call_id << " reason=" << reason;
      if (call_media) {
        call_media->ExpectHopAttach();
      }
      direct_.Detach(call_id_);
      ClearMediaFailure();
      reports_.Emit(direct_report::InboxSyncWanted{call_id_});
      // Nothing is live until the hop attaches: the call waits for it (not "connected").
      reports_.Emit(direct_report::HandedToHop{call_id});
      reports_.Emit(direct_report::Changed{call_id_});
      return true;
    case BundleFailureAction::Fail:
      break;
  }
  return false;
}

CallDirectPath::~CallDirectPath() {
  receive_->open.store(false, std::memory_order_release); // frames still in flight on I/O drop
}

void CallDirectPath::SetReachDeps(mesh::PeerReachPorts reach) {
  reach_.SetDeps(std::move(reach));
}

void CallDirectPath::SetConnectDiagProbe(CallConnectEnvProbe probe) {
  connect_.SetDiagProbe(std::move(probe));
}

void CallDirectPath::SetSeedWarm(std::function<void()> warm) {
  seed_warm_ = std::move(warm);
}

void CallDirectPath::SetSeedReserve(std::function<void()> reserve) {
  seed_reserve_ = std::move(reserve);
}

void CallDirectPath::SetSeedParkAwait(mesh::PeerReachCoordinator::SeedParkAwait park) {
  // Remember the outcome so the UI can say why a connect failed (no seed reachable, e.g. a VPN
  // that drops UDP) — the reach coordinator itself keeps nothing across attempts.
  reach_.SetSeedParkAwait([state = seed_park_state_, park = std::move(park)](std::function<void(bool)> done,
                                                                             int timeout_ms) {
    const auto started = std::chrono::steady_clock::now();
    const uint64_t epoch = state->epoch.load(std::memory_order_acquire);
    park(
        [state, done = std::move(done), started, epoch, timeout_ms](bool parked) {
          const auto elapsed_ms =
              std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
          if (SeedParkOutcome::SeedUnreachable(parked, elapsed_ms, timeout_ms, epoch,
                                               state->epoch.load(std::memory_order_acquire))) {
            state->unreachable.store(true, std::memory_order_release);
          }
          done(parked);
        },
        timeout_ms);
  });
}

CallLinkCounters CallDirectPath::MediaLinkCounters() const {
  return direct_.IsActive() ? direct_.ActiveLinkCounters() : CallLinkCounters{};
}

std::string CallDirectPath::MediaPathKind() const {
  // The bound link is the truth: an answerer's inbound leg can ride a relay carrier while the
  // reach loop (and the dialer-only hop registry) think "punched" (dogfood 2026-09-24).
  if (direct_.IsActive() && direct_.ActiveLinkKind() == CallMediaLinkKind::Relayed) {
    return "circuit";
  }
  // A direct bound link wins over a relay hop still registered for the peer: after a k3
  // migration the relay stays the call's fallback, not its path.
  const bool bound_direct = direct_.IsActive() && direct_.ActiveLinkKind() == CallMediaLinkKind::Direct;
  if (bound_direct && reach_kind_ == mesh::PeerLinkKind::Relayed) {
    // Same rule the other way: the reach loop settled over the circuit while the call is bound on a
    // direct (punched) link the peer's hello opened — that call is not relayed (hard-lab FLIP race:
    // "circuit" skipped the relay standby and ran a pointless direct upgrade).
    return "punched";
  }
  if (!bound_direct && reach_.HasRelayHop(media_peer_identity_)) {
    return "circuit";
  }
  switch (reach_kind_) {
    case mesh::PeerLinkKind::Direct:
      return "direct";
    case mesh::PeerLinkKind::Punched:
      return "punched";
    case mesh::PeerLinkKind::Relayed:
      return "circuit";
    case mesh::PeerLinkKind::Unknown:
      break;
  }
  return {};
}

bool CallDirectPath::DirectMediaReady() const {
  return direct_.Phase() == CallMediaSessionPhase::MediaReady;
}

void CallDirectPath::SetDirectArmingPorts(CallDirectArmingPorts ports) {
  arming_ = std::move(ports);
}

void CallDirectPath::SetMediaKeyInboxPollRoundsForTest(const int rounds) {
  media_key_inbox_poll_rounds_ = rounds < 0 ? 0 : rounds;
}

void CallDirectPath::SetDialWaitBudgetMsForTest(const int budget_ms) {
  reach_.SetDialBudgetMsForTest(budget_ms);
}

void CallDirectPath::SetConnectAttemptTimeoutMsForTest(const int timeout_ms) {
  connect_.SetAttemptTimeoutMsForTest(timeout_ms);
}

void CallDirectPath::SetSeatPorts(CallDirectSeatPorts ports) {
  seat_ = std::move(ports);
}

CallDirectPlannerApplyContext CallDirectPath::BuildDirectPlannerContext(const std::string& call_id,
                                                                        const std::string& peer_identity) {
  CallDirectPlannerApplyContext ctx;
  ctx.allows_direct_path = !arming_.IsBound() || (arming_.direct_ops_allowed && arming_.direct_ops_allowed());
  ctx.stopping = stopping_.load(std::memory_order_acquire);
  ctx.peer_nonempty = !peer_identity.empty();
  if (!call_id.empty() && engine_->Runs() && (DirectMediaReady() || MediaConnected(call_id))) {
    ctx.media_live_same_call = true;
  }
  if (!call_id.empty()) {
    if (auto key = LoadActiveMediaKey(call_id); key) {
      ctx.has_media_key = true;
    }
  }
  return ctx;
}

void CallDirectPath::SetDirectPlannerPhase(const CallDirectPlannerPhase next, const CallDirectPlannerEvent ev,
                                           const std::string& call_id) {
  const CallDirectPlannerPhase prev = direct_planner_phase_;
  direct_planner_phase_ = next;
  log().info << "planner=Direct phase=" << CallDirectPlannerPhaseName(prev) << "->" << CallDirectPlannerPhaseName(next)
             << " event=" << CallDirectPlannerEventName(ev) << " call_id=" << call_id
             << " connect_gen=" << connect_generation_.load(std::memory_order_acquire);
  if (next == CallDirectPlannerPhase::Connecting || next == CallDirectPlannerPhase::Live ||
      next == CallDirectPlannerPhase::DegradedTxOnly) {
    ArmDirectHealthTimer();
  } else if (next == CallDirectPlannerPhase::Idle || next == CallDirectPlannerPhase::KeyWait) {
    if (next == CallDirectPlannerPhase::Idle) {
      CancelDirectHealthTimer();
    }
  }
}

void CallDirectPath::Apply(CallDirectPlannerEvent ev, const std::string& call_id, const std::string& peer_identity) {
  const CallDirectPlannerApplyContext ctx = BuildDirectPlannerContext(call_id, peer_identity);
  const CallDirectPlannerPhaseOutcome out = DecideCallDirectPlannerPhase(direct_planner_phase_, ev, ctx);
  if (out.decision == CallDirectPlannerDecision::Ignore) {
    log().info << "planner=Direct ignore event=" << CallDirectPlannerEventName(ev)
               << " phase=" << CallDirectPlannerPhaseName(direct_planner_phase_) << " call_id=" << call_id
               << " allows_direct=" << (ctx.allows_direct_path ? 1 : 0);
    return;
  }
  if (out.decision == CallDirectPlannerDecision::Transition && out.next != direct_planner_phase_) {
    SetDirectPlannerPhase(out.next, ev, call_id);
  } else {
    log().info << "planner=Direct keep phase=" << CallDirectPlannerPhaseName(direct_planner_phase_)
               << " event=" << CallDirectPlannerEventName(ev) << " call_id=" << call_id;
  }

  switch (ev) {
    case CallDirectPlannerEvent::TxOnlyGraceExpired:
      if (direct_planner_phase_ == CallDirectPlannerPhase::DegradedTxOnly) {
        std::string peer = peer_identity.empty() ? media_peer_identity_ : peer_identity;
        const std::string cid = call_id.empty() ? call_id_ : call_id;
        if (peer.empty() && !cid.empty()) {
          if (auto resolved = host_.PeerIdentityForCall(cid); resolved && resolved->has_value()) {
            peer = **resolved;
          }
        }
        if (!cid.empty() && !peer.empty()) {
          reports_.Emit(direct_report::Progress{cid, CallDirectPlannerPhase::DegradedTxOnly});
          EscalateTxOnlyViaCircuit(cid, peer);
        }
      }
      break;
    case CallDirectPlannerEvent::CircuitEscalated:
      break;
    case CallDirectPlannerEvent::KeyTimeout:
    case CallDirectPlannerEvent::ConnectFailed:
    case CallDirectPlannerEvent::Dropped:
    case CallDirectPlannerEvent::ConnectSucceeded:
    case CallDirectPlannerEvent::ScheduleOfferer:
    case CallDirectPlannerEvent::ScheduleAnswerer:
    case CallDirectPlannerEvent::KeyReady:
    case CallDirectPlannerEvent::ReleaseTransport:
    case CallDirectPlannerEvent::Stop:
      break;
    case CallDirectPlannerEvent::PathMigrated:
      // A TX-only call that moved (k3-4), or a reconnected one (k4), is connected again on its new
      // path: chrome follows.
      if (out.decision == CallDirectPlannerDecision::Transition) {
        CancelReanchor();
        reports_.Emit(direct_report::Connected{call_id});
      }
      break;
    case CallDirectPlannerEvent::PathLost:
      if (out.decision == CallDirectPlannerDecision::Transition) {
        reports_.Emit(direct_report::Progress{call_id, CallDirectPlannerPhase::Reconnecting});
        reports_.Emit(direct_report::Changed{call_id_});
        // The offerer reaches the peer again and moves the call onto that link; the answerer's
        // transport accepts the migrate (invite / accept is the agreement, as for the first connect).
        if (session_offerer_) {
          ScheduleReanchor(call_id, std::chrono::milliseconds(0));
        }
      }
      break;
  }
}

void CallDirectPath::SetOutbox(OwnerOutbox<DirectPathEvent> outbox) {
  outbox_ = std::move(outbox);
  connect_.SetOutbox(outbox_.For<ConnectEvent>(
      [](ConnectEvent event) { return DirectPathEvent{direct_event::ForConnect{std::move(event)}}; }));
}

void CallDirectPath::Handle(DirectPathEvent& event) {
  std::visit(
      [this](auto& e) {
        using E = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<E, direct_event::HealthTick>) {
          if (direct_health_timer_id_ == 0) {
            return; // cancelled since
          }
          direct_health_timer_id_ = outbox_.After(std::chrono::milliseconds(1000), direct_event::HealthTick{});
          OnDirectHealthTimerFire();
        } else if constexpr (std::is_same_v<E, direct_event::ReserveRenewTick>) {
          if (reserve_renew_timer_id_ == 0) {
            return;
          }
          reserve_renew_timer_id_ =
              outbox_.After(std::chrono::milliseconds(reserve_renew_interval_ms_), direct_event::ReserveRenewTick{});
          OnReserveRenewFire();
        } else if constexpr (std::is_same_v<E, direct_event::UpgradeDue>) {
          if (upgrade_timer_id_ != 0) {
            OnDirectUpgradeFire();
          }
        } else if constexpr (std::is_same_v<E, direct_event::StandbyDue>) {
          if (standby_timer_id_ != 0) {
            OnRelayStandbyFire();
          }
        } else if constexpr (std::is_same_v<E, direct_event::ReanchorDue>) {
          if (reanchor_timer_id_ != 0 && reanchor_armed_ && e.call_id == call_id_) {
            reanchor_timer_id_ = 0;
            Reanchor(e.call_id);
          }
        } else if constexpr (std::is_same_v<E, direct_event::RecoveryGraceOver>) {
          FailUnlessDirectRecovered(e.call_id, e.error, /*grace_used=*/true);
        } else if constexpr (std::is_same_v<E, direct_event::BundleConnected>) {
          log().info << e.label << " connected call_id=" << e.call_id;
          CommitDirectConnected(e.call_id);
        } else if constexpr (std::is_same_v<E, direct_event::BundleFailed>) {
          OnBundleFailed(e.call_id, e.reason, e.code);
        } else if constexpr (std::is_same_v<E, direct_event::PathLost>) {
          OnPathLost(e.call_id);
        } else if constexpr (std::is_same_v<E, direct_event::PeerEnded>) {
          OnPeerEnded(e.call_id);
        } else if constexpr (std::is_same_v<E, direct_event::PathChanged>) {
          OnPathChanged(e.call_id, e.kind);
        } else if constexpr (std::is_same_v<E, direct_event::EscalateRestart>) {
          RestartAfterEscalate(e.call_id, e.peer);
        } else if constexpr (std::is_same_v<E, direct_event::StartOfferer>) {
          RunOffererStart(e.call_id, e.peer);
        } else if constexpr (std::is_same_v<E, direct_event::MediaKeyReady>) {
          StartDeferredAnswerer(e.call_id);
        } else if constexpr (std::is_same_v<E, direct_event::KeyPollDue>) {
          OnKeyPollDue(e);
        } else if constexpr (std::is_same_v<E, direct_event::RebindInboundStream>) {
          RebindInboundStream(e.call_id);
        } else if constexpr (std::is_same_v<E, direct_event::ForConnect>) {
          connect_.Handle(e.event);
        } else if constexpr (std::is_same_v<E, direct_event::StepReady>) {
          steps_.Run(e.step.id, e.step.value.get());
        } else {
          static_assert(!sizeof(E), "handle every DirectPathEvent");
        }
      },
      event);
}

void CallDirectPath::OnPathChanged(const std::string& call_id, const CallMediaLinkKind kind) {
  log().info << "call-media path migrated call_id=" << call_id
             << " path=" << (kind == CallMediaLinkKind::Relayed ? "circuit" : "direct");
  if (kind == CallMediaLinkKind::Direct) {
    CancelDirectUpgrade();
    ArmRelayStandby(call_id); // after a failover onto a direct standby, keep a relay behind it
    // Off the relay onto a link the upgrade punch (or the peer's punch) opened.
    if (reach_kind_ == mesh::PeerLinkKind::Relayed || reach_kind_ == mesh::PeerLinkKind::Unknown) {
      reach_kind_ = mesh::PeerLinkKind::Punched;
    }
  } else {
    reach_kind_ = mesh::PeerLinkKind::Relayed;
  }
  Apply(CallDirectPlannerEvent::PathMigrated, call_id);
}

void CallDirectPath::CancelDirectHealthTimer() {
  outbox_.Cancel(direct_health_timer_id_);
}

void CallDirectPath::ArmDirectHealthTimer() {
  CancelDirectHealthTimer();
  // ~1s tick while Connecting/Live/Degraded — primary connect health / TX-only path (no UI poll).
  direct_health_timer_id_ = outbox_.After(std::chrono::milliseconds(1000), direct_event::HealthTick{});
}

void CallDirectPath::CancelReserveRenewal() {
  outbox_.Cancel(reserve_renew_timer_id_);
}

void CallDirectPath::ArmReserveRenewal() {
  CancelReserveRenewal();
  if (!seed_reserve_) {
    return;
  }
  // Reservations are a 15 s lease (StartReserve TTL) and nothing else renews them; a caller that
  // dials after the lease lapses finds no park (dogfood 2026-09-24 "Couldn't connect"). 10 s keeps
  // one lease overlapping the next, so the relay link also stays hot throughout.
  reserve_renew_timer_id_ =
      outbox_.After(std::chrono::milliseconds(reserve_renew_interval_ms_), direct_event::ReserveRenewTick{});
}

void CallDirectPath::OnReserveRenewFire() {
  if (stopping_.load(std::memory_order_acquire) || !media_session_) {
    CancelReserveRenewal();
    return;
  }
  log().info << "circuit reserve renew call_id=" << call_id_;
  if (seed_reserve_) {
    seed_reserve_();
  }
}

// --- k3: a relayed call keeps trying for a direct path; the transport moves onto it by itself ------

void CallDirectPath::ArmDirectUpgrade(const std::string& call_id) {
  PathKeepingInput in;
  in.offerer = session_offerer_;
  in.has_call = !call_id.empty();
  in.on_relay = MediaPathKind() == "circuit";
  in.already_for_call = upgrade_armed_ && call_id == call_id_;
  in.policy_upgrade_to_direct = in.has_call && PathPolicyFor(call_id).upgrade_to_direct;
  if (!ShouldArmDirectUpgrade(in)) {
    if (in.offerer && in.has_call && in.on_relay && !in.already_for_call) {
      log().info << "direct upgrade: not for this pair (a mobile end anchors on the relay) call_id=" << call_id;
    }
    return;
  }
  CancelDirectUpgrade();
  upgrade_armed_ = true;
  upgrade_attempt_ = 0;
  ScheduleDirectUpgrade();
}

void CallDirectPath::ScheduleDirectUpgrade() {
  const std::optional<int> delay = RetryDelayMs(kDirectUpgradeDelaysMs, upgrade_attempt_);
  if (!delay) {
    log().info << "direct upgrade: giving up call_id=" << call_id_ << " (stays on the relay)";
    return;
  }
  const int delay_ms = upgrade_delay_ms_for_test_ > 0 ? upgrade_delay_ms_for_test_ : *delay;
  upgrade_timer_id_ = outbox_.After(std::chrono::milliseconds(delay_ms), direct_event::UpgradeDue{});
}

void CallDirectPath::CancelDirectUpgrade() {
  outbox_.Cancel(upgrade_timer_id_);
  upgrade_armed_ = false;
}

void CallDirectPath::OnDirectUpgradeFire() {
  upgrade_timer_id_ = 0;
  const std::string call_id = upgrade_armed_ ? call_id_ : std::string{};
  if (stopping_.load(std::memory_order_acquire) || call_id.empty() || !(media_session_ && call_id == call_id_) ||
      direct_planner_phase_ != CallDirectPlannerPhase::Live || MediaPathKind() != "circuit") {
    CancelDirectUpgrade();
    return;
  }
  ++upgrade_attempt_;
  const std::string peer_id = CallPeerMeshId();
  log().info << "direct upgrade attempt=" << upgrade_attempt_ << " call_id=" << call_id << " peer=" << peer_id;
  reach_.UpgradeToDirect(
      peer_id,
      OnOwner<mesh::PeerReachCoordinator::UpgradeRoe>([this, call_id](mesh::PeerReachCoordinator::UpgradeRoe result) {
        if (!(upgrade_armed_ && call_id == call_id_)) {
          return;
        }
        if (result) {
          log().info << "direct upgrade: direct link up call_id=" << call_id << " (the call moves onto it)";
          return; // PathMigrated cancels the schedule; a failed migration retries from the transport
        }
        log().info << "direct upgrade miss call_id=" << call_id << " err=" << result.error().message;
        ScheduleDirectUpgrade();
      }));
}

// --- k6: a direct call keeps a relayed standby (K003) -----------------------------------------------

void CallDirectPath::ArmRelayStandby(const std::string& call_id) {
  PathKeepingInput in;
  in.offerer = session_offerer_;
  in.has_call = !call_id.empty();
  in.on_relay = MediaPathKind() == "circuit";
  in.already_for_call = standby_armed_ && call_id == call_id_;
  in.has_circuit_reach = reach_.HasCircuitReach();
  in.policy_relay_standby = in.has_call && PathPolicyFor(call_id).want_relay_standby;
  if (!ShouldArmRelayStandby(in)) {
    return;
  }
  CancelRelayStandby();
  standby_armed_ = true;
  standby_attempt_ = 0;
  ScheduleRelayStandby();
}

void CallDirectPath::ScheduleRelayStandby() {
  const std::optional<int> delay = RetryDelayMs(kRelayStandbyDelaysMs, standby_attempt_);
  if (!delay) {
    log().info << "relay standby: giving up call_id=" << call_id_ << " (a lost path re-anchors)";
    return;
  }
  const int delay_ms = standby_delay_ms_for_test_ > 0 ? standby_delay_ms_for_test_ : *delay;
  standby_timer_id_ = outbox_.After(std::chrono::milliseconds(delay_ms), direct_event::StandbyDue{});
}

void CallDirectPath::CancelRelayStandby() {
  outbox_.Cancel(standby_timer_id_);
  if (standby_reach_id_ != 0) {
    reach_.Cancel(standby_reach_id_);
    standby_reach_id_ = 0;
  }
  standby_armed_ = false;
}

void CallDirectPath::OnRelayStandbyFire() {
  standby_timer_id_ = 0;
  const std::string call_id = standby_armed_ ? call_id_ : std::string{};
  const bool still_wanted = !stopping_.load(std::memory_order_acquire) && !call_id.empty() && media_session_ &&
                            call_id == call_id_ && direct_planner_phase_ == CallDirectPlannerPhase::Live &&
                            MediaPathKind() != "circuit" && PathPolicyFor(call_id).want_relay_standby;
  if (!still_wanted) {
    CancelRelayStandby();
    return;
  }
  if (direct_.StandbyLinkKind() == CallMediaLinkKind::Relayed) {
    // The relayed path the call left for a direct one stayed as its standby: nothing to build.
    log().info << "relay standby up call_id=" << call_id << " (kept from the relayed path)";
    CancelRelayStandby();
    return;
  }
  ++standby_attempt_;
  const std::string peer_id = CallPeerMeshId();
  mesh::PeerReachRequest request;
  request.keys.push_back(peer_id);
  request.mode = mesh::PeerReachMode::Reach;
  request.exclude_direct = true; // a circuit, beside the direct link the call is on
  const CallStandbyPriority priority = StandbyPriorityFor(PathPolicyFor(call_id), MediaPathKind() == "punched");
  request.circuit_standby_priority = priority == CallStandbyPriority::High     ? mesh::CircuitStandbyPriority::High
                                     : priority == CallStandbyPriority::Medium ? mesh::CircuitStandbyPriority::Medium
                                                                               : mesh::CircuitStandbyPriority::Low;
  log().info << "relay standby attempt=" << standby_attempt_ << " call_id=" << call_id << " peer=" << peer_id
             << " priority=" << CallStandbyPriorityName(priority);
  const auto retry = [this, call_id]() {
    if (standby_armed_ && call_id == call_id_) {
      ScheduleRelayStandby();
    }
  };
  standby_reach_id_ = reach_.Ensure(
      std::move(request),
      OnOwner<mesh::PeerReachCoordinator::ReachRoe>(
          [this, call_id, retry](mesh::PeerReachCoordinator::ReachRoe reached) {
            if (!(standby_armed_ && call_id == call_id_)) {
              return;
            }
            standby_reach_id_ = 0;
            if (!reached) {
              log().info << "relay standby: no circuit (" << reached.error().message << ")";
              retry();
              return;
            }
            direct_.AddStandby(CallMediaLinkKind::Relayed, OnOwner<Roe<void>>([this, call_id, retry](Roe<void> added) {
                                 if (!(standby_armed_ && call_id == call_id_)) {
                                   return;
                                 }
                                 if (!added) {
                                   log().info << "relay standby: not added (" << added.error().message << ")";
                                   retry();
                                   return;
                                 }
                                 log().info << "relay standby up call_id=" << call_id;
                                 standby_armed_ = false;
                               }));
          }));
}

void CallDirectPath::ApplyPathPolicyToTransport(const std::string& call_id) {
  direct_.SetAutoMigrateToDirect(PathPolicyFor(call_id).upgrade_to_direct);
}

// --- k6: the pair's path policy changed (a mobility class flipped mid-call) ----------------------

void CallDirectPath::OnPathPolicyChanged(const std::string& call_id) {
  if (stopping_.load(std::memory_order_acquire) || call_id.empty() || !(media_session_ && call_id == call_id_)) {
    return;
  }
  const CallPathPolicy policy = PathPolicyFor(call_id);
  log().info << "path policy call_id=" << call_id << " upgrade=" << (policy.upgrade_to_direct ? 1 : 0)
             << " relay=" << CallRelayRoleName(policy.relay_role) << " relay_only=" << (policy.relay_only ? 1 : 0);
  ApplyPathPolicyToTransport(call_id);
  if (!policy.upgrade_to_direct) {
    CancelDirectUpgrade();
  } else if (direct_planner_phase_ == CallDirectPlannerPhase::Live) {
    ArmDirectUpgrade(call_id);
  }
  if (direct_planner_phase_ == CallDirectPlannerPhase::Live) {
    ArmRelayStandby(call_id);
  }
}

// --- k5: the device's network changed -----------------------------------------------------------

void CallDirectPath::OnLocalNetworkChanged() {
  const std::string call_id = media_session_ ? call_id_ : std::string{};
  if (stopping_.load(std::memory_order_acquire) || call_id.empty()) {
    return;
  }
  switch (DecideOnNetworkChange(direct_planner_phase_, MediaPathKind() == "circuit", session_offerer_)) {
    case NetworkChangeAction::Reanchor:
      log().info << "network changed: re-anchoring after the links settle call_id=" << call_id;
      ScheduleReanchor(call_id, std::chrono::milliseconds(network_settle_ms_));
      return;
    case NetworkChangeAction::RestartDirectUpgrade:
      log().info << "network changed: direct upgrade starts over call_id=" << call_id;
      CancelDirectUpgrade();
      ArmDirectUpgrade(call_id);
      return;
    case NetworkChangeAction::None:
      return;
  }
}

// --- k4: re-anchor a call that lost its last path ----------------------------------------------

void CallDirectPath::ScheduleReanchor(const std::string& call_id, const std::chrono::milliseconds delay) {
  CancelReanchor();
  reanchor_armed_ = true;
  reanchor_timer_id_ = outbox_.After(delay, direct_event::ReanchorDue{call_id});
}

void CallDirectPath::CancelReanchor() {
  outbox_.Cancel(reanchor_timer_id_);
  if (reanchor_reach_id_ != 0) {
    reach_.Cancel(reanchor_reach_id_);
    reanchor_reach_id_ = 0;
  }
  reanchor_armed_ = false;
}

void CallDirectPath::Reanchor(const std::string& call_id) {
  if (stopping_.load() || !engine_->Runs() || direct_planner_phase_ != CallDirectPlannerPhase::Reconnecting) {
    return;
  }
  const std::string peer_id = CallPeerMeshId();
  mesh::PeerReachRequest request;
  request.keys.push_back(peer_id);
  request.mode = mesh::PeerReachMode::Reach;
  // The same pair policy and H013 relays as the first connect (a relay-only pair stays relay-only).
  ApplyReachPolicy(request, call_id, /*offerer=*/true);
  log().info << "reconnect: reaching the peer again call_id=" << call_id << " peer=" << peer_id
             << " advertised_relays=" << request.advertised_relays.size()
             << " exclude_direct=" << (request.exclude_direct ? 1 : 0);
  const auto retry = [this, call_id]() {
    if (direct_planner_phase_ == CallDirectPlannerPhase::Reconnecting && reanchor_armed_ && call_id == call_id_) {
      ScheduleReanchor(call_id, std::chrono::milliseconds(reanchor_retry_ms_));
    }
  };
  reanchor_reach_id_ = reach_.Ensure(
      std::move(request),
      OnOwner<mesh::PeerReachCoordinator::ReachRoe>(
          [this, call_id, retry](mesh::PeerReachCoordinator::ReachRoe reached) {
            if (!(reanchor_armed_ && call_id == call_id_)) {
              return;
            }
            reanchor_reach_id_ = 0;
            if (direct_planner_phase_ != CallDirectPlannerPhase::Reconnecting) {
              return;
            }
            if (!reached) {
              log().info << "reconnect: peer not reached yet (" << reached.error().message << ")";
              retry();
              return;
            }
            const CallMediaLinkKind kind =
                reached->kind == mesh::PeerLinkKind::Relayed ? CallMediaLinkKind::Relayed : CallMediaLinkKind::Direct;
            direct_.MigrateTo(kind, OnOwner<Roe<void>>([this, call_id, retry](Roe<void> moved) {
                                if (!(reanchor_armed_ && call_id == call_id_)) {
                                  return;
                                }
                                if (!moved && moved.error().code == static_cast<int32_t>(CallMediaErr::AlreadyOnLink)) {
                                  // The stream already rides the link the peer was reached on: the reconnect
                                  // is done (a lab group call stayed Reconnecting on it and refused its hop).
                                  log().info << "reconnect: the call is on the reached link call_id=" << call_id;
                                  Apply(CallDirectPlannerEvent::PathMigrated, call_id);
                                  return;
                                }
                                if (!moved) {
                                  log().info << "reconnect: migrate failed (" << moved.error().message << ")";
                                  retry();
                                }
                                // OK: on_path_changed → PathMigrated → Live.
                              }));
          }));
}

void CallDirectPath::CancelEscalateReach() {
  if (escalate_reach_id_ != 0) {
    reach_.Cancel(escalate_reach_id_);
    escalate_reach_id_ = 0;
  }
}

void CallDirectPath::OnDirectHealthTimerFire() {
  if (direct_planner_phase_ == CallDirectPlannerPhase::Idle ||
      direct_planner_phase_ == CallDirectPlannerPhase::KeyWait ||
      direct_planner_phase_ == CallDirectPlannerPhase::Stopping) {
    CancelDirectHealthTimer();
    return;
  }
  PollMeshConnectHealth();
}

void CallDirectPath::CommitDirectConnected(const std::string& call_id) {
  if (call_id.empty()) {
    return;
  }
  Apply(CallDirectPlannerEvent::ConnectSucceeded, call_id, media_peer_identity_);
  if (direct_planner_phase_ != CallDirectPlannerPhase::Live &&
      direct_planner_phase_ != CallDirectPlannerPhase::DegradedTxOnly) {
    // Connected after this side's connect failed: the call may still be open (failed ≠ closed) —
    // the lifecycle resumes media over this stream if so; after Leave it ignores it.
    if (media_failure_ != MediaFailure::None && failed_open_ && call_id == call_id_ &&
        !failed_open_->resume_requested && DirectMediaReady()) {
      log().info << "call-media connected after connect failed call_id=" << call_id << " — resume?";
      failed_open_->resume_requested = true;
      reports_.Emit(direct_report::PeerReconnected{call_id});
    }
    return;
  }
  ClearMediaFailure();
  if (direct_connected_at_ms_ <= 0) {
    direct_connected_at_ms_ = util::NowUnixMs();
  }
  reports_.Emit(direct_report::Connected{call_id});
  ApplyPathPolicyToTransport(call_id);
  ArmDirectUpgrade(call_id);
  ArmRelayStandby(call_id);
  reports_.Emit(direct_report::Changed{call_id_});
}

void CallDirectPath::ReceiveDirectMedia(ReceiveGate& gate, CallEngine& engine,
                                        const OwnerOutbox<DirectPathEvent>& outbox, const std::string& call_id,
                                        const uint32_t fixed_stream, const uint8_t channel, const uint32_t seq,
                                        const uint8_t mark, const std::vector<uint8_t>& payload) {
  // The data plane stays on I/O, like the hop's frames: only atomics and the engine's thread-safe
  // surface here. Owner state it needs (the bound stream) is published into the gate.
  if (!gate.open.load(std::memory_order_acquire) || !engine.Runs() ||
      (gate.hop_carries_media && gate.hop_carries_media->load(std::memory_order_acquire))) {
    return;
  }
  // Outbound bundles know the dialed identity; inbound ones use the bound (or deferred) stream.
  const uint32_t remote_stream = fixed_stream != 0 ? fixed_stream : gate.remote_stream.load(std::memory_order_acquire);
  if (remote_stream == 0) {
    const int64_t now =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    int64_t not_before = gate.rebind_not_before_ms.load(std::memory_order_acquire);
    if (now >= not_before && gate.rebind_not_before_ms.compare_exchange_strong(not_before, now + 250)) {
      outbox.Emit(direct_event::RebindInboundStream{call_id});
    }
    const uint32_t n = g_inbound_unmapped_audio_drops.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || (n % 50) == 0) {
      DirectReceiveLog().warning << "Inbound call-media drop: stream not bound yet call_id=" << call_id
                                 << " drops=" << n;
    }
    return;
  }
  MediaEngine::MediaPacket pkt;
  pkt.stream_id = remote_stream;
  pkt.channel_id = channel;
  pkt.seq = seq;
  pkt.mark = mark;
  pkt.payload = payload;
  (void)engine.Deliver(pkt);
}

void CallDirectPath::RebindInboundStream(const std::string& call_id) {
  if (receive_->remote_stream.load(std::memory_order_acquire) != 0 || !engine_->Runs()) {
    return;
  }
  std::string account = media_peer_identity_;
  const std::string deferred = inbound_deferred_peer_id_;
  if (account.rfind("account:", 0) != 0 && !deferred.empty()) {
    if (auto mapped = host_.RelayIdentityForMeshPeerId(call_id, deferred);
        mapped && mapped->has_value() && !mapped->value().empty()) {
      account = mapped->value();
      media_peer_identity_ = account;
    }
  }
  if (account.rfind("account:", 0) != 0) {
    log().debug << "Inbound call-media stream still unknown peer_id=" << (deferred.empty() ? "(empty)" : deferred)
                << " media_peer=" << (media_peer_identity_.empty() ? "(empty)" : media_peer_identity_)
                << " call_id=" << call_id;
    return; // I/O asks again after its backoff
  }
  const uint32_t stream = PublisherStreamIdForIdentity(account);
  receive_->remote_stream.store(stream, std::memory_order_release);
  log().info << "Inbound call-media rebound stream_id=" << stream << " account=" << account << " call_id=" << call_id;
}

bool CallDirectPath::IsMeshConnectFailed() const {
  return media_failure_ == MediaFailure::ConnectFailed;
}

bool CallDirectPath::MeshConnectMissingMic() const {
  return mesh_connect_missing_mic_;
}

bool CallDirectPath::MeshConnectSeedUnreachable() const {
  return seed_park_state_->unreachable.load(std::memory_order_acquire);
}

void CallDirectPath::ClearMediaFailure() {
  media_failure_ = MediaFailure::None;
  mesh_connect_missing_mic_ = false;
  seed_park_state_->unreachable.store(false, std::memory_order_release);
  seed_park_state_->epoch.fetch_add(1, std::memory_order_acq_rel);
}

void CallDirectPath::PollMeshConnectHealth() {
  const std::string& call_id = call_id_;
  DirectHealthInput in;
  in.already_failed = media_failure_ != MediaFailure::None;
  in.engine_running = engine_->Runs();
  if (in.engine_running && !in.already_failed) {
    CallMedia* call_media = CallMediaFor(call_id);
    in.hop_in_flight = call_media && call_media->HopInFlight();
    in.hop_expected = call_media && call_media->ExpectsHop();
    in.connect_in_flight = connect_.InFlight();
    in.media_connected = MediaConnected(call_id);
    in.direct_ready = DirectMediaReady();
    in.direct_was_connected = direct_connected_at_ms_ > 0;
    in.hop_attached = HopAttachedFor(call_id);
    in.peer_ended = peer_ended_;
    in.has_call = !call_id.empty();
    if (in.has_call && !in.direct_ready) {
      if (auto joined = sessions_.CountJoined(call_id)) {
        in.n_joined = *joined;
      }
    }
    in.started_at_ms = engine_->StartedAtMs();
    in.now_ms = util::NowUnixMs();
    in.timeout_ms = kMeshConnectTimeoutMs;
    if (in.direct_was_connected && !in.direct_ready) {
      if (stream_gone_since_ms_ == 0) {
        stream_gone_since_ms_ = in.now_ms;
      }
      in.stream_gone_ms = in.now_ms - stream_gone_since_ms_;
    } else {
      stream_gone_since_ms_ = 0;
    }
  }
  switch (DecideDirectHealth(in)) {
    case DirectHealthAction::None:
      return;
    case DirectHealthAction::Healthy:
      ClearMediaFailure();
      MaybeEscalateTxOnlyDirect();
      return;
    case DirectHealthAction::HealConnected:
      ClearMediaFailure();
      log().info << "Heal call-media connected from direct stream call_id=" << call_id;
      CommitDirectConnected(call_id);
      MaybeEscalateTxOnlyDirect();
      return;
    case DirectHealthAction::Dropped:
      // K014: the leg's own report should have come; the call was connected, so it dropped.
      SurfaceDropped(call_id, "health: the 1:1 stream is gone after connect, no report");
      return;
    case DirectHealthAction::TimedOut:
      break;
  }
  log().warning << "Mesh connect timeout call_id=" << call_id;
  mesh_connect_missing_mic_ = !engine_->HasLocalCapture();
  SurfaceConnectFailed(call_id, "mesh connect timeout", /*stop_media=*/true, CallMediaErrorKind::ConnectTimeout);
}

void CallDirectPath::MaybeEscalateTxOnlyDirect() {
  std::string peer = media_peer_identity_;
  const std::string& call_id = call_id_;
  if (peer.empty() && !call_id.empty()) {
    if (auto resolved = host_.PeerIdentityForCall(call_id); resolved && resolved->has_value()) {
      peer = **resolved;
    }
  }
  const auto snap = engine_->HealthSnapshot();
  CallTxOnlyEscalateDecisionInput in;
  in.already_done = tx_only_escalation_done_;
  in.sfu_attached = HopAttachedFor(call_id_);
  in.stopping = stopping_.load();
  // Bound-link truth: never "escalate via circuit" when media already rides a relay carrier.
  in.media_path_kind = MediaPathKind();
  in.has_circuit_reach = reach_.HasCircuitReach();
  in.direct_active = direct_.IsActive();
  in.active_call_id = call_id;
  in.media_call_id = media_session_ ? call_id_ : std::string{};
  in.rx_audio_frames = snap.rx_audio_frames;
  in.tx_audio_frames = snap.tx_audio_frames;
  in.direct_connected_at_ms = direct_connected_at_ms_;
  in.now_ms = util::NowUnixMs();
  in.peer_nonempty = !peer.empty();
  if (!ShouldEscalateTxOnlyDirect(in)) {
    return;
  }
  tx_only_escalation_done_ = true;
  log().warning << "Call-media TX-only on path=" << (in.media_path_kind.empty() ? "unknown" : in.media_path_kind)
                << " — escalate via circuit call_id=" << call_id << " peer=" << peer
                << " tx_frames=" << snap.tx_audio_frames;
  Apply(CallDirectPlannerEvent::TxOnlyGraceExpired, call_id, peer);
}

void CallDirectPath::EscalateTxOnlyViaCircuit(const std::string& call_id, const std::string& peer) {
  // k3-4 make-before-break: the call keeps running on its path while a circuit to the peer is
  // built, then moves onto it. Only if that fails does the old break-before-make restart run.
  const std::string peer_id = CallPeerMeshId();
  if (peer_id.empty() || !reach_.HasCircuitReach()) {
    EscalateBreakBeforeMake(call_id, peer);
    return;
  }
  mesh::PeerReachRequest request;
  request.keys.push_back(peer_id);
  request.mode = session_offerer_ ? mesh::PeerReachMode::Reach : mesh::PeerReachMode::Await;
  request.exclude_direct = true;
  log().info << "TX-only escalate: building a circuit under the live call call_id=" << call_id << " peer=" << peer_id;
  escalate_reach_id_ = reach_.Ensure(
      std::move(request),
      OnOwner<mesh::PeerReachCoordinator::ReachRoe>([this, call_id,
                                                     peer](mesh::PeerReachCoordinator::ReachRoe reached) {
        escalate_reach_id_ = 0;
        if (stopping_.load() || !engine_->Runs() || direct_planner_phase_ != CallDirectPlannerPhase::DegradedTxOnly) {
          return;
        }
        if (!reached) {
          log().warning << "TX-only escalate: circuit failed (" << reached.error().message
                        << ") — restarting via circuit";
          EscalateBreakBeforeMake(call_id, peer);
          return;
        }
        direct_.MigrateTo(CallMediaLinkKind::Relayed, OnOwner<Roe<void>>([this, call_id, peer](Roe<void> moved) {
                            if (stopping_.load() || !engine_->Runs()) {
                              return;
                            }
                            if (moved) {
                              log().info << "TX-only escalate: call moved onto the circuit call_id=" << call_id;
                              return; // on_path_changed → PathMigrated brings the planner back to Live
                            }
                            if (direct_planner_phase_ == CallDirectPlannerPhase::DegradedTxOnly) {
                              log().warning << "TX-only escalate: migration failed (" << moved.error().message
                                            << ") — restarting via circuit";
                              EscalateBreakBeforeMake(call_id, peer);
                            }
                          }));
      }));
}

void CallDirectPath::EscalateBreakBeforeMake(const std::string& call_id, const std::string& peer) {
  Apply(CallDirectPlannerEvent::CircuitEscalated, call_id, peer);
  reach_kind_ = mesh::PeerLinkKind::Unknown;
  direct_connected_at_ms_ = 0;
  reach_.ForgetPath(peer);
  // Re-open transport under circuit without full engine Stop (keep capture).
  AbortConnectSequence();
  force_circuit_ensure_ = true; // consumed by the next reach (BuildReachRequest)
  direct_.Detach(call_id_);
  outbox_.Emit(direct_event::EscalateRestart{call_id, peer});
}

void CallDirectPath::RestartAfterEscalate(const std::string& call_id, const std::string& peer) {
  if (stopping_.load() || !engine_->Runs()) {
    return;
  }
  log().info << "TX-only escalate BeginSession role=" << (session_offerer_ ? "offerer" : "answerer")
             << " call_id=" << call_id;
  if (auto started = BeginSession(call_id, peer, session_offerer_); !started) {
    log().warning << "TX-only escalate BeginSession failed: " << started.error().message;
    FailUnlessDirectRecovered(call_id, started.error().message);
  }
}

void CallDirectPath::AbortConnectSequence() {
  connect_generation_.fetch_add(1, std::memory_order_acq_rel);
  connect_.Abort();
}

void CallDirectPath::FailUnlessDirectRecovered(const std::string& call_id, const std::string& err,
                                               const bool grace_used) {
  if (stopping_.load()) {
    return;
  }
  if (grace_used && !(media_session_ && call_id == call_id_)) {
    return; // the session moved on during the grace (stopped / another call)
  }
  const CallMediaSessionPhase phase = direct_.Phase();
  CallConnectFailureFacts facts;
  facts.direct_media_ready = DirectMediaReady();
  facts.inbound_in_progress = phase == CallMediaSessionPhase::HelloInbound || phase == CallMediaSessionPhase::Adopting;
  facts.hello_grace_used = grace_used;
  switch (DecideConnectFailure(facts)) {
    case CallConnectFailureDecision::Commit:
      log().info << "connect attempt failed but direct media is up (peer redial) — keep call_id=" << call_id
                 << " err=" << err;
      CommitDirectConnected(call_id);
      return;
    case CallConnectFailureDecision::WaitForHello:
      log().info << "connect attempt failed while the peer's hello is in progress — grace call_id=" << call_id;
      (void)outbox_.After(kInboundRecoveryGrace, direct_event::RecoveryGraceOver{call_id, err});
      return;
    case CallConnectFailureDecision::Fail:
      break;
  }
  SurfaceConnectFailed(call_id, err, /*stop_media=*/true);
}

void CallDirectPath::SurfaceConnectFailed(const std::string& call_id, const std::string& err, const bool stop_media,
                                          const CallMediaErrorKind kind) {
  if (!err.empty()) {
    reports_.Emit(direct_report::MediaError{call_id_, err, kind});
  }
  // H013: the caller tells the answerer it stopped (read before StopMeshMedia clears the session).
  const bool tell_peer =
      session_offerer_ && !call_id.empty() && media_session_ && call_id == call_id_ && !media_peer_identity_.empty();
  const std::string gave_up_peer = media_peer_identity_;
  // StopMeshMedia also resets the seed-park outcome; this failure is what it explains (dogfood 2026-10-01:
  // the "turn off your VPN" hint never showed because the stop ran first).
  const bool seed_unreachable = MeshConnectSeedUnreachable();
  StopFailedMedia(call_id, stop_media);
  if (!call_id.empty()) {
    reports_.Emit(direct_report::MediaAttempted{call_id});
  }
  Apply(CallDirectPlannerEvent::ConnectFailed, call_id, media_peer_identity_);
  reports_.Emit(direct_report::ConnectFailed{call_id});
  if (tell_peer) {
    reports_.Emit(direct_report::GaveUpReaching{call_id, gave_up_peer});
  }
  media_failure_ = MediaFailure::ConnectFailed;
  if (seed_unreachable) {
    seed_park_state_->unreachable.store(true, std::memory_order_release);
  }
  reports_.Emit(direct_report::Changed{call_id_});
}

void CallDirectPath::SurfaceDropped(const std::string& call_id, const std::string& reason) {
  log().warning << "call-media dropped after connect call_id=" << call_id << " reason=" << reason;
  reports_.Emit(direct_report::MediaError{call_id_, reason, CallMediaErrorKind::Dropped});
  StopFailedMedia(call_id, /*stop_media=*/true);
  Apply(CallDirectPlannerEvent::Dropped, call_id, media_peer_identity_);
  reports_.Emit(direct_report::Dropped{call_id});
  media_failure_ = MediaFailure::Dropped;
  reports_.Emit(direct_report::Changed{call_id_});
}

void CallDirectPath::StopFailedMedia(const std::string& call_id, const bool stop_media) {
  // Stop late EnsureViaCircuit / StartBridge before chrome refresh (dogfood SIGSEGV after give-up).
  reach_.AbortCircuitAttempts();
  if (stop_media && engine_->Runs()) {
    // StopMeshMedia clears the failure and the failed-open mark for Leave hygiene — the call is still
    // open, so the caller re-asserts the failure and this the mark.
    StopMeshMedia(call_id);
  } else if (!media_peer_identity_.empty()) {
    reach_.AbandonDial(media_peer_identity_);
  }
  if (!call_id.empty()) {
    failed_open_ = FailedOpenCall{};
  }
}

void CallDirectPath::OnPeerGaveUpReaching(const std::string& call_id) {
  if (call_id.empty() || !(media_session_ && call_id == call_id_) || session_offerer_ ||
      media_failure_ != MediaFailure::None || !connect_.InFlight()) {
    log().info << "peer gave up reaching us; not waiting for it call_id=" << call_id;
    return;
  }
  log().info << "peer gave up reaching us; stop waiting call_id=" << call_id;
  SurfaceConnectFailed(call_id, {}, /*stop_media=*/true);
}

std::string CallDirectPath::CallPeerMeshId() {
  // The peer's authenticated PeerId on the call's link — not ActiveParams().peer_key, which after a
  // path move is that link's dial key and can be a local alias (amp:burst:…): a relay cannot route
  // to it (hard-lab flip: the relay standby never came up).
  if (std::string peer_id = direct_.ActiveRemotePeerId(); !peer_id.empty()) {
    return peer_id;
  }
  if (std::string peer_id = ReachPeerIdFor(media_peer_identity_); !peer_id.empty()) {
    return peer_id;
  }
  return ReachPeerIdFor(direct_.ActiveParams().peer_key);
}

std::string CallDirectPath::ReachPeerIdFor(const std::string& key) {
  // Circuit / punch / OpenChannel keys are Amp PeerIds. Invite/Accept may pass account: —
  // resolve here (call roster knowledge).
  if (key.rfind("account:", 0) == 0) {
    if (auto mapped = host_.MeshPeerIdForAccount(key); mapped && mapped->has_value() && !mapped->value().empty()) {
      log().info << "CallMedia reach account→PeerId account=" << key << " peer_id=" << mapped->value();
      return mapped->value();
    }
  }
  return key;
}

mesh::PeerReachRequest CallDirectPath::BuildReachRequest(const CallMediaDirectConnectParams& params) {
  mesh::PeerReachRequest request;
  // Keep the account as an alias the mesh may know too (hard-lab / dogfood NAT: "endpoint not
  // registered").
  const std::string reach_key = ReachPeerIdFor(params.peer_key);
  request.keys.push_back(reach_key);
  if (reach_key != params.peer_key) {
    request.keys.push_back(params.peer_key);
  }
  // The offerer reaches; the answerer awaits the offerer's link (invite/accept is the agreement).
  request.mode = params.offerer ? mesh::PeerReachMode::Reach : mesh::PeerReachMode::Await;
  ApplyReachPolicy(request, params.call_id, params.offerer);
  // TX-only escalation: this connect insists on a relayed link (once).
  request.exclude_direct = std::exchange(force_circuit_ensure_, false) || request.exclude_direct;
  if (!params.offerer) {
    // H013: the caller starts only once our CallAccept crossed its inbox — wait for it, until it tells
    // us it gave up (OnPeerGaveUpReaching), the link comes up, or this hold ends.
    request.await_hold_ms = answerer_await_hold_ms_;
  }
  return request;
}

void CallDirectPath::ApplyReachPolicy(mesh::PeerReachRequest& request, const std::string& call_id,
                                      const bool offerer) const {
  const CallPathPolicy policy = PathPolicyFor(call_id);
  request.exclude_direct = policy.relay_only;
  request.allow_punch = policy.punch_at_start;
  if (offerer && advertised_relays_) {
    // H013: the answerer said where it is parked; its relays go first.
    request.advertised_relays = advertised_relays_(call_id);
  }
}

CallMediaConnectHooks CallDirectPath::MakeConnectHooks(const std::string& call_id) {
  CallMediaConnectHooks hooks;
  hooks.before_attempt = [this](const CallMediaDirectConnectParams& params) {
    if (!params.offerer) {
      return;
    }
    // Answerer often defers waiting on the relay — resend the epoch key each attempt. MediaKey is
    // addressed by roster account:, not the mesh dial PeerId.
    const std::string key_peer = (!media_peer_identity_.empty() && media_peer_identity_.rfind("account:", 0) == 0)
                                     ? media_peer_identity_
                                     : params.peer_key;
    reports_.Emit(direct_report::MediaKeyResendWanted{params.call_id, key_peer});
  };
  hooks.on_link_ready = [this](const mesh::PeerLinkKind kind) { reach_kind_ = kind; };
  hooks.on_finished = [this, call_id](CallMediaRoe result) {
    if (DirectMediaReady()) {
      CommitDirectConnected(call_id);
      return;
    }
    if (!result) {
      FailUnlessDirectRecovered(call_id, result.error().message);
    }
  };
  return hooks;
}

Roe<ByteVector> CallDirectPath::LoadActiveMediaKey(const std::string& call_id) const {
  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value()) {
    return Error("call session not found");
  }
  auto key = media_keys_.LoadEpochKey(call_id, (*session)->media_epoch);
  if (!key) {
    return key.error();
  }
  if (!key->has_value()) {
    return Error("call media key not ready");
  }
  return **key;
}

Roe<void> CallDirectPath::BeginSession(const std::string& call_id, const std::string& peer_identity, bool offerer) {
  if (arming_.IsBound() && arming_.direct_ops_allowed && !arming_.direct_ops_allowed()) {
    log().info << "BeginSession skipped (direct not armed) call_id=" << call_id
               << " arming=" << (arming_.arming_debug_name ? arming_.arming_debug_name() : "?");
    return Error("direct path not armed");
  }
  auto key = LoadActiveMediaKey(call_id);
  if (!key) {
    log().warning << "BeginSession LoadActiveMediaKey failed call_id=" << call_id
                  << " role=" << (offerer ? "offerer" : "answerer") << " err=" << key.error().message;
    return key.error();
  }
  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value()) {
    return Error("call session not found");
  }
  const uint32_t media_epoch = (*session)->media_epoch;

  ResetDirectSessionState(call_id, peer_identity, offerer);
  const bool keep_inbound = StopPriorDirectAttempt(offerer);
  log().info << "CallMedia BeginSession role=" << (offerer ? "offerer" : "answerer") << " call_id=" << call_id
             << " peer=" << peer_identity << " epoch=" << media_epoch << " keep_inbound=" << (keep_inbound ? 1 : 0);
  ParkOnBootstrapSeed();
  if (auto started = StartDirectEngine(call_id); !started) {
    return started;
  }
  if (keep_inbound && DirectMediaReady()) {
    log().info << "Media started with existing inbound stream call_id=" << call_id
               << " role=" << (offerer ? "offerer" : "answerer");
    CommitDirectConnected(call_id);
    return {};
  }
  if (keep_inbound) {
    // Inbound bundle still in hello / AwaitingMedia: keep it (no Detach) and let the connect
    // sequence join it — ConnectAsync → StartLeg adopts and succeeds only at MediaReady.
    log().info << "Media start joins inbound stream still in handshake call_id=" << call_id
               << " role=" << (offerer ? "offerer" : "answerer");
  }
  StartDirectConnect(call_id, peer_identity, offerer, media_epoch, *key);
  mesh_connect_missing_mic_ = false;
  reports_.Emit(direct_report::Changed{call_id_});
  return {};
}

void CallDirectPath::ResetDirectSessionState(const std::string& call_id, const std::string& peer_identity,
                                             bool offerer) {
  if (!(media_session_ && call_id == call_id_)) {
    tx_only_escalation_done_ = false;
  }
  failed_open_.reset();
  reports_.Emit(direct_report::MediaAttempted{call_id});
  media_session_ = true;
  media_peer_identity_ = peer_identity;
  session_offerer_ = offerer;
  direct_connected_at_ms_ = 0;
  stream_gone_since_ms_ = 0;
  peer_ended_ = false;
  // Otherwise keep what an inbound hello of this session bound; stop / teardown clear it between calls.
  if (peer_identity.rfind("account:", 0) == 0) {
    receive_->remote_stream.store(PublisherStreamIdForIdentity(peer_identity), std::memory_order_release);
  }
  audio_seq_.store(0);
  audio_instance_.store(mesh::NewMediaSenderInstance()); // a restarted numbering is a new sender instance
  ClearMediaFailure();
  if (!offerer) {
    pending_answerer_ = false;
    ++key_wait_gen_;
    pending_answerer_peer_.clear();
  }
}

bool CallDirectPath::StopPriorDirectAttempt(bool offerer) {
  // Answerer may already have accepted inbound hello (key landed first). Offerer must NOT Detach:
  // answerer-only dial often negotiates the stream before BeginSession runs on the offerer
  // (dogfood: Detach raced inbound → phone never read hello → "Failed to read call-media frame header").
  const bool keep_inbound = direct_.IsActive();
  const bool restarting = engine_->Runs() || connect_.InFlight();
  if (engine_->Runs()) {
    StopOwnEngine("prior direct attempt");
  }
  if (!offerer && !keep_inbound) {
    direct_.Detach(call_id_);
  }
  // Abort in-flight Connect from a prior BeginSession (Kick thrash used to Stop without bumping).
  if (restarting && !keep_inbound) {
    AbortConnectSequence();
  }
  return keep_inbound;
}

void CallDirectPath::ParkOnBootstrapSeed() {
  // Both roles park on org seed: answerer reverse-dial makes the *offerer* the circuit target
  // (dogfood 997c1c6f). Reserve keeps a Connected PeerLink so peer-id-only StartBridge can
  // EnsureAssociation without dialing into NAT / requiring a dial-book MA. Single entry —
  // ReserveOnBootstrapSeeds registers + associates (do not also Warm in parallel).
  if (seed_reserve_) {
    seed_reserve_();
    ArmReserveRenewal();
  } else if (seed_warm_) {
    seed_warm_();
  }
}

Roe<void> CallDirectPath::StartDirectEngine(const std::string& call_id) {
  const uint64_t send_gen = connect_generation_.load(std::memory_order_acquire);
  CallMedia* call_media = host_.CallMediaOf(call_id);
  if (!call_media) {
    return Error("no live call for media " + call_id);
  }
  auto started = call_media->StartEngine(CallMediaPath::Direct, [this, send_gen](const MediaEngine::MediaPacket& pkt) {
    if (!mesh::IsAudioChannel(pkt.channel_id) && !mesh::IsVideoChannel(pkt.channel_id)) {
      return;
    }
    // SoftMigrate ReleaseDirectTransport bumps connect_generation_ before Detach.
    if (connect_generation_.load(std::memory_order_acquire) != send_gen) {
      return;
    }
    // Audio is renumbered here (its own counter and instance); video keeps the engine's seq and instance.
    const bool audio = mesh::IsAudioChannel(pkt.channel_id);
    const uint32_t seq = audio ? (audio_seq_.fetch_add(1) + 1) : pkt.seq;
    const uint64_t instance = audio ? audio_instance_.load(std::memory_order_acquire) : pkt.instance;
    (void)direct_.SendMedia(static_cast<uint8_t>(pkt.channel_id), pkt.payload, seq, pkt.mark, instance);
  });
  if (!started) {
    return started;
  }
  // A started engine is a fresh attempt: a failure surfaced before it no longer stands. Connected
  // waits for the direct stream (CommitDirectConnected).
  ClearMediaFailure();
  reports_.Emit(direct_report::Changed{call_id_});
  return {};
}

void CallDirectPath::StartDirectConnect(const std::string& call_id, const std::string& peer_identity, bool offerer,
                                        uint32_t media_epoch, const ByteVector& media_key) {
  CallMediaDirectConnectParams params;
  params.peer_key = peer_identity;
  params.call_id = call_id;
  params.media_epoch = media_epoch;
  params.media_key = media_key;
  params.offerer = offerer;
  // Prefer mesh PeerId for OpenChannel. Account: may be "dialable" via a stale alias while the
  // Connected PeerLink lives under PeerId (dogfood 7bd62: AssociationNotReady forever).
  if (peer_identity.rfind("account:", 0) == 0) {
    // Account → PeerId is roster knowledge (here); which key the mesh can dial is link knowledge.
    if (auto mapped = host_.MeshPeerIdForAccount(peer_identity);
        mapped && mapped->has_value() && !mapped->value().empty()) {
      params.peer_key = reach_.PreferDialKey(peer_identity, **mapped);
    } else {
      log().info << "CallMedia dial key account (no PeerId map) account=" << peer_identity;
    }
  }
  // V049 / B31: both roles dial immediately (simultaneous open). CallMediaDirect claims one
  // stream and elects under A026; inbound still wins if it lands first (keep_inbound).
  ApplyPathPolicyToTransport(call_id);
  CallMediaConnectRequest request;
  request.reach = BuildReachRequest(params);
  request.params = std::move(params);
  request.callbacks = MakeBundleCallbacks(call_id, PublisherStreamIdForIdentity(peer_identity), "Call-media");
  connect_.Start(std::move(request), MakeConnectHooks(call_id));
}

Roe<void> CallDirectPath::StartMediaAsOfferer(const std::string& call_id, const std::string& peer_identity) {
  return BeginSession(call_id, peer_identity, true);
}

Roe<void> CallDirectPath::StartMediaAsAnswerer(const std::string& call_id, const std::string& peer_identity) {
  return BeginSession(call_id, peer_identity, false);
}

void CallDirectPath::ScheduleDirectStart(const std::string& call_id, const std::string& peer_identity,
                                         const bool offerer) {
  if (!ForThisCall(call_id, "ScheduleDirectStart")) {
    return;
  }
  if (offerer) {
    ScheduleStartMediaAsOfferer(call_id, peer_identity);
  } else {
    ScheduleStartMediaAsAnswerer(call_id, peer_identity);
  }
}

void CallDirectPath::ScheduleStartMediaAsOfferer(const std::string& call_id, const std::string& peer_identity) {
  // Mark before UI hop so CallController orphan auto-Leave cannot race CallAccept→Active.
  reports_.Emit(direct_report::MediaAttempted{call_id});
  outbox_.Emit(direct_event::StartOfferer{call_id, peer_identity});
}

void CallDirectPath::RunOffererStart(const std::string& call_id, const std::string& peer_identity) {
  Apply(CallDirectPlannerEvent::ScheduleOfferer, call_id, peer_identity);
  if (direct_planner_phase_ != CallDirectPlannerPhase::Arming &&
      direct_planner_phase_ != CallDirectPlannerPhase::Connecting &&
      direct_planner_phase_ != CallDirectPlannerPhase::KeyWait) {
    return;
  }
  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value()) {
    log().info << "StartMediaAsOfferer skip: no session call_id=" << call_id;
    Apply(CallDirectPlannerEvent::Stop, call_id, peer_identity);
    return;
  }
  if ((*session)->state == CallSessionState::Ended) {
    log().info << "StartMediaAsOfferer skip: session ended call_id=" << call_id;
    Apply(CallDirectPlannerEvent::Stop, call_id, peer_identity);
    return;
  }
  if (engine_->Runs()) {
    log().info << "StartMediaAsOfferer skip: media already active call_id=" << call_id;
    return;
  }
  log().info << "StartMediaAsOfferer call_id=" << call_id << " peer=" << peer_identity;
  SetDirectPlannerPhase(CallDirectPlannerPhase::Connecting, CallDirectPlannerEvent::ScheduleOfferer, call_id);
  if (auto started = StartMediaAsOfferer(call_id, peer_identity); !started) {
    log().warning << "StartMediaAsOfferer failed: " << started.error().message;
    SurfaceConnectFailed(call_id, started.error().message, /*stop_media=*/true);
  }
}

void CallDirectPath::ScheduleStartMediaAsAnswerer(const std::string& call_id, const std::string& peer_identity) {
  PP_ASSERT_ON_OWNER(kMediaSessionsOwner);
  reports_.Emit(direct_report::MediaAttempted{call_id});
  RunAnswererStart(call_id, peer_identity);
}

void CallDirectPath::RunAnswererStart(const std::string& call_id, const std::string& peer_identity) {
  log().info << "ScheduleStartMediaAsAnswerer call_id=" << call_id << " peer=" << peer_identity;
  // The owner armed the call for the 1:1 path before starting it here (calls down).
  Apply(CallDirectPlannerEvent::ScheduleAnswerer, call_id, peer_identity);
  if (direct_planner_phase_ != CallDirectPlannerPhase::Arming &&
      direct_planner_phase_ != CallDirectPlannerPhase::Connecting &&
      direct_planner_phase_ != CallDirectPlannerPhase::KeyWait) {
    return;
  }
  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value() || (*session)->state == CallSessionState::Ended) {
    log().info << "ScheduleStartMediaAsAnswerer skip (no active session) call_id=" << call_id
               << " has_session=" << (session && session->has_value() ? 1 : 0)
               << " state=" << (session && session->has_value() ? static_cast<int>((*session)->state) : -1);
    Apply(CallDirectPlannerEvent::Stop, call_id, peer_identity);
    return;
  }
  if (engine_->Runs()) {
    log().info << "ScheduleStartMediaAsAnswerer skip (already active) call_id=" << call_id
               << " engine=" << (engine_->Runs() ? 1 : 0) << " direct=" << (direct_.IsActive() ? 1 : 0);
    return;
  }
  if (auto key = LoadActiveMediaKey(call_id); !key) {
    DeferAnswererUntilMediaKey(call_id, peer_identity, key.error().message);
    return;
  }
  log().info << "ScheduleStartMediaAsAnswerer key ready — BeginSession StartDuplex call_id=" << call_id;
  SetDirectPlannerPhase(CallDirectPlannerPhase::Connecting, CallDirectPlannerEvent::ScheduleAnswerer, call_id);
  if (auto started = StartMediaAsAnswerer(call_id, peer_identity); !started) {
    log().warning << "StartMediaAsAnswerer failed: " << started.error().message;
    SurfaceConnectFailed(call_id, started.error().message, /*stop_media=*/true);
  } else {
    log().info << "answerer StartDuplex ok call_id=" << call_id << " direct=" << (direct_.IsActive() ? 1 : 0)
               << " engine=" << (engine_->Runs() ? 1 : 0);
  }
}

void CallDirectPath::DeferAnswererUntilMediaKey(const std::string& call_id, const std::string& peer_identity,
                                                const std::string& reason) {
  // V015: epoch-1 key is sent by offerer on CallAccept — defer until it lands.
  SetDirectPlannerPhase(CallDirectPlannerPhase::KeyWait, CallDirectPlannerEvent::ScheduleAnswerer, call_id);
  log().info << "Defer answerer media until CallMediaKey call_id=" << call_id << " reason=" << reason;
  pending_answerer_ = true;
  pending_answerer_peer_ = peer_identity;
  reports_.Emit(direct_report::MediaAttempted{call_id});
  reports_.Emit(direct_report::MediaDeferred{call_id});
  // Each poll round is a delayed event naming this wait; a newer wait (or none) makes it stale.
  const uint64_t wait = ++key_wait_gen_;
  if (media_key_inbox_poll_rounds_ <= 0) {
    outbox_.Emit(direct_event::KeyPollDue{call_id, wait, 0});
    return;
  }
  reports_.Emit(direct_report::InboxSyncWanted{call_id_});
  (void)outbox_.After(kMediaKeyPollRound, direct_event::KeyPollDue{call_id, wait, 0});
}

void CallDirectPath::OnKeyPollDue(const direct_event::KeyPollDue& due) {
  // Accept-time SyncInbox often races the offerer's MediaKey send — keep polling (one request a
  // round; SyncInbox coalesces, so a request need not start HTTP).
  if (stopping_.load(std::memory_order_acquire) || key_wait_gen_ != due.wait) {
    return; // key arrived, Leave, or a newer wait
  }
  if (due.round < media_key_inbox_poll_rounds_ && LoadActiveMediaKey(due.call_id)) {
    log().info << "Deferred MediaKey found in store — kick start call_id=" << due.call_id;
    OnMediaKeyReady(due.call_id);
    return;
  }
  if (due.round + 1 < media_key_inbox_poll_rounds_) {
    reports_.Emit(direct_report::InboxSyncWanted{call_id_});
    (void)outbox_.After(kMediaKeyPollRound, direct_event::KeyPollDue{due.call_id, due.wait, due.round + 1});
    return;
  }
  // Surface failure — do not leave chrome stuck in MediaPending forever.
  OnDeferredMediaKeyTimeout(due.call_id);
}

void CallDirectPath::OnDeferredMediaKeyTimeout(const std::string& call_id) {
  if (!(pending_answerer_ && call_id == call_id_)) {
    return; // key arrived, Leave, or superseding Accept
  }
  pending_answerer_ = false;
  ++key_wait_gen_;
  pending_answerer_peer_.clear();
  log().warning << "Deferred MediaKey wait exhausted call_id=" << call_id << " — ConnectFailed";
  media_failure_ = MediaFailure::ConnectFailed;
  reports_.Emit(direct_report::MediaError{call_id_, Localized("call.error.media_key_timeout")});
  Apply(CallDirectPlannerEvent::KeyTimeout, call_id);
  reports_.Emit(direct_report::ConnectFailed{call_id});
  reports_.Emit(direct_report::Changed{call_id_});
}

void CallDirectPath::OnMediaKeyReady(const std::string& call_id) {
  if (call_id.empty() || !ForThisCall(call_id, "OnMediaKeyReady")) {
    return;
  }
  // The start runs as its own event, never inside the key exchange that reported the key.
  outbox_.Emit(direct_event::MediaKeyReady{call_id});
}

void CallDirectPath::StartDeferredAnswerer(const std::string& call_id) {
  std::string peer = pending_answerer_peer_;
  const bool pending = pending_answerer_ && call_id == call_id_;
  if (!pending) {
    // Key stored for later Accept LoadActiveMediaKey — do NOT auto-start. Late keys from a
    // prior call were starting answerer media on the wrong call_id (Samsung dogfood).
    log().info << "CallMediaKey stored (not deferred yet) call_id=" << call_id;
    return;
  }
  if (peer.empty()) {
    if (auto resolved = host_.PeerIdentityForCall(call_id); resolved && resolved->has_value()) {
      peer = **resolved;
    }
  }
  log().info << "CallMediaKey ready — starting deferred answerer media call_id=" << call_id;
  pending_answerer_ = false;
  ++key_wait_gen_;
  pending_answerer_peer_.clear();
  Apply(CallDirectPlannerEvent::KeyReady, call_id, peer);
  // The owner arms the call and starts the answerer through the call's media (calls down).
  reports_.Emit(direct_report::MediaKeyReady{call_id, peer});
}

void CallDirectPath::StopMeshMedia(const std::string& call_id) {
  // Bridge state, the connect sequence and MediaEngine::Stop (SDL capture — CALLS.md) are
  // calls-owner only; every stop reaches here through the owner's events.
  PP_ASSERT_ON_OWNER(kMediaSessionsOwner);
  if (!call_id.empty() && !ForThisCall(call_id, "StopMeshMedia")) {
    return;
  }
  receive_->remote_stream.store(0, std::memory_order_release); // the next call binds its own
  // Abort any Connect sequence before Detach — LeaveCall can run while Connect is mid-dial.
  reach_.AbortCircuitAttempts();
  Apply(CallDirectPlannerEvent::Stop, call_id, media_peer_identity_);
  AbortConnectSequence();
  CancelDirectHealthTimer();
  CancelReserveRenewal();
  CancelDirectUpgrade();
  CancelRelayStandby();
  CancelEscalateReach();
  CancelReanchor();
  const std::string peer = media_peer_identity_;
  if (pending_answerer_ && call_id == call_id_) {
    pending_answerer_ = false;
    ++key_wait_gen_;
    pending_answerer_peer_.clear();
  }
  reach_.ReleasePeer(peer);
  if (CallOverHere()) {
    direct_.HangUp(call_id_); // K014: the peer hears `bye` — the close that follows is the call's end
  } else {
    direct_.Detach(call_id_); // the call stays open (a failed connect, a restart): a lost path to the peer
  }
  media_peer_identity_.clear();
  media_session_ = false;
  reach_kind_ = mesh::PeerLinkKind::Unknown;
  force_circuit_ensure_ = false;
  session_offerer_ = false;
  direct_connected_at_ms_ = 0;
  stream_gone_since_ms_ = 0;
  peer_ended_ = false;
  tx_only_escalation_done_ = false;
  ClearMediaFailure();
  failed_open_.reset();

  // This call's media only (V057): another call's leftover is the device's to stop (the seat and
  // LiveCallMedia::StopMediaExcept, before a call takes media — dogfood cbe535). The call's media
  // stops whatever the engine still runs.
  if (!engine_->Runs()) {
    return;
  }
  log().info << "StopMeshMedia stopping engine call_id=" << call_id_ << " leave=" << call_id;
  StopOwnEngine("stop mesh media");
}

CallMedia* CallDirectPath::CallMediaFor(const std::string& call_id) {
  // Quietly null for a call not admitted here (a leftover engine id): nothing to ask about it.
  return !call_id.empty() && host_.LiveCallOf(call_id) ? host_.CallMediaOf(call_id) : nullptr;
}

bool CallDirectPath::CallOverHere() const {
  const LiveCall* call = host_.LiveCallOf(call_id_);
  return !call || !call->IsOpen();
}

bool CallDirectPath::MediaConnected(const std::string& call_id) {
  return direct_connected_at_ms_ > 0 || HopAttachedFor(call_id);
}

bool CallDirectPath::HopAttachedFor(const std::string& call_id) {
  CallMedia* call_media = CallMediaFor(call_id);
  return call_media && call_media->HopAttached();
}

void CallDirectPath::StopOwnEngine(const char* why) {
  if (CallMedia* call_media = CallMediaFor(call_id_)) {
    call_media->StopEngine(why);
    return;
  }
  // The call is gone from LiveCalls (its media is being dropped): nothing of it to stop here.
  log().info << "engine stop without a live call call_id=" << call_id_ << " (" << why << ")";
}

void CallDirectPath::ReleaseDirectTransport() {
  if (seat_.IsBound()) {
    ReleaseDirectTransport(seat_.current_token());
    return;
  }
  ReleaseDirectTransportBody();
}

void CallDirectPath::ReleaseDirectTransport(const CallMediaSeat::Token& token) {
  if (seat_.IsBound()) {
    if (!seat_.allows_path_op(token)) {
      log().info << "ReleaseDirectTransport skip (token not bound) call_id=" << token.call_id
                 << " bound=" << (seat_.bound_call_id ? seat_.bound_call_id() : "");
      return;
    }
  }
  ReleaseDirectTransportBody();
}

void CallDirectPath::ReleaseDirectTransportBody() {
  // SoftMigrate: drop 1:1 transport; keep MediaEngine capture feeding media_relay.
  Apply(CallDirectPlannerEvent::ReleaseTransport, media_session_ ? call_id_ : std::string{}, media_peer_identity_);
  AbortConnectSequence();
  CancelDirectHealthTimer();
  const std::string peer = media_peer_identity_;
  reach_.ReleasePeer(peer);
  direct_.Detach(call_id_);
  media_peer_identity_.clear();
  reach_kind_ = mesh::PeerLinkKind::Unknown;
  inbound_deferred_peer_id_.clear();
  receive_->remote_stream.store(0, std::memory_order_release);
  // Do not clear the remote audio tracks here — SoftMigrate+2s would wipe live media_relay tracks
  // that already replaced 1:1 (dogfood: streams look healthy then Moto silent on PreferLocal).
  // 1:1 on_audio is already ignored once the hop is attached; stream_id==1 is dropped in engine.
  ClearMediaFailure();
  // The hop's attach completion reports the call live (LiveCall Status) — not ReleaseDirect alone.
  if (engine_->Runs()) {
    reports_.Emit(direct_report::Connected{call_id_});
  }
  reports_.Emit(direct_report::Changed{call_id_});
}

void CallDirectPath::NotePeerIdRelayMapping(const std::string& peer_id, const std::string& relay_identity) {
  if (peer_id.empty() || relay_identity.rfind("account:", 0) != 0) {
    return;
  }
  if (inbound_deferred_peer_id_.empty() || inbound_deferred_peer_id_ != peer_id) {
    return;
  }
  media_peer_identity_ = relay_identity;
  const uint32_t stream = PublisherStreamIdForIdentity(relay_identity);
  receive_->remote_stream.store(stream, std::memory_order_release);
  log().info << "Inbound call-media mapping from CallAccept/Invite stream_id=" << stream << " peer_id=" << peer_id
             << " account=" << relay_identity;
}

void CallDirectPath::DetachReach() {
  SetReachDeps({});
  SetConnectDiagProbe({}); // it reads the dial registry the reach just let go
}

void CallDirectPath::AttachReach(const CallDirectPathDeps& deps) {
  SetReachDeps(deps.Reach());
  SetConnectDiagProbe(deps.diag_env);
  SetSeedWarm(deps.seed_warm);
  SetSeedReserve(deps.seed_reserve);
  SetSeedParkAwait(deps.seed_park_await);
  SetPathPolicyProvider(deps.path_policy);
  SetAdvertisedRelaysProvider(deps.advertised_relays);
}

CallDirectPathStatus CallDirectPath::Status() const {
  CallDirectPathStatus out;
  out.media_path_kind = MediaPathKind();
  out.link = MediaLinkCounters();
  out.connect_failed = IsMeshConnectFailed();
  out.missing_mic = out.connect_failed && MeshConnectMissingMic();
  out.seed_unreachable = out.connect_failed && MeshConnectSeedUnreachable();
  out.connect_inflight = IsConnectWorkerInflight();
  return out;
}

void CallDirectPath::PrepareForTeardown(int /*timeout_ms*/) {
  // Its owner is stopping: no more arming / seat asks back into it.
  arming_ = {};
  seat_ = {};
  receive_->remote_stream.store(0, std::memory_order_release);
  stopping_.store(true, std::memory_order_release);
  reach_.AbortCircuitAttempts();
  Apply(CallDirectPlannerEvent::Stop, media_session_ ? call_id_ : std::string{}, media_peer_identity_);
  AbortConnectSequence();
  connect_.Shutdown();
  CancelDirectHealthTimer();
  CancelReserveRenewal();
  CancelDirectUpgrade();
  CancelRelayStandby();
  CancelEscalateReach();
  CancelReanchor();
  const std::string peer = media_peer_identity_;
  const std::string call_id = media_session_ ? call_id_ : std::string{};
  pending_answerer_ = false;
  ++key_wait_gen_;
  pending_answerer_peer_.clear();
  reach_.ReleasePeer(peer);
  direct_.Detach(call_id_);
  media_peer_identity_.clear();
  media_session_ = false;
  ClearMediaFailure();

  if (!call_id.empty() && engine_->Runs()) {
    StopOwnEngine("1:1 stop");
  }
}

Roe<void> CallDirectPath::RetryMeshMedia(const std::string& call_id) {
  if (call_id.empty()) {
    return Error("call_id required");
  }
  if (!ForThisCall(call_id, "RetryMeshMedia")) {
    return Error("not this path's call");
  }
  // Restarts the engine and the connect sequence — calls owner only.
  PP_ASSERT_ON_OWNER(kMediaSessionsOwner);
  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value() || (*session)->state == CallSessionState::Ended) {
    return Error("Call session not found");
  }
  std::string peer = media_peer_identity_;
  if (peer.empty()) {
    if (const auto call = PeerRoleFromLiveCall(call_id)) {
      peer = call->peer_identity;
    }
  }
  if (peer.empty()) {
    if (auto resolved = host_.PeerIdentityForCall(call_id); resolved && resolved->has_value()) {
      peer = **resolved;
    }
  }
  if (peer.empty()) {
    return Error("No peer for call retry");
  }
  ClearMediaFailure();
  reach_.ForgetPath(peer);
  if (engine_->Runs()) {
    StopOwnEngine("retry");
  }
  direct_.Detach(call_id_);
  ArmPlannerForRestart(call_id, peer, true);
  return BeginSession(call_id, peer, true);
}

void CallDirectPath::ArmPlannerForRestart(const std::string& call_id, const std::string& peer_identity,
                                          const bool offerer) {
  Apply(offerer ? CallDirectPlannerEvent::ScheduleOfferer : CallDirectPlannerEvent::ScheduleAnswerer, call_id,
        peer_identity);
  Apply(CallDirectPlannerEvent::KeyReady, call_id, peer_identity);
}

Roe<void> CallDirectPath::ResumeMeshMediaFromInbound(const std::string& call_id) {
  if (call_id.empty()) {
    return Error("call_id required");
  }
  if (!ForThisCall(call_id, "ResumeMeshMediaFromInbound")) {
    return Error("not this path's call");
  }
  PP_ASSERT_ON_OWNER(kMediaSessionsOwner);
  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value() || (*session)->state == CallSessionState::Ended) {
    return Error("Call session not found");
  }
  if (!failed_open_ || call_id != call_id_) {
    return Error("call is not failed-open here");
  }
  if (!DirectMediaReady()) {
    return Error("peer stream gone before resume");
  }
  const auto call = PeerRoleFromLiveCall(call_id);
  if (!call) {
    return Error("No live 1:1 call to resume");
  }
  log().info << "resume call media over the peer's stream call_id=" << call_id
             << " role=" << (call->offerer ? "offerer" : "answerer");
  ClearMediaFailure();
  ArmPlannerForRestart(call_id, call->peer_identity, call->offerer);
  // BeginSession keeps the active inbound bundle (StopPriorDirectAttempt) and commits it.
  return BeginSession(call_id, call->peer_identity, call->offerer);
}

std::optional<CallDirectPath::CallPeerRole> CallDirectPath::PeerRoleFromLiveCall(const std::string& call_id) const {
  const LiveCall* call = host_.LiveCallOf(call_id);
  if (!call || !call->IsOpen()) {
    return std::nullopt;
  }
  auto peer = call->SolePeer();
  if (!peer) {
    return std::nullopt;
  }
  return CallPeerRole{*peer, call->Origin() == LiveCallOrigin::Placed};
}

} // namespace pbr
