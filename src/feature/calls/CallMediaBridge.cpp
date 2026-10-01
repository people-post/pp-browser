#include "feature/calls/CallMediaBridge.h"
#include "domain/messaging/CallTxOnlyEscalateLogic.h"

#include "foundation/i18n/LocalizationService.h"
#include "domain/messaging/CallHopAttachLogic.h"
#include "domain/mesh/l4/call_media/CallMediaFrameCrypto.h"
#include "foundation/runtime/AppRuntime.h"
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
  static logging::Logger logger = logging::getLogger("CallMediaBridge");
  return logger;
}

} // namespace

CallMediaBridge::CallMediaBridge(CallMediaHost& host, CallSessionStore& sessions,
                                             CallMediaKeyStore& media_keys, CallMediaEngine& media,
                                             ICallMediaTransport& direct, IDialRegistry* dial,
                                             ICircuitHopReach* circuit_reach)
    : host_(host), sessions_(sessions), media_keys_(media_keys), media_(media), direct_(direct),
      reach_(dial, circuit_reach), connect_(direct, reach_),
      media_key_inbox_poll_rounds_(kMediaKeyInboxPollRounds) {
  redirectLogger("CallMediaBridge");

  connect_.SetInboundPorts(MakeInboundPorts());
}

CallMediaInboundPorts CallMediaBridge::MakeInboundPorts() {
  // Calls owner (the connect sequence's inbound hello event).
  CallMediaInboundPorts ports;
  ports.session_open = [this](const std::string& call_id) {
    auto session = sessions_.LoadSession(call_id);
    return session && session->has_value() && (*session)->state != CallSessionState::Ended;
  };
  ports.load_key = [this](const std::string& call_id, const uint32_t epoch) -> std::optional<ByteVector> {
    if (auto key = media_keys_.LoadEpochKey(call_id, epoch); key && key->has_value()) {
      return **key;
    }
    return std::nullopt;
  };
  // Offerer often dials before the relay delivers CallMediaKey — keep inbox sync running.
  ports.request_key = [this](const std::string& /*call_id*/) { host_.P2pRequestInboxSync(); };
  ports.on_accepted = [this](const CallMediaInboundHello& hello) {
    // On the owner, before the bundle's callbacks exist: bind the stream now. Media is read on the
    // transport's I/O, so the first frame must already see this call's stream (never the last one's).
    BindInboundPeer(hello.call_id, hello.peer_id);
    return MakeBundleCallbacks(hello.call_id, /*fixed_stream=*/0, "Inbound call-media");
  };
  return ports;
}

void CallMediaBridge::BindInboundPeer(const std::string& call_id, const std::string& inbound_peer_id) {
  // Prefer call-roster Account ID for PublisherStreamIdForIdentity. The hello's peer is the mesh
  // PeerId; hashing that yields a different stream_id than SoftMigrate (account:…). Never use
  // P2pPeerIdentityForCall as the mapping — with N≥2 remotes it returns an arbitrary peer
  // (dogfood: Moto PeerId → wrong person stream).
  std::string identity = inbound_peer_id;
  if (inbound_peer_id.rfind("account:", 0) != 0) {
    if (auto mapped = host_.RelayIdentityForMeshPeerId(call_id, inbound_peer_id);
        mapped && mapped->has_value() && !mapped->value().empty()) {
      identity = mapped->value();
    } else if (!media_peer_identity_.empty() && media_peer_identity_.rfind("account:", 0) == 0) {
      // Last resort for 1:1 before contacts hydrate — only when dialed peer is the sole remote.
      if (auto sole = host_.P2pPeerIdentityForCall(call_id);
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
    // B30: the hello may be the first sign the answerer accepted (the relay's Accept can lag).
    host_.P2pNoteInboundHello(call_id, identity, inbound_peer_id);
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

CallMediaDirectCallbacks CallMediaBridge::MakeBundleCallbacks(const std::string& call_id,
                                                              const uint32_t fixed_stream,
                                                              const char* label) {
  CallMediaDirectCallbacks cbs;
  // Edges: the transport calls these on its I/O thread — they only report.
  cbs.on_connected = [outbox = outbox_, call_id, label]() {
    outbox.Emit(direct_event::BundleConnected{call_id, label});
  };
  cbs.on_media = [gate = receive_, &media = media_, &host = host_, outbox = outbox_, call_id,
                 fixed_stream](uint8_t channel, uint32_t seq, uint8_t mark, const std::vector<uint8_t>& payload) {
    ReceiveDirectMedia(*gate, media, host, outbox, call_id, fixed_stream, channel, seq, mark, payload);
  };
  cbs.on_failed = [outbox = outbox_, call_id](const std::string& reason) {
    outbox.Emit(direct_event::BundleFailed{call_id, reason});
  };
  // k4: the call lost its last path — reconnect window (the offerer re-anchors).
  cbs.on_path_lost = [outbox = outbox_, call_id]() { outbox.Emit(direct_event::PathLost{call_id}); };
  // k3: the transport moved the call to another path; the path label (UI snapshot) follows.
  cbs.on_path_changed = [outbox = outbox_, call_id](CallMediaLinkKind kind) {
    outbox.Emit(direct_event::PathChanged{call_id, kind});
  };
  return cbs;
}

void CallMediaBridge::OnBundleFailed(const std::string& call_id, const std::string& reason) {
  if (media_.ActiveCallId() != call_id) {
    return;
  }
  // Ignore a late fail when another bundle already carries media.
  if (DirectMediaReady() && media_.IsConnected()) {
    return;
  }
  // SoftMigrate StartSfu swaps send to media_relay but leaves the 1:1 stream until
  // ReleaseDirectTransport; peer teardown must not flip ConnectFailed over live SFU.
  // IsSfuMode() is also true for 1:1 capture — require media_relay attach.
  CallMediaCoordinator* call_media = LiveCallMedia(call_id);
  if (call_media && call_media->HopAttached() && media_.IsConnected()) {
    log().info << "Ignoring call-media fail after SoftMigrate/SFU call_id=" << call_id << " reason=" << reason;
    direct_.Detach();
    ClearMeshConnectFailed();
    if (arming_.on_connected) {
      arming_.on_connected(call_id);
    }
    host_.P2pNotifyRingChanged();
    return;
  }
  // PreferLocal ReleaseDirect closes 1:1 while capture stays up; CallSfuAttach may still be in
  // flight (dogfood: Moto ConnectFailed when attach lagged ReleaseDirect).
  const bool soft_direct_close =
      reason.find("read_eof") != std::string::npos || reason.find("stream closed") != std::string::npos;
  if ((call_media && (call_media->HopInFlight() || call_media->ExpectsHop())) ||
      (soft_direct_close && media_.IsActive() && media_.IsConnected())) {
    log().info << "Ignoring call-media fail while awaiting SFU attach call_id=" << call_id
               << " reason=" << reason;
    if (call_media) {
      call_media->ExpectHopAttach();
    }
    direct_.Detach();
    ClearMeshConnectFailed();
    host_.P2pRequestInboxSync();
    if (arming_.on_connected) {
      arming_.on_connected(call_id);
    }
    host_.P2pNotifyRingChanged();
    return;
  }
  log().warning << "Call-media failed call_id=" << call_id << " reason=" << reason;
  SurfaceConnectFailed(call_id, reason, /*stop_media=*/true);
}

CallMediaBridge::~CallMediaBridge() {
  receive_->open.store(false, std::memory_order_release);  // frames still in flight on I/O drop
  media_.SetOnStateChanged({});  // installed by StartDirectEngine; the engine outlives the bridge
}

void CallMediaBridge::SetReachDeps(IDialRegistry* dial, ICircuitHopReach* circuit_reach) {
  reach_.SetDeps(dial, circuit_reach);
}

void CallMediaBridge::SetSeedWarm(std::function<void()> warm) {
  seed_warm_ = std::move(warm);
}

void CallMediaBridge::SetSeedReserve(std::function<void()> reserve) {
  seed_reserve_ = std::move(reserve);
}

void CallMediaBridge::SetSeedParkAwait(PeerReachCoordinator::SeedParkAwait park) {
  // Remember the outcome so the UI can say why a connect failed (no seed reachable, e.g. a VPN
  // that drops UDP) — the reach coordinator itself keeps nothing across attempts.
  reach_.SetSeedParkAwait([flag = mesh_connect_seed_unreachable_, park = std::move(park)](
                              std::function<void(bool)> done, int timeout_ms) {
    park(
        [flag, done = std::move(done)](bool parked) {
          flag->store(!parked, std::memory_order_relaxed);
          done(parked);
        },
        timeout_ms);
  });
}

std::string CallMediaBridge::MediaPathKind() const {
  // The bound link is the truth: an answerer's inbound leg can ride a relay carrier while the
  // reach loop (and the dialer-only hop registry) think "punched" (dogfood 2026-09-24).
  if (direct_.IsActive() && direct_.ActiveLinkKind() == CallMediaLinkKind::Relayed) {
    return "circuit";
  }
  // A direct bound link wins over a relay hop still registered for the peer: after a k3
  // migration the relay stays the call's fallback, not its path.
  const bool bound_direct = direct_.IsActive() && direct_.ActiveLinkKind() == CallMediaLinkKind::Direct;
  if (bound_direct && reach_kind_ == PeerLinkKind::Relayed) {
    // Same rule the other way: the reach loop settled over the circuit while the call is bound on a
    // direct (punched) link the peer's hello opened — that call is not relayed (hard-lab FLIP race:
    // "circuit" skipped the relay standby and ran a pointless direct upgrade).
    return "punched";
  }
  if (!bound_direct && reach_.HasRelayHop(media_peer_identity_)) {
    return "circuit";
  }
  switch (reach_kind_) {
  case PeerLinkKind::Direct:
    return "direct";
  case PeerLinkKind::Punched:
    return "punched";
  case PeerLinkKind::Relayed:
    return "circuit";
  case PeerLinkKind::Unknown:
    break;
  }
  return {};
}

bool CallMediaBridge::DirectMediaReady() const {
  return direct_.Phase() == CallMediaSessionPhase::MediaReady;
}

bool CallMediaBridge::HasActiveDirectStream() const {
  return direct_.IsActive();
}

void CallMediaBridge::SetDirectArmingPorts(CallDirectArmingPorts ports) {
  arming_ = std::move(ports);
}

void CallMediaBridge::SetMediaKeyInboxPollRoundsForTest(const int rounds) {
  media_key_inbox_poll_rounds_ = rounds < 0 ? 0 : rounds;
}

void CallMediaBridge::SetDialWaitBudgetMsForTest(const int budget_ms) {
  reach_.SetDialBudgetMsForTest(budget_ms);
}

void CallMediaBridge::SetConnectAttemptTimeoutMsForTest(const int timeout_ms) {
  connect_.SetAttemptTimeoutMsForTest(timeout_ms);
}

void CallMediaBridge::SetSeatPorts(CallDirectSeatPorts ports) {
  seat_ = std::move(ports);
}

CallDirectPlannerApplyContext CallMediaBridge::BuildDirectPlannerContext(
    const std::string& call_id, const std::string& peer_identity) const {
  CallDirectPlannerApplyContext ctx;
  ctx.allows_direct_path = !arming_.IsBound() || (arming_.direct_ops_allowed && arming_.direct_ops_allowed());
  ctx.stopping = stopping_.load(std::memory_order_acquire);
  ctx.peer_nonempty = !peer_identity.empty();
  if (!call_id.empty() && media_.IsActive() && media_.ActiveCallId() == call_id &&
      (DirectMediaReady() || media_.IsConnected())) {
    ctx.media_live_same_call = true;
  }
  if (!call_id.empty()) {
    if (auto key = LoadActiveMediaKey(call_id); key) {
      ctx.has_media_key = true;
    }
  }
  return ctx;
}

void CallMediaBridge::SetDirectPlannerPhase(const CallDirectPlannerPhase next,
                                            const CallDirectPlannerEvent ev,
                                            const std::string& call_id) {
  const CallDirectPlannerPhase prev = direct_planner_phase_;
  direct_planner_phase_ = next;
  log().info << "planner=Direct phase=" << CallDirectPlannerPhaseName(prev) << "->"
             << CallDirectPlannerPhaseName(next) << " event=" << CallDirectPlannerEventName(ev)
             << " call_id=" << call_id
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

void CallMediaBridge::Apply(CallDirectPlannerEvent ev, const std::string& call_id,
                            const std::string& peer_identity) {
  const CallDirectPlannerApplyContext ctx = BuildDirectPlannerContext(call_id, peer_identity);
  const CallDirectPlannerPhaseOutcome out =
      DecideCallDirectPlannerPhase(direct_planner_phase_, ev, ctx);
  if (out.decision == CallDirectPlannerDecision::Ignore) {
    log().info << "planner=Direct ignore event=" << CallDirectPlannerEventName(ev)
               << " phase=" << CallDirectPlannerPhaseName(direct_planner_phase_)
               << " call_id=" << call_id << " allows_direct=" << (ctx.allows_direct_path ? 1 : 0);
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
      const std::string cid = call_id.empty() ? media_.ActiveCallId() : call_id;
      if (peer.empty() && !cid.empty()) {
        if (auto resolved = host_.P2pPeerIdentityForCall(cid); resolved && resolved->has_value()) {
          peer = **resolved;
        }
      }
      if (!cid.empty() && !peer.empty()) {
        if (arming_.report_progress) {
          arming_.report_progress(CallDirectPlannerPhase::DegradedTxOnly, cid);
        }
        EscalateTxOnlyViaCircuit(cid, peer);
      }
    }
    break;
  case CallDirectPlannerEvent::CircuitEscalated:
    break;
  case CallDirectPlannerEvent::KeyTimeout:
  case CallDirectPlannerEvent::ConnectFailed:
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
      if (arming_.on_connected) {
        arming_.on_connected(call_id);
      }
    }
    break;
  case CallDirectPlannerEvent::PathLost:
    if (out.decision == CallDirectPlannerDecision::Transition) {
      if (arming_.report_progress) {
        arming_.report_progress(CallDirectPlannerPhase::Reconnecting, call_id);
      }
      host_.P2pNotifyRingChanged();
      // The offerer reaches the peer again and moves the call onto that link; the answerer's
      // transport accepts the migrate (invite / accept is the agreement, as for the first connect).
      if (session_offerer_) {
        ScheduleReanchor(call_id, std::chrono::milliseconds(0));
      }
    }
    break;
  }
}

void CallMediaBridge::SetOutbox(OwnerOutbox<DirectPathEvent> outbox) {
  outbox_ = std::move(outbox);
  connect_.SetOutbox(outbox_.For<ConnectEvent>(
      [](ConnectEvent event) { return DirectPathEvent{direct_event::ForConnect{std::move(event)}}; }));
}

void CallMediaBridge::Handle(DirectPathEvent& event) {
  std::visit(
      [this](auto& e) {
        using E = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<E, direct_event::HealthTick>) {
          if (direct_health_timer_id_ == 0) {
            return;  // cancelled since
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
          if (reanchor_timer_id_ != 0 && reanchor_call_id_ == e.call_id) {
            reanchor_timer_id_ = 0;
            Reanchor(e.call_id);
          }
        } else if constexpr (std::is_same_v<E, direct_event::RecoveryGraceOver>) {
          FailUnlessDirectRecovered(e.call_id, e.error, /*grace_used=*/true);
        } else if constexpr (std::is_same_v<E, direct_event::BundleConnected>) {
          log().info << e.label << " connected call_id=" << e.call_id;
          CommitDirectConnected(e.call_id);
        } else if constexpr (std::is_same_v<E, direct_event::BundleFailed>) {
          OnBundleFailed(e.call_id, e.reason);
        } else if constexpr (std::is_same_v<E, direct_event::PathLost>) {
          Apply(CallDirectPlannerEvent::PathLost, e.call_id, media_peer_identity_);
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

void CallMediaBridge::OnPathChanged(const std::string& call_id, const CallMediaLinkKind kind) {
  log().info << "call-media path migrated call_id=" << call_id
             << " path=" << (kind == CallMediaLinkKind::Relayed ? "circuit" : "direct");
  if (kind == CallMediaLinkKind::Direct) {
    CancelDirectUpgrade();
    ArmRelayStandby(call_id);  // after a failover onto a direct standby, keep a relay behind it
    // Off the relay onto a link the upgrade punch (or the peer's punch) opened.
    if (reach_kind_ == PeerLinkKind::Relayed || reach_kind_ == PeerLinkKind::Unknown) {
      reach_kind_ = PeerLinkKind::Punched;
    }
  } else {
    reach_kind_ = PeerLinkKind::Relayed;
  }
  Apply(CallDirectPlannerEvent::PathMigrated, call_id);
}

void CallMediaBridge::CancelDirectHealthTimer() {
  outbox_.Cancel(direct_health_timer_id_);
}

void CallMediaBridge::ArmDirectHealthTimer() {
  CancelDirectHealthTimer();
  // ~1s tick while Connecting/Live/Degraded — primary connect health / TX-only path (no UI poll).
  direct_health_timer_id_ = outbox_.After(std::chrono::milliseconds(1000), direct_event::HealthTick{});
}

void CallMediaBridge::CancelReserveRenewal() {
  outbox_.Cancel(reserve_renew_timer_id_);
}

void CallMediaBridge::ArmReserveRenewal() {
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

void CallMediaBridge::OnReserveRenewFire() {
  if (stopping_.load(std::memory_order_acquire) || media_call_id_.empty()) {
    CancelReserveRenewal();
    return;
  }
  log().info << "circuit reserve renew call_id=" << media_call_id_;
  if (seed_reserve_) {
    seed_reserve_();
  }
}

// --- k3: a relayed call keeps trying for a direct path; the transport moves onto it by itself ------

void CallMediaBridge::ArmDirectUpgrade(const std::string& call_id) {
  // The migration driver (offerer) punches; the answerer's transport follows the migrate.
  if (!session_offerer_ || call_id.empty() || MediaPathKind() != "circuit" || upgrade_call_id_ == call_id) {
    return;
  }
  if (!PathPolicyFor(call_id).upgrade_to_direct) {
    log().info << "direct upgrade: not for this pair (a mobile end anchors on the relay) call_id=" << call_id;
    return;
  }
  CancelDirectUpgrade();
  upgrade_call_id_ = call_id;
  upgrade_attempt_ = 0;
  ScheduleDirectUpgrade();
}

void CallMediaBridge::ScheduleDirectUpgrade() {
  static constexpr int kDelaysMs[] = {3000, 20000, 60000};
  if (upgrade_attempt_ >= static_cast<int>(std::size(kDelaysMs))) {
    log().info << "direct upgrade: giving up call_id=" << upgrade_call_id_ << " (stays on the relay)";
    return;
  }
  const int delay_ms = upgrade_delay_ms_for_test_ > 0 ? upgrade_delay_ms_for_test_ : kDelaysMs[upgrade_attempt_];
  upgrade_timer_id_ = outbox_.After(std::chrono::milliseconds(delay_ms), direct_event::UpgradeDue{});
}

void CallMediaBridge::CancelDirectUpgrade() {
  outbox_.Cancel(upgrade_timer_id_);
  upgrade_call_id_.clear();
}

void CallMediaBridge::OnDirectUpgradeFire() {
  upgrade_timer_id_ = 0;
  const std::string call_id = upgrade_call_id_;
  if (stopping_.load(std::memory_order_acquire) || call_id.empty() || call_id != media_call_id_ ||
      direct_planner_phase_ != CallDirectPlannerPhase::Live || MediaPathKind() != "circuit") {
    CancelDirectUpgrade();
    return;
  }
  ++upgrade_attempt_;
  const std::string peer_id = CallPeerMeshId();
  log().info << "direct upgrade attempt=" << upgrade_attempt_ << " call_id=" << call_id << " peer=" << peer_id;
  reach_.UpgradeToDirect(peer_id, OnOwner<Roe<void>>([this, call_id](Roe<void> result) {
    if (upgrade_call_id_ != call_id) {
      return;
    }
    if (result) {
      log().info << "direct upgrade: direct link up call_id=" << call_id << " (the call moves onto it)";
      return;  // PathMigrated cancels the schedule; a failed migration retries from the transport
    }
    log().info << "direct upgrade miss call_id=" << call_id << " err=" << result.error().message;
    ScheduleDirectUpgrade();
  }));
}

// --- k6: a direct call keeps a relayed standby (K003) -----------------------------------------------

void CallMediaBridge::ArmRelayStandby(const std::string& call_id) {
  if (!session_offerer_ || call_id.empty() || MediaPathKind() == "circuit" || standby_call_id_ == call_id ||
      !reach_.HasCircuitReach()) {
    return;
  }
  if (!PathPolicyFor(call_id).want_relay_standby) {
    return;
  }
  CancelRelayStandby();
  standby_call_id_ = call_id;
  standby_attempt_ = 0;
  ScheduleRelayStandby();
}

void CallMediaBridge::ScheduleRelayStandby() {
  static constexpr int kDelaysMs[] = {5000, 20000, 60000};
  if (standby_attempt_ >= static_cast<int>(std::size(kDelaysMs))) {
    log().info << "relay standby: giving up call_id=" << standby_call_id_ << " (a lost path re-anchors)";
    return;
  }
  const int delay_ms = standby_delay_ms_for_test_ > 0 ? standby_delay_ms_for_test_ : kDelaysMs[standby_attempt_];
  standby_timer_id_ = outbox_.After(std::chrono::milliseconds(delay_ms), direct_event::StandbyDue{});
}

void CallMediaBridge::CancelRelayStandby() {
  outbox_.Cancel(standby_timer_id_);
  if (standby_reach_id_ != 0) {
    reach_.Cancel(standby_reach_id_);
    standby_reach_id_ = 0;
  }
  standby_call_id_.clear();
}

void CallMediaBridge::OnRelayStandbyFire() {
  standby_timer_id_ = 0;
  const std::string call_id = standby_call_id_;
  const bool still_wanted = !stopping_.load(std::memory_order_acquire) && !call_id.empty() &&
                            call_id == media_call_id_ && direct_planner_phase_ == CallDirectPlannerPhase::Live &&
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
  PeerReachRequest request;
  request.keys.push_back(peer_id);
  request.mode = PeerReachMode::Reach;
  request.exclude_direct = true;  // a circuit, beside the direct link the call is on
  const CallStandbyPriority priority = StandbyPriorityFor(PathPolicyFor(call_id), MediaPathKind() == "punched");
  request.circuit_standby_priority = priority == CallStandbyPriority::High     ? CircuitStandbyPriority::High
                                     : priority == CallStandbyPriority::Medium ? CircuitStandbyPriority::Medium
                                                                               : CircuitStandbyPriority::Low;
  log().info << "relay standby attempt=" << standby_attempt_ << " call_id=" << call_id << " peer=" << peer_id
             << " priority=" << CallStandbyPriorityName(priority);
  const auto retry = [this, call_id]() {
    if (standby_call_id_ == call_id) {
      ScheduleRelayStandby();
    }
  };
  standby_reach_id_ = reach_.Ensure(
      std::move(request), OnOwner<Roe<PeerReachResult>>([this, call_id, retry](Roe<PeerReachResult> reached) {
        if (standby_call_id_ != call_id) {
          return;
        }
        standby_reach_id_ = 0;
        if (!reached) {
          log().info << "relay standby: no circuit (" << reached.error().message << ")";
          retry();
          return;
        }
        direct_.AddStandby(CallMediaLinkKind::Relayed, OnOwner<Roe<void>>([this, call_id, retry](Roe<void> added) {
          if (standby_call_id_ != call_id) {
            return;
          }
          if (!added) {
            log().info << "relay standby: not added (" << added.error().message << ")";
            retry();
            return;
          }
          log().info << "relay standby up call_id=" << call_id;
          standby_call_id_.clear();
        }));
      }));
}

void CallMediaBridge::ApplyPathPolicyToTransport(const std::string& call_id) {
  direct_.SetAutoMigrateToDirect(PathPolicyFor(call_id).upgrade_to_direct);
}

// --- k6: the pair's path policy changed (a mobility class flipped mid-call) ----------------------

void CallMediaBridge::OnPathPolicyChanged(const std::string& call_id) {
  if (stopping_.load(std::memory_order_acquire) || call_id.empty() || call_id != media_call_id_) {
    return;
  }
  const CallPathPolicy policy = PathPolicyFor(call_id);
  log().info << "path policy call_id=" << call_id << " upgrade=" << (policy.upgrade_to_direct ? 1 : 0)
             << " relay=" << CallRelayRoleName(policy.relay_role);
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

void CallMediaBridge::OnLocalNetworkChanged() {
  const std::string call_id = media_call_id_;
  if (stopping_.load(std::memory_order_acquire) || call_id.empty()) {
    return;
  }
  if (direct_planner_phase_ == CallDirectPlannerPhase::Reconnecting) {
    // The retry backoff may be long; the new network is the best chance of reaching the peer.
    log().info << "network changed: re-anchoring after the links settle call_id=" << call_id;
    ScheduleReanchor(call_id, std::chrono::milliseconds(network_settle_ms_));
    return;
  }
  if (direct_planner_phase_ == CallDirectPlannerPhase::Live && MediaPathKind() == "circuit" && session_offerer_) {
    log().info << "network changed: direct upgrade starts over call_id=" << call_id;
    CancelDirectUpgrade();
    ArmDirectUpgrade(call_id);
  }
  // Live on a direct path: nothing to do here — if it died with the old network, Amp evicts its
  // link and the transport fails over to the standby or reports the path lost.
}

// --- k4: re-anchor a call that lost its last path ----------------------------------------------

void CallMediaBridge::ScheduleReanchor(const std::string& call_id, const std::chrono::milliseconds delay) {
  CancelReanchor();
  reanchor_call_id_ = call_id;
  reanchor_timer_id_ = outbox_.After(delay, direct_event::ReanchorDue{call_id});
}

void CallMediaBridge::CancelReanchor() {
  outbox_.Cancel(reanchor_timer_id_);
  if (reanchor_reach_id_ != 0) {
    reach_.Cancel(reanchor_reach_id_);
    reanchor_reach_id_ = 0;
  }
  reanchor_call_id_.clear();
}

void CallMediaBridge::Reanchor(const std::string& call_id) {
  if (stopping_.load() || media_.ActiveCallId() != call_id ||
      direct_planner_phase_ != CallDirectPlannerPhase::Reconnecting) {
    return;
  }
  const std::string peer_id = CallPeerMeshId();
  PeerReachRequest request;
  request.keys.push_back(peer_id);
  request.mode = PeerReachMode::Reach;
  log().info << "reconnect: reaching the peer again call_id=" << call_id << " peer=" << peer_id;
  const auto retry = [this, call_id]() {
    if (direct_planner_phase_ == CallDirectPlannerPhase::Reconnecting && reanchor_call_id_ == call_id) {
      ScheduleReanchor(call_id, std::chrono::milliseconds(reanchor_retry_ms_));
    }
  };
  reanchor_reach_id_ = reach_.Ensure(
      std::move(request), OnOwner<Roe<PeerReachResult>>([this, call_id, retry](Roe<PeerReachResult> reached) {
        if (reanchor_call_id_ != call_id) {
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
            reached->kind == PeerLinkKind::Relayed ? CallMediaLinkKind::Relayed : CallMediaLinkKind::Direct;
        direct_.MigrateTo(kind, OnOwner<Roe<void>>([this, call_id, retry](Roe<void> moved) {
          if (reanchor_call_id_ != call_id) {
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

void CallMediaBridge::CancelEscalateReach() {
  if (escalate_reach_id_ != 0) {
    reach_.Cancel(escalate_reach_id_);
    escalate_reach_id_ = 0;
  }
}

void CallMediaBridge::OnDirectHealthTimerFire() {
  if (direct_planner_phase_ == CallDirectPlannerPhase::Idle ||
      direct_planner_phase_ == CallDirectPlannerPhase::KeyWait ||
      direct_planner_phase_ == CallDirectPlannerPhase::Stopping) {
    CancelDirectHealthTimer();
    return;
  }
  PollMeshConnectHealth();
}

void CallMediaBridge::CommitDirectConnected(const std::string& call_id) {
  if (call_id.empty()) {
    return;
  }
  Apply(CallDirectPlannerEvent::ConnectSucceeded, call_id, media_peer_identity_);
  if (direct_planner_phase_ != CallDirectPlannerPhase::Live &&
      direct_planner_phase_ != CallDirectPlannerPhase::DegradedTxOnly) {
    // Connected after this side's connect failed: the call may still be open (failed ≠ closed) —
    // the lifecycle resumes media over this stream if so; after Leave it ignores it.
    if (mesh_connect_failed_ && failed_open_ && failed_open_->call_id == call_id && !failed_open_->resume_requested &&
        DirectMediaReady() && arming_.on_peer_reconnected) {
      log().info << "call-media connected after connect failed call_id=" << call_id << " — resume?";
      failed_open_->resume_requested = true;
      arming_.on_peer_reconnected(call_id);
    }
    return;
  }
  // Capture may lag the stream (inbound before BeginSession). Still advance phase so chrome
  // can leave Calling/Connecting once StartSfu runs; keep_inbound / on_connected re-enter.
  if (media_.IsActive() && media_.ActiveCallId() == call_id) {
    media_.SetConnectionState("connected");
  }
  ClearMeshConnectFailed();
  if (direct_connected_at_ms_ <= 0) {
    direct_connected_at_ms_ = util::NowUnixMs();
  }
  // V036 Phase 2: seat Live is the chrome Connected gate — DirectConnected alone is signaling.
  // Prefer Live only when the direct stream is actually up (not StartSfu alone).
  if (seat_.IsBound() && media_.IsActive() && media_.ActiveCallId() == call_id &&
      (DirectMediaReady() || HopAttachedFor(call_id))) {
    seat_.note_live(call_id);
  }
  if (arming_.on_connected) {
    arming_.on_connected(call_id);
  }
  ApplyPathPolicyToTransport(call_id);
  ArmDirectUpgrade(call_id);
  ArmRelayStandby(call_id);
  host_.P2pNotifyRingChanged();
}

void CallMediaBridge::ReceiveDirectMedia(ReceiveGate& gate, CallMediaEngine& media, const CallMediaHost& host,
                                         const OwnerOutbox<DirectPathEvent>& outbox, const std::string& call_id,
                                         const uint32_t fixed_stream, const uint8_t channel, const uint32_t seq,
                                         const uint8_t mark, const std::vector<uint8_t>& payload) {
  // The data plane stays on I/O, like the hop's frames: only atomics and the engine's thread-safe
  // surface here. Owner state it needs (the bound stream) is published into the gate.
  if (!gate.open.load(std::memory_order_acquire) || !media.IsActive() || media.ActiveCallId() != call_id ||
      host.HopCarriesMedia()) {
    return;
  }
  // Outbound bundles know the dialed identity; inbound ones use the bound (or deferred) stream.
  const uint32_t remote_stream =
      fixed_stream != 0 ? fixed_stream : gate.remote_stream.load(std::memory_order_acquire);
  if (remote_stream == 0) {
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    int64_t not_before = gate.rebind_not_before_ms.load(std::memory_order_acquire);
    if (now >= not_before && gate.rebind_not_before_ms.compare_exchange_strong(not_before, now + 250)) {
      outbox.Emit(direct_event::RebindInboundStream{call_id});
    }
    const uint32_t n = g_inbound_unmapped_audio_drops.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || (n % 50) == 0) {
      DirectReceiveLog().warning << "Inbound call-media drop: stream not bound yet call_id=" << call_id << " drops=" << n;
    }
    return;
  }
  CallMediaEngine::SfuPacket pkt;
  pkt.stream_id = remote_stream;
  pkt.channel_id = channel;
  pkt.seq = seq;
  pkt.mark = mark;
  pkt.payload = payload;
  media.OnSfuPacket(pkt);
}

void CallMediaBridge::RebindInboundStream(const std::string& call_id) {
  if (receive_->remote_stream.load(std::memory_order_acquire) != 0 || media_.ActiveCallId() != call_id) {
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
    return;  // I/O asks again after its backoff
  }
  const uint32_t stream = PublisherStreamIdForIdentity(account);
  receive_->remote_stream.store(stream, std::memory_order_release);
  log().info << "Inbound call-media rebound stream_id=" << stream << " account=" << account << " call_id=" << call_id;
}

bool CallMediaBridge::IsMeshConnectFailed() const {
  return mesh_connect_failed_;
}

bool CallMediaBridge::MeshConnectMissingMic() const {
  return mesh_connect_missing_mic_;
}

bool CallMediaBridge::MeshConnectSeedUnreachable() const {
  return mesh_connect_seed_unreachable_->load(std::memory_order_relaxed);
}

void CallMediaBridge::ClearMeshConnectFailed() {
  mesh_connect_failed_ = false;
  mesh_connect_missing_mic_ = false;
  mesh_connect_seed_unreachable_->store(false, std::memory_order_relaxed);
}

void CallMediaBridge::PollMeshConnectHealth() {
  if (mesh_connect_failed_ || !media_.IsActive() || !media_.IsSfuMode()) {
    return;
  }
  if (CallMediaCoordinator* call_media = LiveCallMedia(media_.ActiveCallId()); call_media && call_media->HopInFlight()) {
    return;
  }
  // ConnectOffererWithRetry runs on a worker thread — do not UI-timeout while it is dialing.
  if (connect_.InFlight()) {
    return;
  }
  if (media_.IsConnected() && DirectMediaReady()) {
    ClearMeshConnectFailed();
    MaybeEscalateTxOnlyDirect();
    return;
  }
  const std::string call_id = media_.ActiveCallId();
  if (call_id.empty()) {
    return;
  }
  // Stream can be up while StartSfu→"connecting" left IsConnected false (chrome stuck).
  if (DirectMediaReady()) {
    ClearMeshConnectFailed();
    if (media_.IsActive() && !media_.IsConnected()) {
      log().info << "Heal call-media connected from direct stream call_id=" << call_id;
      CommitDirectConnected(call_id);
    }
    MaybeEscalateTxOnlyDirect();
    return;
  }
  auto joined = sessions_.CountJoined(call_id);
  if (joined && *joined >= 3) {
    return;
  }
  const int64_t started = media_.StartedAtMs();
  if (started <= 0) {
    return;
  }
  if (util::NowUnixMs() - started < kMeshConnectTimeoutMs) {
    return;
  }
  log().warning << "Mesh connect timeout call_id=" << call_id;
  mesh_connect_missing_mic_ = !media_.HasLocalCapture();
  SurfaceConnectFailed(call_id, "mesh connect timeout", /*stop_media=*/true);
}

void CallMediaBridge::MaybeEscalateTxOnlyDirect() {
  std::string peer = media_peer_identity_;
  const std::string call_id = media_.ActiveCallId();
  if (peer.empty() && !call_id.empty()) {
    if (auto resolved = host_.P2pPeerIdentityForCall(call_id); resolved && resolved->has_value()) {
      peer = **resolved;
    }
  }
  const auto snap = media_.HealthSnapshot();
  CallTxOnlyEscalateDecisionInput in;
  in.already_done = tx_only_escalation_done_;
  in.sfu_attached = HopAttachedFor(media_.ActiveCallId());
  in.stopping = stopping_.load();
  // Bound-link truth: never "escalate via circuit" when media already rides a relay carrier.
  in.media_path_kind = MediaPathKind();
  in.has_circuit_reach = reach_.HasCircuitReach();
  in.direct_active = direct_.IsActive();
  in.active_call_id = call_id;
  in.media_call_id = media_call_id_;
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

void CallMediaBridge::EscalateTxOnlyViaCircuit(const std::string& call_id, const std::string& peer) {
  // k3-4 make-before-break: the call keeps running on its path while a circuit to the peer is
  // built, then moves onto it. Only if that fails does the old break-before-make restart run.
  const std::string peer_id = CallPeerMeshId();
  if (peer_id.empty() || !reach_.HasCircuitReach()) {
    EscalateBreakBeforeMake(call_id, peer);
    return;
  }
  PeerReachRequest request;
  request.keys.push_back(peer_id);
  request.mode = session_offerer_ ? PeerReachMode::Reach : PeerReachMode::Await;
  request.exclude_direct = true;
  log().info << "TX-only escalate: building a circuit under the live call call_id=" << call_id << " peer=" << peer_id;
  escalate_reach_id_ = reach_.Ensure(
      std::move(request), OnOwner<Roe<PeerReachResult>>([this, call_id, peer](Roe<PeerReachResult> reached) {
      escalate_reach_id_ = 0;
      if (stopping_.load() || media_.ActiveCallId() != call_id ||
          direct_planner_phase_ != CallDirectPlannerPhase::DegradedTxOnly) {
        return;
      }
      if (!reached) {
        log().warning << "TX-only escalate: circuit failed (" << reached.error().message << ") — restarting via circuit";
        EscalateBreakBeforeMake(call_id, peer);
        return;
      }
      direct_.MigrateTo(CallMediaLinkKind::Relayed, OnOwner<Roe<void>>([this, call_id, peer](Roe<void> moved) {
        if (stopping_.load() || media_.ActiveCallId() != call_id) {
          return;
        }
        if (moved) {
          log().info << "TX-only escalate: call moved onto the circuit call_id=" << call_id;
          return;  // on_path_changed → PathMigrated brings the planner back to Live
        }
        if (direct_planner_phase_ == CallDirectPlannerPhase::DegradedTxOnly) {
          log().warning << "TX-only escalate: migration failed (" << moved.error().message
                        << ") — restarting via circuit";
          EscalateBreakBeforeMake(call_id, peer);
        }
      }));
    }));
}

void CallMediaBridge::EscalateBreakBeforeMake(const std::string& call_id, const std::string& peer) {
  Apply(CallDirectPlannerEvent::CircuitEscalated, call_id, peer);
  if (seat_.IsBound()) {
    seat_.note_connecting(call_id);
  }
  media_.SetConnectionState("connecting");
  reach_kind_ = PeerLinkKind::Unknown;
  direct_connected_at_ms_ = 0;
  reach_.ForgetPath(peer);
  // Re-open transport under circuit without full engine Stop (keep capture).
  AbortConnectSequence();
  force_circuit_ensure_ = true;  // consumed by the next reach (BuildReachRequest)
  direct_.Detach();
  outbox_.Emit(direct_event::EscalateRestart{call_id, peer});
}

void CallMediaBridge::RestartAfterEscalate(const std::string& call_id, const std::string& peer) {
  if (stopping_.load() || media_.ActiveCallId() != call_id) {
    return;
  }
  log().info << "TX-only escalate BeginSession role=" << (session_offerer_ ? "offerer" : "answerer")
             << " call_id=" << call_id;
  if (auto started = BeginSession(call_id, peer, session_offerer_); !started) {
    log().warning << "TX-only escalate BeginSession failed: " << started.error().message;
    FailUnlessDirectRecovered(call_id, started.error().message);
  }
}

bool CallMediaBridge::ShouldUseMeshForPeer(const std::string& /*peer_identity*/) const {
  return reach_.Available();
}

void CallMediaBridge::AbortConnectSequence() {
  connect_generation_.fetch_add(1, std::memory_order_acq_rel);
  connect_.Abort();
}

void CallMediaBridge::FailUnlessDirectRecovered(const std::string& call_id, const std::string& err,
                                                const bool grace_used) {
  if (stopping_.load()) {
    return;
  }
  if (grace_used && media_call_id_ != call_id) {
    return;  // the session moved on during the grace (stopped / another call)
  }
  const CallMediaSessionPhase phase = direct_.Phase();
  CallConnectFailureFacts facts;
  facts.direct_media_ready = DirectMediaReady();
  facts.inbound_in_progress =
      phase == CallMediaSessionPhase::HelloInbound || phase == CallMediaSessionPhase::Adopting;
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

void CallMediaBridge::SurfaceConnectFailed(const std::string& call_id, const std::string& err,
                                           const bool stop_media) {
  if (!err.empty()) {
    host_.P2pSetLastMediaError(err);
  }
  // Stop late EnsureViaCircuit / StartBridge before chrome refresh (dogfood SIGSEGV after give-up).
  reach_.AbortCircuitAttempts();
  if (stop_media && (media_.IsActive() || media_.IsSfuMode())) {
    // StopMeshMedia clears mesh_connect_failed_ and the attempted mark for Leave hygiene — the call
    // is still open, so both are re-asserted below.
    StopMeshMedia(call_id);
  } else if (!media_peer_identity_.empty()) {
    reach_.AbandonDial(media_peer_identity_);
  }
  if (!call_id.empty()) {
    media_attempted_calls_.Insert(call_id);
    failed_open_ = FailedOpenCall{call_id};
  }
  if (seat_.note_failed) {
    seat_.note_failed(call_id);
  }
  Apply(CallDirectPlannerEvent::ConnectFailed, call_id, media_peer_identity_);
  if (arming_.on_connect_failed) {
    arming_.on_connect_failed(call_id);
  }
  mesh_connect_failed_ = true;
  host_.P2pNotifyRingChanged();
}

std::string CallMediaBridge::CallPeerMeshId() {
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

std::string CallMediaBridge::ReachPeerIdFor(const std::string& key) {
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

PeerReachRequest CallMediaBridge::BuildReachRequest(const CallMediaDirectConnectParams& params) {
  PeerReachRequest request;
  // Keep the account as an alias the mesh may know too (hard-lab / dogfood NAT: "endpoint not
  // registered").
  const std::string reach_key = ReachPeerIdFor(params.peer_key);
  request.keys.push_back(reach_key);
  if (reach_key != params.peer_key) {
    request.keys.push_back(params.peer_key);
  }
  // The offerer reaches; the answerer awaits the offerer's link (invite/accept is the agreement).
  request.mode = params.offerer ? PeerReachMode::Reach : PeerReachMode::Await;
  request.exclude_direct = std::exchange(force_circuit_ensure_, false);
  request.allow_punch = PathPolicyFor(params.call_id).punch_at_start;
  return request;
}

CallMediaConnectHooks CallMediaBridge::MakeConnectHooks(const std::string& call_id) {
  CallMediaConnectHooks hooks;
  hooks.before_attempt = [this](const CallMediaDirectConnectParams& params) {
    if (!params.offerer) {
      return;
    }
    // Answerer often defers waiting on the relay — resend the epoch key each attempt. MediaKey is
    // addressed by roster account:, not the mesh dial PeerId.
    const std::string key_peer =
        (!media_peer_identity_.empty() && media_peer_identity_.rfind("account:", 0) == 0)
            ? media_peer_identity_
            : params.peer_key;
    host_.P2pResendMediaKey(params.call_id, key_peer);
  };
  hooks.on_link_ready = [this](const PeerLinkKind kind) { reach_kind_ = kind; };
  hooks.on_finished = [this, call_id](Roe<void> result) {
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

Roe<ByteVector> CallMediaBridge::LoadActiveMediaKey(const std::string& call_id) const {
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

Roe<void> CallMediaBridge::BeginSession(const std::string& call_id, const std::string& peer_identity,
                                              bool offerer) {
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
  host_.P2pNotifyRingChanged();
  return {};
}

void CallMediaBridge::ResetDirectSessionState(const std::string& call_id, const std::string& peer_identity,
                                              bool offerer) {
  if (media_call_id_ != call_id) {
    tx_only_escalation_done_ = false;
  }
  failed_open_.reset();
  media_attempted_calls_.Insert(call_id);
  media_call_id_ = call_id;
  media_peer_identity_ = peer_identity;
  session_offerer_ = offerer;
  direct_connected_at_ms_ = 0;
  // Otherwise keep what an inbound hello of this session bound; stop / teardown clear it between calls.
  if (peer_identity.rfind("account:", 0) == 0) {
    receive_->remote_stream.store(PublisherStreamIdForIdentity(peer_identity), std::memory_order_release);
  }
  audio_seq_.store(0);
  ClearMeshConnectFailed();
  if (!offerer) {
    pending_answerer_call_id_.clear();
    ++key_wait_gen_;
    pending_answerer_peer_.clear();
  }
}

bool CallMediaBridge::StopPriorDirectAttempt(bool offerer) {
  // Answerer may already have accepted inbound hello (key landed first). Offerer must NOT Detach:
  // answerer-only dial often negotiates the stream before BeginSession runs on the offerer
  // (dogfood: Detach raced inbound → phone never read hello → "Failed to read call-media frame header").
  const bool keep_inbound = direct_.IsActive();
  const bool restarting = media_.IsActive() || connect_.InFlight();
  if (media_.IsActive()) {
    StopEngineFor(media_.ActiveCallId(), "prior direct attempt");
  }
  if (!offerer && !keep_inbound) {
    direct_.Detach();
  }
  // Abort in-flight Connect from a prior BeginSession (Kick thrash used to Stop without bumping).
  if (restarting && !keep_inbound) {
    AbortConnectSequence();
  }
  return keep_inbound;
}

void CallMediaBridge::ParkOnBootstrapSeed() {
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

Roe<void> CallMediaBridge::StartDirectEngine(const std::string& call_id) {
  media_.SetOnStateChanged([this](const std::string& state) {
    if (state == "connected") {
      ClearMeshConnectFailed();
    }
    host_.P2pNotifyRingChanged();
  });
  const uint64_t send_gen = connect_generation_.load(std::memory_order_acquire);
  CallMediaCoordinator* call_media = host_.P2pCallMedia(call_id);
  if (!call_media) {
    return Error("no live call for media " + call_id);
  }
  auto started = call_media->StartEngine(CallMediaSeat::PathKind::Direct, [this, send_gen](const CallMediaEngine::SfuPacket& pkt) {
    if (pkt.channel_id > kCallMediaChannelVideoLo) {
      return;
    }
    // SoftMigrate ReleaseDirectTransport bumps connect_generation_ before Detach.
    if (connect_generation_.load(std::memory_order_acquire) != send_gen) {
      return;
    }
    const uint32_t seq = pkt.channel_id == 0 ? (audio_seq_.fetch_add(1) + 1) : pkt.seq;
    (void)direct_.SendMedia(static_cast<uint8_t>(pkt.channel_id), pkt.payload, seq, pkt.mark);
  });
  if (!started) {
    return started;
  }
  // Duplex Live only after direct stream (CommitDirectConnected) — not StartSfu alone.
  if (seat_.IsBound() && !DirectMediaReady()) {
    seat_.note_connecting(call_id);
  }
  // StartSfu marks connected immediately for SFU capture; 1:1 chrome waits on the direct stream.
  if (!DirectMediaReady()) {
    media_.SetConnectionState("connecting");
  }
  return {};
}

void CallMediaBridge::StartDirectConnect(const std::string& call_id, const std::string& peer_identity, bool offerer,
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

Roe<void> CallMediaBridge::StartMediaAsOfferer(const std::string& call_id,
                                                     const std::string& peer_identity) {
  return BeginSession(call_id, peer_identity, true);
}

Roe<void> CallMediaBridge::StartMediaAsAnswerer(const std::string& call_id,
                                                      const std::string& peer_identity) {
  return BeginSession(call_id, peer_identity, false);
}

void CallMediaBridge::ScheduleDirectStart(const std::string& call_id, const std::string& peer_identity,
                                          const bool offerer) {
  if (offerer) {
    ScheduleStartMediaAsOfferer(call_id, peer_identity);
  } else {
    ScheduleStartMediaAsAnswerer(call_id, peer_identity);
  }
}

void CallMediaBridge::ScheduleStartMediaAsOfferer(const std::string& call_id,
                                                        const std::string& peer_identity) {
  // Mark before UI hop so CallController orphan auto-Leave cannot race CallAccept→Active.
  media_attempted_calls_.Insert(call_id);
  outbox_.Emit(direct_event::StartOfferer{call_id, peer_identity});
}

void CallMediaBridge::RunOffererStart(const std::string& call_id, const std::string& peer_identity) {
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
  if (media_.IsActive() && media_.ActiveCallId() == call_id) {
    log().info << "StartMediaAsOfferer skip: media already active call_id=" << call_id;
    return;
  }
  log().info << "StartMediaAsOfferer UI enter call_id=" << call_id << " peer=" << peer_identity;
  SetDirectPlannerPhase(CallDirectPlannerPhase::Connecting, CallDirectPlannerEvent::ScheduleOfferer,
                        call_id);
  if (auto started = StartMediaAsOfferer(call_id, peer_identity); !started) {
    log().warning << "StartMediaAsOfferer failed: " << started.error().message;
    SurfaceConnectFailed(call_id, started.error().message, /*stop_media=*/true);
  }
}

void CallMediaBridge::ScheduleStartMediaAsAnswerer(const std::string& call_id,
                                                         const std::string& peer_identity) {
  PBR_ASSERT_ON_OWNER(OwnerThreadId::MediaSessions);
  media_attempted_calls_.Insert(call_id);
  RunAnswererStart(call_id, peer_identity);
}

void CallMediaBridge::RunAnswererStart(const std::string& call_id, const std::string& peer_identity) {
  log().info << "ScheduleStartMediaAsAnswerer UI enter call_id=" << call_id << " peer=" << peer_identity;
  // Re-arm Direct before Apply so product AllowsDirectPath for Schedule.
  if (arming_.IsBound() && arming_.direct_ops_allowed && !arming_.direct_ops_allowed()) {
    log().info << "ScheduleStartMediaAsAnswerer request_direct_arming call_id=" << call_id
               << " arming=" << (arming_.arming_debug_name ? arming_.arming_debug_name() : "?");
    if (arming_.request_direct_arming) {
      arming_.request_direct_arming(call_id);
    }
  }
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
  if (media_.IsActive() && media_.ActiveCallId() == call_id) {
    log().info << "ScheduleStartMediaAsAnswerer skip (already active) call_id=" << call_id
               << " sfu_mode=" << (media_.IsSfuMode() ? 1 : 0) << " direct=" << (direct_.IsActive() ? 1 : 0);
    return;
  }
  if (auto key = LoadActiveMediaKey(call_id); !key) {
    DeferAnswererUntilMediaKey(call_id, peer_identity, key.error().message);
    return;
  }
  log().info << "ScheduleStartMediaAsAnswerer key ready — BeginSession StartSfu call_id=" << call_id;
  SetDirectPlannerPhase(CallDirectPlannerPhase::Connecting, CallDirectPlannerEvent::ScheduleAnswerer, call_id);
  if (auto started = StartMediaAsAnswerer(call_id, peer_identity); !started) {
    log().warning << "StartMediaAsAnswerer failed: " << started.error().message;
    SurfaceConnectFailed(call_id, started.error().message, /*stop_media=*/true);
  } else {
    log().info << "answerer StartSfu ok call_id=" << call_id << " direct=" << (direct_.IsActive() ? 1 : 0)
               << " engine=" << (media_.IsActive() ? 1 : 0);
  }
}

void CallMediaBridge::DeferAnswererUntilMediaKey(const std::string& call_id, const std::string& peer_identity,
                                                 const std::string& reason) {
  // V015: epoch-1 key is sent by offerer on CallAccept — defer until it lands.
  SetDirectPlannerPhase(CallDirectPlannerPhase::KeyWait, CallDirectPlannerEvent::ScheduleAnswerer, call_id);
  log().info << "Defer answerer media until CallMediaKey call_id=" << call_id << " reason=" << reason;
  pending_answerer_call_id_ = call_id;
  pending_answerer_peer_ = peer_identity;
  media_attempted_calls_.Insert(call_id);
  if (arming_.on_media_deferred) {
    arming_.on_media_deferred(call_id);
  }
  // Each poll round is a delayed event naming this wait; a newer wait (or none) makes it stale.
  const uint64_t wait = ++key_wait_gen_;
  if (media_key_inbox_poll_rounds_ <= 0) {
    outbox_.Emit(direct_event::KeyPollDue{call_id, wait, 0});
    return;
  }
  host_.P2pRequestInboxSync();
  (void)outbox_.After(kMediaKeyPollRound, direct_event::KeyPollDue{call_id, wait, 0});
}

void CallMediaBridge::OnKeyPollDue(const direct_event::KeyPollDue& due) {
  // Accept-time SyncInbox often races the offerer's MediaKey send — keep polling (one request a
  // round; SyncInbox coalesces, so a request need not start HTTP).
  if (stopping_.load(std::memory_order_acquire) || key_wait_gen_ != due.wait) {
    return;  // key arrived, Leave, or a newer wait
  }
  if (due.round < media_key_inbox_poll_rounds_ && LoadActiveMediaKey(due.call_id)) {
    log().info << "Deferred MediaKey found in store — kick start call_id=" << due.call_id;
    OnMediaKeyReady(due.call_id);
    return;
  }
  if (due.round + 1 < media_key_inbox_poll_rounds_) {
    host_.P2pRequestInboxSync();
    (void)outbox_.After(kMediaKeyPollRound, direct_event::KeyPollDue{due.call_id, due.wait, due.round + 1});
    return;
  }
  // Surface failure — do not leave chrome stuck in MediaPending forever.
  OnDeferredMediaKeyTimeout(due.call_id);
}

void CallMediaBridge::OnDeferredMediaKeyTimeout(const std::string& call_id) {
  if (pending_answerer_call_id_ != call_id) {
    return; // key arrived, Leave, or superseding Accept
  }
  pending_answerer_call_id_.clear();
  ++key_wait_gen_;
  pending_answerer_peer_.clear();
  log().warning << "Deferred MediaKey wait exhausted call_id=" << call_id << " — ConnectFailed";
  mesh_connect_failed_ = true;
  host_.P2pSetLastMediaError(Tr("call.error.media_key_timeout"));
  Apply(CallDirectPlannerEvent::KeyTimeout, call_id);
  if (arming_.on_connect_failed) {
    arming_.on_connect_failed(call_id);
  }
  host_.P2pNotifyRingChanged();
}

void CallMediaBridge::OnMediaKeyReady(const std::string& call_id) {
  if (call_id.empty()) {
    return;
  }
  // Wake inbound hello key-wait (if any) before hopping to UI for deferred answerer start.
  connect_.NotifyKeyAvailable();
  // The start runs as its own event, never inside the key exchange that reported the key.
  outbox_.Emit(direct_event::MediaKeyReady{call_id});
}

void CallMediaBridge::StartDeferredAnswerer(const std::string& call_id) {
  std::string peer = pending_answerer_peer_;
  const bool pending = (pending_answerer_call_id_ == call_id);
  if (!pending) {
    // Key stored for later Accept LoadActiveMediaKey — do NOT auto-start. Late keys from a
    // prior call were starting answerer media on the wrong call_id (Samsung dogfood).
    log().info << "CallMediaKey stored (not deferred yet) call_id=" << call_id;
    return;
  }
  if (peer.empty()) {
    if (auto resolved = host_.P2pPeerIdentityForCall(call_id); resolved && resolved->has_value()) {
      peer = **resolved;
    }
  }
  log().info << "CallMediaKey ready — starting deferred answerer media call_id=" << call_id;
  pending_answerer_call_id_.clear();
  ++key_wait_gen_;
  pending_answerer_peer_.clear();
  Apply(CallDirectPlannerEvent::KeyReady, call_id, peer);
  if (arming_.on_media_key_ready) {
    arming_.on_media_key_ready(call_id);
  }
  if (!peer.empty()) {
    ScheduleStartMediaAsAnswerer(call_id, peer);
  }
}

void CallMediaBridge::StopMeshMedia(const std::string& call_id) {
  // Bridge state, the connect sequence and CallMediaEngine::Stop (SDL capture — CALLS.md) are
  // calls-owner only; every stop reaches here through the owner's events.
  PBR_ASSERT_ON_OWNER(OwnerThreadId::MediaSessions);
  StopMeshMediaOnUi(call_id);
}

void CallMediaBridge::StopMeshMediaOnUi(const std::string& call_id) {
  receive_->remote_stream.store(0, std::memory_order_release);  // the next call binds its own
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
  if (pending_answerer_call_id_ == call_id) {
    pending_answerer_call_id_.clear();
    ++key_wait_gen_;
    pending_answerer_peer_.clear();
  }
  reach_.ReleasePeer(peer);
  direct_.Detach();
  media_peer_identity_.clear();
  media_call_id_.clear();
  reach_kind_ = PeerLinkKind::Unknown;
  force_circuit_ensure_ = false;
  session_offerer_ = false;
  direct_connected_at_ms_ = 0;
  tx_only_escalation_done_ = false;
  ClearMeshConnectFailed();
  media_attempted_calls_.Erase(call_id);
  failed_open_.reset();
  media_.SetOnStateChanged({});

  // Always stop leftover media_relay even when ActiveCallId drifted or is empty
  // (dogfood cbe535: End left SFU capture running → next call Connected/reconnecting,
  // zombie RX stream, no audio). One engine serves one call.
  if (!media_.IsActive() && !media_.IsSfuMode()) {
    return;
  }
  const std::string active = media_.ActiveCallId();
  if (!active.empty() && !call_id.empty() && active != call_id) {
    log().warning << "StopMeshMedia stopping mismatched engine call_id=" << active << " leave=" << call_id;
  } else {
    log().info << "StopMeshMedia stopping engine call_id=" << active << " leave=" << call_id;
  }
  StopEngineFor(call_id.empty() ? active : call_id, "stop mesh media");
}

CallMediaCoordinator* CallMediaBridge::LiveCallMedia(const std::string& call_id) {
  // Quietly null for a call not admitted here (a leftover engine id): nothing to ask about it.
  return !call_id.empty() && host_.P2pLiveCall(call_id) ? host_.P2pCallMedia(call_id) : nullptr;
}

bool CallMediaBridge::HopAttachedFor(const std::string& call_id) {
  CallMediaCoordinator* call_media = LiveCallMedia(call_id);
  return call_media && call_media->HopAttached();
}

void CallMediaBridge::StopEngineFor(const std::string& call_id, const char* why) {
  if (CallMediaCoordinator* call_media = LiveCallMedia(call_id)) {
    call_media->StopEngine(why);
    return;
  }
  // Leftover media of a call not admitted here (e.g. from before a restart): still stop it.
  log().info << "engine stop without a live call call_id=" << call_id << " (" << why << ")";
  media_.Stop();
}

void CallMediaBridge::ReleaseDirectTransport() {
  if (seat_.IsBound()) {
    ReleaseDirectTransport(seat_.current_token());
    return;
  }
  ReleaseDirectTransportBody();
}

void CallMediaBridge::ReleaseDirectTransport(const CallMediaSeat::Token& token) {
  if (seat_.IsBound()) {
    if (!seat_.allows_path_op(token)) {
      log().info << "ReleaseDirectTransport skip (token not bound) call_id=" << token.call_id
                 << " bound=" << (seat_.bound_call_id ? seat_.bound_call_id() : "");
      return;
    }
  }
  ReleaseDirectTransportBody();
}

void CallMediaBridge::ReleaseDirectTransportBody() {
  // SoftMigrate: drop 1:1 transport; keep CallMediaEngine capture feeding media_relay.
  Apply(CallDirectPlannerEvent::ReleaseTransport, media_call_id_, media_peer_identity_);
  AbortConnectSequence();
  CancelDirectHealthTimer();
  const std::string peer = media_peer_identity_;
  reach_.ReleasePeer(peer);
  direct_.Detach();
  media_peer_identity_.clear();
  reach_kind_ = PeerLinkKind::Unknown;
  inbound_deferred_peer_id_.clear();
  receive_->remote_stream.store(0, std::memory_order_release);
  // Do not ClearRemoteAudioTracks here — SoftMigrate+2s would wipe live media_relay tracks
  // that already replaced 1:1 (dogfood: streams look healthy then Moto silent on PreferLocal).
  // 1:1 on_audio is already ignored once the hop is attached; stream_id==1 is dropped in engine.
  ClearMeshConnectFailed();
  // V036 Phase 2: signaling may advance to InCall, but chrome Connected requires seat Live
  // (set by CompleteAttachLocalToSfu NoteLive — not ReleaseDirect alone).
  if (arming_.IsBound() && media_.IsActive() && media_.IsSfuMode()) {
    if (arming_.on_connected) {
      arming_.on_connected(media_.ActiveCallId());
    }
  }
  host_.P2pNotifyRingChanged();
}

void CallMediaBridge::NotePeerIdRelayMapping(const std::string& peer_id,
                                                   const std::string& relay_identity) {
  if (peer_id.empty() || relay_identity.rfind("account:", 0) != 0) {
    return;
  }
  if (inbound_deferred_peer_id_.empty() || inbound_deferred_peer_id_ != peer_id) {
    return;
  }
  media_peer_identity_ = relay_identity;
  const uint32_t stream = PublisherStreamIdForIdentity(relay_identity);
  receive_->remote_stream.store(stream, std::memory_order_release);
  log().info << "Inbound call-media mapping from CallAccept/Invite stream_id=" << stream
             << " peer_id=" << peer_id << " account=" << relay_identity;
}

void CallMediaBridge::PrepareForTeardown(int /*timeout_ms*/) {
  receive_->remote_stream.store(0, std::memory_order_release);
  stopping_.store(true, std::memory_order_release);
  reach_.AbortCircuitAttempts();
  Apply(CallDirectPlannerEvent::Stop, media_call_id_, media_peer_identity_);
  AbortConnectSequence();
  // Also releases waiting inbound hellos and drops the inbound handler before Detach, so late
  // streams cannot reach the bridge.
  connect_.Shutdown();
  CancelDirectHealthTimer();
  CancelReserveRenewal();
  CancelDirectUpgrade();
  CancelRelayStandby();
  CancelEscalateReach();
  CancelReanchor();
  const std::string peer = media_peer_identity_;
  const std::string call_id = media_call_id_;
  pending_answerer_call_id_.clear();
  ++key_wait_gen_;
  pending_answerer_peer_.clear();
  reach_.ReleasePeer(peer);
  direct_.Detach();
  media_peer_identity_.clear();
  media_call_id_.clear();
  ClearMeshConnectFailed();

  if (!call_id.empty() && media_.IsActive() && media_.ActiveCallId() == call_id) {
    media_.Stop();
  }
}

Roe<void> CallMediaBridge::RetryMeshMedia(const std::string& call_id) {
  if (call_id.empty()) {
    return Error("call_id required");
  }
  // Restarts the engine and the connect sequence — calls owner only.
  PBR_ASSERT_ON_OWNER(OwnerThreadId::MediaSessions);
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
    if (auto resolved = host_.P2pPeerIdentityForCall(call_id); resolved && resolved->has_value()) {
      peer = **resolved;
    }
  }
  if (peer.empty()) {
    return Error("No peer for call retry");
  }
  ClearMeshConnectFailed();
  reach_.ForgetPath(peer);
  if (media_.IsActive() && media_.ActiveCallId() == call_id) {
    StopEngineFor(call_id, "retry");
  }
  direct_.Detach();
  ArmPlannerForRestart(call_id, peer, true);
  return BeginSession(call_id, peer, true);
}

void CallMediaBridge::ArmPlannerForRestart(const std::string& call_id, const std::string& peer_identity,
                                           const bool offerer) {
  Apply(offerer ? CallDirectPlannerEvent::ScheduleOfferer : CallDirectPlannerEvent::ScheduleAnswerer, call_id,
        peer_identity);
  Apply(CallDirectPlannerEvent::KeyReady, call_id, peer_identity);
}

Roe<void> CallMediaBridge::ResumeMeshMediaFromInbound(const std::string& call_id) {
  if (call_id.empty()) {
    return Error("call_id required");
  }
  PBR_ASSERT_ON_OWNER(OwnerThreadId::MediaSessions);
  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value() || (*session)->state == CallSessionState::Ended) {
    return Error("Call session not found");
  }
  if (!failed_open_ || failed_open_->call_id != call_id) {
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
  ClearMeshConnectFailed();
  ArmPlannerForRestart(call_id, call->peer_identity, call->offerer);
  // BeginSession keeps the active inbound bundle (StopPriorDirectAttempt) and commits it.
  return BeginSession(call_id, call->peer_identity, call->offerer);
}

std::optional<CallMediaBridge::CallPeerRole> CallMediaBridge::PeerRoleFromLiveCall(const std::string& call_id) const {
  const LiveCall* call = host_.P2pLiveCall(call_id);
  if (!call || !call->IsOpen()) {
    return std::nullopt;
  }
  auto peer = call->SolePeer();
  if (!peer) {
    return std::nullopt;
  }
  return CallPeerRole{*peer, call->Origin() == LiveCallOrigin::Placed};
}

void CallMediaBridge::NoteMediaAttempted(const std::string& call_id) {
  media_attempted_calls_.Insert(call_id);
}

bool CallMediaBridge::MediaAttempted(const std::string& call_id) const {
  return media_attempted_calls_.Contains(call_id);
}

} // namespace pbr
