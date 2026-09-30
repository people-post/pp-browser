#include "feature/calls/CallSessionManager.h"
#include "feature/calls/CallControlClient.h"
#include "domain/messaging/CallListenAddrsLogic.h"
#include "domain/messaging/CallAnswererKickLogic.h"
#include "domain/messaging/CallMediaPlannerSelectLogic.h"

#include "foundation/crypto/CryptoUtil.h"
#include "domain/media/CallMediaAdaptation.h"
#include "domain/messaging/CallSessionLogic.h"
#include "domain/messaging/BroadcastJoinTicket.h"
#include "domain/people/DirectChatTargetFromContact.h"
#include "domain/messaging/PairwiseFanoutLogic.h"
#include "domain/messaging/CallControlThreadLogic.h"
#include "domain/messaging/PeerCapsLogic.h"
#include "domain/messaging/SendRelayOptions.h"
#include "domain/people/ContactIdentity.h"
#include "domain/people/ContactJson.h"
#include "domain/people/ContactTypes.h"
#include "domain/people/MeshHopPolicy.h"
#include "foundation/runtime/AppRuntime.h"
#include "common/Utilities.h"

#include <algorithm>
#include <type_traits>
#include <variant>

#include "common/ValueJson.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

void PrefetchReachForIdentity(const CallSessionManager::PrefetchPeerReachFn& fn, const std::string& identity) {
  if (!identity.empty() && fn) {
    fn(identity);
  }
}

void NoteCapsForIdentity(CallSessionManager& sessions, ContactsStore& contacts,
                         const std::string& identity, const CallPeerCaps& caps,
                         const std::vector<std::string>& listen_multiaddrs) {
  if (!caps.present && listen_multiaddrs.empty()) {
    return;
  }
  std::vector<std::string> peer_ids = PeerIdsFromListenMultiaddrs(listen_multiaddrs);
  if (peer_ids.empty() && !identity.empty()) {
    if (auto found = contacts.FindByIdentity(identity, ContactIdKind::Account);
        found && found->has_value()) {
      const std::string peer_id = PeerIdFromContact(**found);
      if (!peer_id.empty()) {
        peer_ids.push_back(peer_id);
      }
    }
  }
  for (const std::string& peer_id : peer_ids) {
    if (caps.present) {
      sessions.NotePeerMediaRelayCap(peer_id, caps.media_relay);
    }
    if (IsAccountIdentityValue(identity)) {
      sessions.NoteMeshPeerIdForRelay(identity, peer_id);
    }
  }
}

} // namespace


CallSessionManager::CallSessionManager(IThreadStore& store, ContactsStore& contacts, IdentityStore& identity,
                                       CallSessionStore& sessions, CallMediaKeyStore& media_keys,
                                       CallDeliveryPorts delivery, IPskSessionStore& psk_store, CallMediaEngine& media)
    : contacts_(contacts), sessions_(sessions), media_keys_(media_keys),
      delivery_(std::move(delivery)), media_(media),
      topology_(sessions, contacts, media),
      control_(store, contacts, identity, sessions, psk_store, delivery_,
               [this]() -> std::optional<std::string> {
                 auto active = ActiveLocalCall();
                 if (active && *active && (*active)->origin_thread_id) {
                   return *(*active)->origin_thread_id;
                 }
                 return std::nullopt;
               }),
      key_exchange_(sessions, media_keys, control_), billing_(identity), reach_signals_(control_),
      workflow_(store, sessions, key_exchange_, billing_, live_calls_), peer_accounts_(contacts) {
  redirectLogger("CallSessionManager");
  topology_.SetMediaKeyStore(&media_keys_);
  key_exchange_.SetOnKeyReady([this](const std::string& call_id) {
    const auto direct_media = direct_media_.Get();
    if (direct_media->on_media_key_ready) {
      direct_media->on_media_key_ready(call_id);
    }
  });
  BindSeat();
  live_calls_.BindHopDriver(&topology_);
  live_calls_.SetOnChanged([this]() { NotifyCallStateChanged(); });
  topology_.SetHopArmingPorts(MakeHopArmingPorts());
  BindTopologyHostPorts();
  BindWorkflowHostPorts();
  BindReachSignalPorts();
}

void CallSessionManager::BindTopologyHostPorts() {
  CallTopologyController::HostPorts ports;
  ports.local_relay_identity = [this]() { return TopologyLocalIdentity(); };
  ports.leave_call = [this](const std::string& call_id) { return TopologyLeaveCall(call_id); };
  ports.call_media = [this](const std::string& call_id) { return live_calls_.Media(call_id); };
  ports.fan_out_joined = [this](const std::string& call_id, CallControlType type, const std::string& detail,
                                const std::string& display, const std::string& skip) {
    return TopologyFanOutToJoined(call_id, type, detail, display, skip);
  };
  ports.send_direct = [this](const std::string& peer, CallControlType type, const std::string& detail,
                             const std::string& display) {
    return TopologySendDirect(peer, type, detail, display);
  };
  ports.notify_ring_changed = [this]() { TopologyNotifyRingChanged(); };
  ports.set_last_media_error = [this](std::string message) { TopologySetLastMediaError(std::move(message)); };
  ports.set_media_activity = [this](std::string message) { TopologySetMediaActivity(std::move(message)); };
  ports.clear_media_activity = [this]() { TopologyClearMediaActivity(); };
  ports.note_media_attempted = [this](const std::string& call_id) { TopologyNoteMediaAttempted(call_id); };
  ports.clear_media_peer_identity = [this]() { TopologyClearMediaPeerIdentity(); };
  ports.request_inbox_sync = [this]() { TopologyRequestInboxSync(); };
  topology_.SetHostPorts(std::move(ports));
}

void CallSessionManager::BindWorkflowHostPorts() {
  CallSessionWorkflow::HostPorts ports;
  ports.wire = MakeWorkflowWirePorts();
  ports.duplex.schedule_start_direct = [this](const std::string& call_id, const std::string& peer, bool offerer) {
    ScheduleStartDirectMedia(call_id, peer, offerer);
  };
  ports.hop = MakeWorkflowHopPorts();
  ports.reach = MakeWorkflowReachPorts();
  workflow_.SetHostPorts(std::move(ports));
}

CallSessionWorkflow::WirePorts CallSessionManager::MakeWorkflowWirePorts() {
  CallSessionWorkflow::WirePorts ports;
  ports.local_relay_identity = [this]() { return control_.LocalRelayIdentity(); };
  ports.notify_ring_changed = [this]() { NotifyRingChanged(); };
  ports.send_direct = [this](const std::string& peer, CallControlType type, const std::string& detail,
                             const std::string& display) {
    return control_.SendDirect(peer, type, detail, display);
  };
  ports.ensure_control_thread = [this](const std::string& peer) { return control_.EnsureCallControlThread(peer); };
  ports.fan_out_joined = [this](const std::string& call_id, CallControlType type, const std::string& detail,
                                const std::string& display, const std::string& skip) {
    return control_.FanOutToJoined(call_id, type, detail, display, skip);
  };
  ports.fan_out_joined_and_ringing = [this](const std::string& call_id, CallControlType type,
                                            const std::string& detail, const std::string& display,
                                            const std::string& skip) {
    return control_.FanOutToJoinedAndRinging(call_id, type, detail, display, skip);
  };
  ports.append_origin_history = [this](const std::string& thread_id, CallControlType type,
                                       const std::string& text, const std::string& detail) {
    return control_.AppendOriginHistory(thread_id, type, text, detail);
  };
  ports.build_roster_detail = [this](const std::string& call_id) { return BuildRosterDetail(call_id); };
  ports.clear_media_activity = [this]() { TopologyClearMediaActivity(); };
  ports.sync_inbox_from_wake = [this]() {
    if (delivery_.sync_inbox_from_wake) {
      delivery_.sync_inbox_from_wake(true);
    }
  };
  return ports;
}

CallSessionWorkflow::HopPathPorts CallSessionManager::MakeWorkflowHopPorts() {
  CallSessionWorkflow::HopPathPorts ports;
  ports.on_joined_count_observed = [this](const std::string& call_id, size_t n) {
    topology_.OnJoinedCountObserved(call_id, n);
  };
  ports.plan_hop_for_invitees = [this](const std::vector<std::string>& invitees, const std::string& local) {
    return topology_.HopPlanning().Plan(invitees, local);
  };
  ports.probe_invite_hops = [this](const std::string& call_id) { topology_.HopPlanning().ProbeInvite(call_id); };
  ports.hop_report_for_accept = [this](const std::string& call_id) {
    return topology_.HopPlanning().ReportForAccept(call_id);
  };
  ports.note_accept_hop_report = [this](const std::string& call_id, const std::string& identity,
                                            const CallHopReport& report) {
    topology_.HopPlanning().NoteAcceptReport(call_id, identity, report);
  };
  ports.clear_sfu_attach_wait = [this]() { topology_.ClearSfuAttachWait(); };
  ports.on_inbound_sfu_attach = [this](const std::string& call_id, const CallSfuAttachDetail& d,
                                           const std::string& sender) {
    return topology_.OnInboundSfuAttach(call_id, d, sender);
  };
  ports.on_inbound_sfu_attach_failed = [this](const CallSfuAttachFailedDetail& d) {
    topology_.OnInboundSfuAttachFailed(d);
  };
  ports.on_inbound_hop_refuse = [this](const CallHopRefuseDetail& d) { topology_.OnInboundHopRefuse(d); };
  ports.is_on_sfu_for_call = [this](const std::string& call_id) {
    return topology_.IsOnSfuForCall(call_id);
  };
  ports.has_media_relay_hop_candidates = [this]() {
    return topology_.HopRanking().HasCandidates();
  };
  return ports;
}

CallSessionWorkflow::ReachPorts CallSessionManager::MakeWorkflowReachPorts() {
  CallSessionWorkflow::ReachPorts ports;
  ports.register_peer_listen = [this](const std::string& identity, const std::vector<std::string>& mas) {
    if (register_peer_listen_multiaddrs_) {
      register_peer_listen_multiaddrs_(identity, mas);
    }
  };
  ports.note_caps_for_identity = [this](const std::string& identity, const CallPeerCaps& caps,
                                        const std::vector<std::string>& listen) {
    NoteCapsForIdentity(*this, contacts_, identity, caps, listen);
  };
  ports.note_call_peer_caps = [this](const std::string& call_id, const CallPeerCaps& caps) {
    if (call_peer_caps_sink_) {
      call_peer_caps_sink_(call_id, caps);
    }
  };
  ports.prefetch_reach = [this](const std::string& identity) {
    PrefetchReachForIdentity(prefetch_reach_, identity);
  };
  ports.ensure_circuit_ready = [this]() {
    if (ensure_circuit_ready_) {
      ensure_circuit_ready_();
    }
  };
  ports.park_circuit = [this](int timeout_ms, std::function<void(bool)> done) {
    if (park_circuit_) {
      park_circuit_(timeout_ms, std::move(done));
    } else {
      done(false);
    }
  };
  ports.note_mesh_peer_id_for_relay = [this](const std::string& relay, const std::string& peer_id) {
    NoteMeshPeerIdForRelay(relay, peer_id);
  };
  ports.local_listen_multiaddrs = [this]() -> std::vector<std::string> {
    return local_listen_multiaddrs_ ? local_listen_multiaddrs_() : std::vector<std::string>{};
  };
  ports.local_peer_caps = [this]() -> CallPeerCaps {
    return local_peer_caps_ ? local_peer_caps_() : CallPeerCaps{};
  };
  ports.local_mesh_peer_id = [this]() -> std::string {
    return local_mesh_peer_id_ ? local_mesh_peer_id_() : std::string{};
  };
  return ports;
}

void CallSessionManager::BindReachSignalPorts() {
  CallReachSignals::CallPorts ports;
  ports.active_call_id = [this]() -> std::optional<std::string> {
    auto active = ActiveLocalCall();
    if (!active || !active->has_value()) {
      return std::nullopt;
    }
    return (*active)->call_id;
  };
  ports.call_peer = [this](const std::string& call_id) -> std::optional<std::string> {
    auto peer = PeerIdentityForCall(call_id);
    if (!peer || !peer->has_value()) {
      return std::nullopt;
    }
    return **peer;
  };
  ports.local_identity = [this]() -> std::string {
    auto local = P2pLocalIdentity();
    return local ? *local : std::string{};
  };
  ports.local_caps = [this]() -> std::optional<CallPeerCaps> {
    return local_peer_caps_ ? std::optional<CallPeerCaps>{local_peer_caps_()} : std::nullopt;
  };
  ports.local_listen_addrs = [this]() {
    return local_listen_multiaddrs_ ? local_listen_multiaddrs_() : std::vector<std::string>{};
  };
  ports.local_peer_id = [this]() { return local_mesh_peer_id_ ? local_mesh_peer_id_() : std::string{}; };
  ports.register_listen = [this](const std::string& key, const std::vector<std::string>& addrs) {
    if (register_peer_listen_multiaddrs_) {
      register_peer_listen_multiaddrs_(key, addrs);
    }
  };
  ports.on_peer_caps = [this](const std::string& call_id, const CallPeerCaps& caps) {
    if (call_peer_caps_sink_) {
      call_peer_caps_sink_(call_id, caps);
    }
  };
  reach_signals_.SetCallPorts(std::move(ports));
}

// --- UI intents (V037) ---------------------------------------------------------------------------

void CallSessionManager::NotifyCallStateChanged() {
  if (on_call_state_changed_) {
    on_call_state_changed_();
  }
}

std::string CallSessionManager::AcceptingCallId() const {
  return !accept_in_flight_.empty() ? accept_in_flight_ : live_calls_.AcceptingCallId();
}

void CallSessionManager::Apply(const CallLifecycleEvent ev, const std::string& call_id) {
  switch (ev) {
  case CallLifecycleEvent::AcceptClicked:
    ClickAccept(call_id);
    return;
  case CallLifecycleEvent::DeclineClicked:
    ClickDecline(call_id);
    return;
  case CallLifecycleEvent::LeaveClicked:
    ClickLeave(call_id);
    return;
  case CallLifecycleEvent::RetryClicked:
    RestartMedia(call_id, false);
    return;
  case CallLifecycleEvent::PeerReconnected:
    RestartMedia(call_id, true);
    return;
  case CallLifecycleEvent::OutboundStarted:
    live_calls_.NoteOutboundStarted(call_id);
    return;
  case CallLifecycleEvent::MediaDeferred:
    live_calls_.NoteMediaDeferred(call_id);
    return;
  case CallLifecycleEvent::MediaKeyReady:
    live_calls_.NoteMediaKeyReady(call_id);
    return;
  case CallLifecycleEvent::DirectConnected:
    live_calls_.NoteMediaConnected(call_id);
    return;
  case CallLifecycleEvent::ConnectFailedEvt:
    live_calls_.NoteMediaFailed(call_id);
    return;
  case CallLifecycleEvent::InviteSeen:
  case CallLifecycleEvent::InviteCleared:
    // The ring is the LiveCall's: nothing to change, only to show.
    NotifyCallStateChanged();
    return;
  }
}

void CallSessionManager::ClickAccept(const std::string& call_id_arg) {
  std::string call_id = call_id_arg;
  if (call_id.empty()) {
    if (const LiveCall* ring = live_calls_.TheRing()) {
      call_id = ring->Id();
    }
  }
  if (call_id.empty()) {
    log().info << "AcceptClicked ignored (no call_id)";
    return;
  }
  if (AcceptingCallId() == call_id) {
    log().info << "AcceptClicked already in flight call_id=" << call_id;
    NotifyCallStateChanged();
    return;
  }
  accept_in_flight_ = call_id;
  NotifyCallStateChanged();
  AppRuntime::ResumeBackgroundWork();  // relay-fallback sends run on workers
  log().info << "AcceptInvite queued call_id=" << call_id;
  AcceptInviteAsync(call_id, [this, call_id](Roe<void> accepted) { OnAcceptResult(call_id, accepted); });
}

void CallSessionManager::OnAcceptResult(const std::string& call_id, const Roe<void>& accepted) {
  if (accept_in_flight_ == call_id) {
    accept_in_flight_.clear();
  }
  if (!accepted) {
    // The workflow put the call back to ringing (or it ended meanwhile).
    log().warning << "AcceptInvite failed call_id=" << call_id << " err=" << accepted.error().message;
    last_error_ = accepted.error().message;
    NotifyCallStateChanged();
    return;
  }
  const LiveCall* active = live_calls_.Active();
  if (!active || active->Id() != call_id) {
    // B-CONFLICT: another accept (or a leave) moved on while this one was on the wire.
    log().info << "AcceptInvite result ignored stale call_id=" << call_id;
    return;
  }
  log().info << "AcceptInvite ok call_id=" << call_id;
  KickAnswererAfterAccept(call_id);
  NotifyCallStateChanged();
}

void CallSessionManager::KickAnswererAfterAccept(const std::string& call_id) {
  if (!live_calls_.AllowsDirectPath(call_id)) {
    log().info << "AcceptSucceeded skip KickAnswerer StartSfu call_id=" << call_id
               << " status=" << CallMediaStatusName(live_calls_.Status(call_id));
    return;
  }
  log().info << "AcceptSucceeded KickAnswererDirectMedia StartSfu arm call_id=" << call_id;
  KickAnswererDirectMediaIfArmed(call_id);
  // Once more, as the next event: the first kick can land before the 1:1 start is armed.
  outbox_.Emit(session_event::AnswererKickRetry{call_id});
}

void CallSessionManager::ClickDecline(const std::string& call_id_arg) {
  std::string call_id = call_id_arg;
  if (call_id.empty()) {
    if (const LiveCall* ring = live_calls_.TheRing()) {
      call_id = ring->Id();
    }
  }
  if (call_id.empty()) {
    log().info << "DeclineClicked ignored (no call_id)";
    return;
  }
  if (auto declined = DeclineInvite(call_id); !declined) {
    log().warning << "DeclineInvite failed call_id=" << call_id << " err=" << declined.error().message;
    live_calls_.Close(call_id, LiveCallEndReason::Declined);  // the ring goes either way
  }
  NotifyCallStateChanged();
}

void CallSessionManager::ClickLeave(const std::string& call_id_arg) {
  std::string call_id = call_id_arg;
  if (call_id.empty()) {
    if (const LiveCall* shown = live_calls_.Shown()) {
      call_id = shown->Id();
    }
  }
  if (call_id.empty()) {
    log().info << "LeaveClicked ignored (no call_id)";
    return;
  }
  if (auto left = LeaveCall(call_id); !left) {
    log().warning << "LeaveCall failed call_id=" << call_id << " err=" << left.error().message;
    live_calls_.Close(call_id, LiveCallEndReason::LocalLeave);  // the call leaves the screen either way
  }
  NotifyCallStateChanged();
}

void CallSessionManager::RestartMedia(const std::string& call_id_arg, const bool resume) {
  const char* what = resume ? "PeerReconnected" : "RetryClicked";
  const LiveCall* active = live_calls_.Active();
  const std::string call_id = call_id_arg.empty() && active ? active->Id() : call_id_arg;
  const LiveCall* call = live_calls_.Find(call_id);
  // Only a failed, still-open call restarts; a resume only for the call this device is in.
  if (!call || call->Phase() != CallPhase::ConnectFailed || (resume && (!active || active->Id() != call_id))) {
    log().info << what << " ignored (call not failed-open) call_id=" << call_id;
    return;
  }
  // Re-arm Direct before the restart → BeginSession (Failed blocks AllowsDirectPath).
  live_calls_.SetMediaStatus(call_id, CallMediaStatus::DirectConnecting, what);
  // As the next event: the restart runs the engine and the bridge's connect sequence — never inside
  // the click (or the bridge callback) that asked for it.
  outbox_.Emit(session_event::MediaRestart{call_id, resume});
}

void CallSessionManager::RunMediaRestart(const std::string& call_id, const bool resume) {
  const char* what = resume ? "PeerReconnected" : "RetryClicked";
  const Roe<void> restarted = resume ? ResumeP2pMedia(call_id) : RetryP2pMedia(call_id);
  if (!restarted) {
    log().warning << what << " media restart failed call_id=" << call_id << " err=" << restarted.error().message;
    // Back to Failed only while the call is still open and not live again (a duplicate restart
    // whose call already resumed must not knock it down — PR #239 review).
    const LiveCall* again = live_calls_.Find(call_id);
    if (again && again->IsOpen() && !again->MediaProgress().reached_live) {
      live_calls_.NoteMediaFailed(call_id);
    }
    return;
  }
  NotifyCallStateChanged();
}

void CallSessionManager::RetryAnswererKick(const std::string& call_id) {
  const LiveCall* active = live_calls_.Active();
  if (!active || active->Id() != call_id || !live_calls_.AllowsDirectPath(call_id)) {
    return;
  }
  if (media_.IsActive() && media_.ActiveCallId() == call_id) {
    return;
  }
  log().info << "AcceptSucceeded retry KickAnswererDirectMedia StartSfu call_id=" << call_id;
  KickAnswererDirectMediaIfArmed(call_id);
}

void CallSessionManager::SetOutbox(OwnerOutbox<SessionEvent> outbox) {
  outbox_ = std::move(outbox);
  workflow_.SetOutbox(outbox_.For<WorkflowEvent>([](WorkflowEvent event) { return SessionEvent{std::move(event)}; }));
  topology_.SetOutbox(outbox_.For<TopologyEvent>([](TopologyEvent event) { return SessionEvent{std::move(event)}; }));
}

void CallSessionManager::Handle(SessionEvent& event) {
  std::visit(
      [this](auto& e) {
        using E = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<E, session_event::AnswererKickRetry>) {
          RetryAnswererKick(e.call_id);
        } else if constexpr (std::is_same_v<E, session_event::MediaRestart>) {
          RunMediaRestart(e.call_id, e.resume);
        } else if constexpr (std::is_same_v<E, WorkflowEvent>) {
          workflow_.Handle(e);
        } else if constexpr (std::is_same_v<E, TopologyEvent>) {
          topology_.Handle(e);
        } else if constexpr (std::is_same_v<E, session_event::ForDirectPath>) {
          if (direct_path_ && e.generation == direct_path_generation_) {
            direct_path_->Handle(e.event);
          }
        } else {
          static_assert(!sizeof(E), "route every SessionEvent");
        }
      },
      event);
}

void CallSessionManager::SetInitiationBillingStore(InitiationBillingStore* store) {
  billing_.SetStore(store);
}

void CallSessionManager::SetMediaRelayDeps(MediaRelayDeps deps) {
  topology_.SetMediaRelayDeps(std::move(deps));
}

// Port setters swap a snapshot; the workflow / topology host ports (bound once in the ctor) read the
// current snapshot per call — never rebuilt under a running caller (thread-ownership t2a).
void CallSessionManager::SetDirectMediaPorts(CallDirectMediaPorts ports) {
  direct_media_.Set(std::move(ports));
}

CallHopArmingPorts CallSessionManager::MakeHopArmingPorts() {
  CallHopArmingPorts ports;
  ports.hop_ops_allowed = [this]() { return live_calls_.AllowsHopPath(); };
  ports.soft_migrate_may_arm = [this]() { return live_calls_.SoftMigrateMayArm(); };
  ports.media_cancel_gen = [this]() { return live_calls_.MediaCancelGen(); };
  ports.report_progress = [this](CallHopPlannerPhase phase, const std::string& call_id) {
    live_calls_.ReportHopProgress(call_id, phase);
  };
  ports.arming_debug_name = [this]() { return CallMediaStatusName(live_calls_.Status()); };
  return ports;
}

CallDirectArmingPorts CallSessionManager::DirectArmingPorts() {
  CallDirectArmingPorts ports;
  ports.direct_ops_allowed = [this]() { return live_calls_.AllowsDirectPath(); };
  ports.request_direct_arming = [this](const std::string& call_id) { live_calls_.RequestDirectArming(call_id); };
  ports.report_progress = [this](CallDirectPlannerPhase phase, const std::string& call_id) {
    live_calls_.ReportDirectProgress(call_id, phase);
  };
  ports.on_connected = [this](const std::string& call_id) { live_calls_.NoteMediaConnected(call_id); };
  ports.on_connect_failed = [this](const std::string& call_id) { live_calls_.NoteMediaFailed(call_id); };
  ports.on_peer_reconnected = [this](const std::string& call_id) { RestartMedia(call_id, true); };
  ports.on_media_deferred = [this](const std::string& call_id) { live_calls_.NoteMediaDeferred(call_id); };
  ports.on_media_key_ready = [this](const std::string& call_id) { live_calls_.NoteMediaKeyReady(call_id); };
  ports.arming_debug_name = [this]() { return CallMediaStatusName(live_calls_.Status()); };
  return ports;
}

void CallSessionManager::BindSeat() {
  live_calls_.BindMediaResources(&media_, &seat_);
  CallMediaSeatPorts release;
  release.release = [this](const std::string& call_id) { seat_.Release(call_id); };
  media_seat_ports_.Set(std::move(release));
  topology_.SetSeatPorts(MakeTopologySeatPorts());
  seat_.SetTeardownHooks([this](const std::string& call_id) { topology_.OnMediaStopped(call_id); },
                         [this](const std::string& call_id, uint64_t epoch_at_post, bool force) {
                           StopMediaForSeat(call_id, epoch_at_post, force);
                         });
}

void CallSessionManager::StopMediaForSeat(const std::string& call_id, const uint64_t epoch_at_post, const bool force) {
  if (!force && seat_.Epoch() != epoch_at_post) {
    log().info << "MediaSeat stop skip stale call_id=" << call_id << " posted_epoch=" << epoch_at_post
               << " seat_epoch=" << seat_.Epoch();
    return;
  }
  if (direct_driver_) {
    direct_driver_->StopMeshMedia(call_id);
    return;
  }
  if (media_.IsActive() || media_.IsSfuMode()) {
    media_.Stop();
  }
}

void CallSessionManager::AttachDirectPath(CallDirectPathDeps deps) {
  if (!deps.IsUsable()) {
    DropDirectPath();
    return;
  }
  if (!direct_path_ || direct_transport_ != deps.transport) {
    DropDirectPath();
    direct_path_ = std::make_unique<CallMediaBridge>(AsMediaHost(), sessions_, media_keys_, media_, *deps.transport,
                                                     deps.dial, deps.circuit_reach);
    direct_transport_ = deps.transport;
    const uint64_t generation = ++direct_path_generation_;
    direct_path_->SetOutbox(outbox_.For<DirectPathEvent>([generation](DirectPathEvent event) {
      return SessionEvent{session_event::ForDirectPath{generation, std::move(event)}};
    }));
    log().info << "1:1 path built";
  } else {
    direct_path_->SetReachDeps(deps.dial, deps.circuit_reach);
  }
  CallMediaBridge& path = *direct_path_;
  path.SetSeedWarm(std::move(deps.seed_warm));
  path.SetSeedReserve(std::move(deps.seed_reserve));
  path.SetSeedParkAwait(std::move(deps.seed_park_await));
  path.SetPathPolicyProvider(std::move(deps.path_policy));
  path.SetDirectArmingPorts(DirectArmingPorts());
  path.SetSeatPorts(DirectSeatPorts());
  SetDirectMediaPorts(MakeDirectMediaPorts());
  SetDirectDriver(&path);
}

void CallSessionManager::DetachDirectPathReach() {
  if (direct_path_) {
    direct_path_->SetReachDeps(nullptr, nullptr);
  }
}

void CallSessionManager::PrepareDirectPathForStop(const int wait_ms) {
  SetDirectMediaPorts({});
  SetDirectDriver(nullptr);
  if (direct_path_) {
    direct_path_->SetDirectArmingPorts({});
    direct_path_->SetSeatPorts({});
    direct_path_->PrepareForTeardown(wait_ms);
  }
}

void CallSessionManager::DropDirectPath() {
  SetDirectMediaPorts({});
  SetDirectDriver(nullptr);
  direct_path_.reset();
  direct_transport_ = nullptr;
}

void CallSessionManager::OnLocalNetworkMoved() {
  if (direct_path_) {
    direct_path_->OnLocalNetworkChanged();
  }
}

void CallSessionManager::OnPathPolicyChanged(const std::string& call_id) {
  if (direct_path_) {
    direct_path_->OnPathPolicyChanged(call_id);
  }
}

bool CallSessionManager::IsDirectConnectInFlight() const {
  return direct_path_ && direct_path_->IsConnectWorkerInflight();
}

CallDirectMediaPorts CallSessionManager::MakeDirectMediaPorts() {
  CallDirectMediaPorts ports;
  CallMediaBridge* bridge = direct_path_.get();
  if (!bridge) {
    return ports;
  }
  ports.media_path_kind = [bridge]() { return bridge->MediaPathKind(); };
  ports.note_peer_id_relay_mapping = [bridge](const std::string& peer_id, const std::string& relay_identity) {
    bridge->NotePeerIdRelayMapping(peer_id, relay_identity);
  };
  ports.is_connect_failed = [bridge]() { return bridge->IsMeshConnectFailed(); };
  ports.connect_missing_mic = [bridge]() { return bridge->IsMeshConnectFailed() && bridge->MeshConnectMissingMic(); };
  ports.poll_connect_health = [bridge]() { bridge->PollMeshConnectHealth(); };
  ports.media_attempted = [bridge](const std::string& call_id) { return bridge->MediaAttempted(call_id); };
  ports.note_media_attempted = [bridge](const std::string& call_id) { bridge->NoteMediaAttempted(call_id); };
  ports.on_media_key_ready = [bridge](const std::string& call_id) { bridge->OnMediaKeyReady(call_id); };
  return ports;
}

CallDirectSeatPorts CallSessionManager::DirectSeatPorts() {
  CallDirectSeatPorts ports;
  CallMediaSeat* seat = &seat_;
  ports.acquire = [seat](const std::string& call_id) { return seat->Acquire(call_id); };
  ports.allows_path_op = [seat](const CallMediaSeat::Token& token) { return seat->AllowsPathOp(token); };
  ports.current_token = [seat]() { return seat->CurrentToken(); };
  ports.bound_call_id = [seat]() { return seat->BoundCallId(); };
  ports.note_connecting = [seat](const std::string& call_id) { seat->NoteConnecting(call_id); };
  ports.note_start = [seat](const std::string& call_id) { seat->NoteStart(call_id); };
  ports.note_path = [seat](CallMediaSeat::PathKind kind) { seat->NotePath(kind); };
  ports.note_live = [seat](const std::string& call_id) { seat->NoteLive(call_id); };
  ports.note_failed = [seat](const std::string& call_id) { seat->NoteFailed(call_id); };
  return ports;
}

CallTopologySeatPorts CallSessionManager::MakeTopologySeatPorts() {
  CallTopologySeatPorts ports;
  CallMediaSeat* seat = &seat_;
  ports.is_bound = [seat](const std::string& call_id) { return seat->IsBound(call_id); };
  ports.bound_call_id = [seat]() { return seat->BoundCallId(); };
  ports.acquire = [seat](const std::string& call_id) { return seat->Acquire(call_id); };
  ports.allows_path_op = [seat](const CallMediaSeat::Token& token) { return seat->AllowsPathOp(token); };
  ports.begin_attach = [seat](const std::string& call_id, const std::string& hop, CallMediaSeat::AttachTicket* ticket) {
    return seat->BeginAttach(call_id, hop, ticket);
  };
  ports.end_attach_if_matching = [seat](const std::string& call_id, const std::string& hop) {
    seat->EndAttachIfMatching(call_id, hop);
  };
  ports.has_attach_in_flight = [seat]() { return seat->HasAttachInFlight(); };
  ports.attaching_hop = [seat]() { return seat->AttachingHopPeerId(); };
  ports.note_connecting = [seat](const std::string& call_id) { seat->NoteConnecting(call_id); };
  ports.note_start = [seat](const std::string& call_id) { seat->NoteStart(call_id); };
  ports.note_path = [seat](CallMediaSeat::PathKind kind) { seat->NotePath(kind); };
  ports.note_live = [seat](const std::string& call_id) { seat->NoteLive(call_id); };
  ports.cancel_attach_for_call = [seat](const std::string& call_id) { seat->CancelAttachForCall(call_id); };
  return ports;
}

void CallSessionManager::ScheduleStartDirectMedia(const std::string& call_id, const std::string& peer_identity,
                                                  bool offerer) {
  if (!live_calls_.AllowsDirectPath(call_id)) {
    log().info << "ScheduleStartDirectMedia skipped (Status disallows Bridge) call_id=" << call_id
               << " status=" << CallMediaStatusName(live_calls_.Status(call_id))
               << " armed=" << CallArmedPlannerName(live_calls_.ArmedPlanner(call_id));
    return;
  }
  // CSM is signaling-only for duplex start — the call's media coordinator takes the seat and has the
  // direct driver connect.
  log().info << "ScheduleStartDirectMedia libp2p role=" << (offerer ? "offerer" : "answerer")
                << " call_id=" << call_id << " peer=" << peer_identity;
  CallMediaCoordinator* call_media = live_calls_.Media(call_id);
  if (auto begun = call_media ? call_media->BeginDirect(peer_identity, offerer)
                              : Roe<void>(Error("no live call for media"));
      !begun) {
    log().error << "ScheduleStartDirectMedia: " << begun.error().message << " call_id=" << call_id;
    last_media_error_ = "Call media unavailable";
    NotifyRingChanged();
  }
}

void CallSessionManager::KickAnswererDirectMediaIfArmed(const std::string& call_id) {
  if (call_id.empty()) {
    log().info << "KickAnswererDirectMediaIfArmed skip (empty call_id)";
    return;
  }
  std::string peer;
  if (!workflow_.PeekPendingAnswererKick(call_id, &peer)) {
    auto resolved = PeerIdentityForCall(call_id);
    if (resolved && resolved->has_value()) {
      peer = **resolved;
    }
  }
  CallAnswererKickDecisionInput in;
  in.allows_direct_path = live_calls_.AllowsDirectPath(call_id);
  // IsActive alone — do not require tx/connected (capture lags StartSfu; dogfood e157 thrash).
  in.media_already_active_same_call = media_.IsActive() && media_.ActiveCallId() == call_id;
  in.peer_nonempty = !peer.empty();
  if (!ShouldKickAnswererDirectMedia(in)) {
    if (!in.allows_direct_path) {
      log().info << "KickAnswererDirectMediaIfArmed skip (Status disallows Bridge) call_id=" << call_id
                 << " status=" << CallMediaStatusName(live_calls_.Status(call_id));
    } else if (in.media_already_active_same_call) {
      log().info << "KickAnswererDirectMediaIfArmed skip (media already active) call_id=" << call_id;
    } else {
      log().warning << "KickAnswererDirectMediaIfArmed no peer call_id=" << call_id;
    }
    return;
  }
  log().info << "KickAnswererDirectMediaIfArmed call_id=" << call_id << " peer=" << peer;
  ScheduleStartDirectMedia(call_id, peer, false);
}

CallHopHealth CallSessionManager::HopHealth() const {
  if (!topology_.IsSfuAttached()) {
    return {};
  }
  // Topology holds relay deps; sample via public IsSfuAttached + Media PathPressure elsewhere.
  return topology_.HopHealth();
}

std::string CallSessionManager::MediaPathKind() const {
  const auto direct_media = direct_media_.Get();
  if (!direct_media->media_path_kind) {
    return {};
  }
  return direct_media->media_path_kind();
}

bool CallSessionManager::IsSfuAttached() const {
  return topology_.IsSfuAttached();
}

CallMediaEngine& CallSessionManager::Media() {
  return media_;
}

Roe<void> CallSessionManager::SetLocalAudioMuted(bool muted) {
  auto local = control_.LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  const std::string call_id = live_calls_.MediaRunningCallId();
  CallMediaCoordinator* call_media = call_id.empty() ? nullptr : live_calls_.Media(call_id);
  if (!call_media) {
    return Error("No active call media");
  }
  call_media->SetMuted(muted);
  auto participant = sessions_.FindParticipant(call_id, *local);
  if (participant && participant->has_value()) {
    (*participant)->media.audio_muted = muted;
    (void)sessions_.UpsertParticipant(**participant);
  }
  auto roster = BuildRosterDetail(call_id);
  if (roster) {
    auto roster_json = CallControlCodec::EncodeRoster(*roster);
    if (roster_json) {
      (void)control_.FanOutToJoined(call_id, CallControlType::CallRoster, *roster_json, "Call roster", *local);
    }
  }
  NotifyRingChanged();
  return {};
}

Roe<void> CallSessionManager::SetLocalVideoEnabled(bool enabled, const int display_rotation_degrees) {
  auto local = control_.LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  const std::string call_id = live_calls_.MediaRunningCallId();
  CallMediaCoordinator* call_media = call_id.empty() ? nullptr : live_calls_.Media(call_id);
  if (!call_media) {
    return Error("No active call media");
  }
  if (enabled) {
    auto session = sessions_.LoadSession(call_id);
    if (!session || !session->has_value() || !(*session)->video_allowed) {
      return Error("Video is not allowed for this call");
    }
  }
  if (auto cam = call_media->SetCamera(enabled, display_rotation_degrees); !cam) {
    return cam.error();
  }
  auto participant = sessions_.FindParticipant(call_id, *local);
  if (participant && participant->has_value()) {
    (*participant)->media.video_enabled = enabled && call_media->CameraOn();
    (void)sessions_.UpsertParticipant(**participant);
  }
  auto roster = BuildRosterDetail(call_id);
  if (roster) {
    auto roster_json = CallControlCodec::EncodeRoster(*roster);
    if (roster_json) {
      (void)control_.FanOutToJoined(call_id, CallControlType::CallRoster, *roster_json, "Call roster", *local);
    }
  }
  NotifyRingChanged();
  return {};
}

Roe<void> CallSessionManager::RequestVideoRefresh(const std::string& call_id,
                                                 const std::string& publisher_identity) {
  auto local = control_.LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  if (call_id.empty()) {
    return Error("No active call");
  }
  if (publisher_identity.empty() || publisher_identity == *local) {
    if (CallMediaCoordinator* call_media = live_calls_.Media(call_id)) {
      call_media->RequestKeyframe();
    }
    return {};
  }
  CallVideoRefreshDetail detail;
  detail.call_id = call_id;
  detail.identity = publisher_identity;
  auto encoded = CallControlCodec::EncodeVideoRefresh(detail);
  if (!encoded) {
    return encoded.error();
  }
  return control_.SendDirect(publisher_identity, CallControlType::CallVideoRefresh, *encoded, "");
}

void CallSessionManager::ClearLastMediaErrorIf(const std::string& seen) {
  if (last_media_error_ && *last_media_error_ == seen) {
    last_media_error_.reset();
  }
}

std::optional<std::string> CallSessionManager::TakeLastMediaError() {
  auto out = std::move(last_media_error_);
  last_media_error_.reset();
  return out;
}

void CallSessionManager::SetOnRingChanged(RingChangedFn callback) {
  on_ring_changed_ = std::move(callback);
}

void CallSessionManager::SetOnRingChangedMesh(RingChangedFn callback) {
  on_ring_changed_mesh_ = std::move(callback);
}

void CallSessionManager::SetPrefetchPeerReachability(PrefetchPeerReachFn callback) {
  prefetch_reach_ = std::move(callback);
}

void CallSessionManager::SetEnsureCircuitReady(EnsureCircuitReadyFn callback) {
  ensure_circuit_ready_ = std::move(callback);
}

void CallSessionManager::SetParkCircuit(ParkCircuitFn park) {
  park_circuit_ = std::move(park);
}

void CallSessionManager::SetLocalListenMultiaddrsProvider(LocalListenMultiaddrsFn callback) {
  local_listen_multiaddrs_ = std::move(callback);
}

void CallSessionManager::SetLocalPeerCapsProvider(LocalPeerCapsFn callback) {
  local_peer_caps_ = std::move(callback);
}

void CallSessionManager::SetCallPeerCapsSink(CallPeerCapsSink sink) { call_peer_caps_sink_ = std::move(sink); }

void CallSessionManager::SetLocalMeshPeerIdProvider(LocalMeshPeerIdFn callback) {
  local_mesh_peer_id_ = std::move(callback);
}

void CallSessionManager::SetRegisterPeerListenMultiaddrs(RegisterPeerListenMultiaddrsFn callback) {
  register_peer_listen_multiaddrs_ = std::move(callback);
}

void CallSessionManager::NotePeerMediaRelayCap(const std::string& peer_id, bool media_relay) {
  // Topology owns SoftMigrate nudge (N≥3 / attach-wait only — V038).
  if (media_relay_caps_.Note(peer_id, media_relay)) {
    if (auto active = ActiveLocalCall(); active && active->has_value()) {
      topology_.OnPeerMediaRelayCapLearned((*active)->call_id, peer_id);
    }
  }
}

void CallSessionManager::NoteMeshPeerIdForRelay(const std::string& relay_identity, const std::string& peer_id) {
  if (!peer_accounts_.Learn(relay_identity, peer_id)) {
    return;
  }
  const auto direct_media = direct_media_.Get();
  if (direct_media->note_peer_id_relay_mapping) {
    direct_media->note_peer_id_relay_mapping(peer_id, relay_identity);
  }
}

bool CallSessionManager::PeerHasMediaRelayCap(const std::string& peer_id) const {
  return media_relay_caps_.Has(peer_id);
}

std::vector<std::string> CallSessionManager::ListMediaRelayCapablePeerIds() const {
  return media_relay_caps_.ListCapable();
}

void CallSessionManager::NotifyRingChanged() {
  if (on_ring_changed_mesh_) {
    on_ring_changed_mesh_();
  }
  if (on_ring_changed_) {
    on_ring_changed_();
  }
}





Roe<CallRosterDetail> CallSessionManager::BuildRosterDetail(const std::string& call_id) const {
  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value()) {
    return Error("Call session not found");
  }
  auto participants = sessions_.ListParticipants(call_id);
  if (!participants) {
    return participants.error();
  }
  CallRosterDetail detail;
  detail.call_id = call_id;
  detail.media_epoch = (*session)->media_epoch;
  for (const CallParticipant& row : *participants) {
    CallRosterEntry entry;
    entry.identity = row.identity;
    entry.state = row.state;
    entry.audio_muted = row.media.audio_muted;
    entry.video_enabled = row.media.video_enabled;
    entry.joined_at = row.joined_at;
    detail.participants.push_back(std::move(entry));
  }
  return detail;
}




void CallSessionManager::StopCallMedia(const std::string& call_id) {
  StopMediaIfCall(call_id);
}

void CallSessionManager::StopMediaIfCall(const std::string& call_id) {
  // CSM is signaling-only for duplex stop: the call registry releases the seat (Detach-then-Stop).
  live_calls_.StopMedia(call_id);
}



Roe<CallSession> CallSessionManager::StartCall(const std::string& origin_thread_id, const bool video_allowed,
                                               const std::vector<std::string>& invitee_identities) {
  auto started = workflow_.StartCall(origin_thread_id, video_allowed, invitee_identities);
  if (started) {
    // Circuit path often chooses R1 before Invite (product-stack / hard-w5); flush wire announce.
    reach_signals_.FlushCircuitR1();
  }
  return started;
}


Roe<void> CallSessionManager::InviteParticipant(const std::string& call_id, const std::string& invitee_identity) {
  return workflow_.InviteParticipant(call_id, invitee_identity);
}


int64_t CallSessionManager::InitiationOfferMinorForPeer(const std::string& peer_identity) const {
  return billing_.OfferFrom(peer_identity);
}


void CallSessionManager::SetPendingAcceptChargeDecision(const InitiationChargeDecision decision) {
  workflow_.SetPendingAcceptChargeDecision(decision);
}


void CallSessionManager::SetPendingAcceptVoiceOnly(const bool voice_only) {
  workflow_.SetPendingAcceptVoiceOnly(voice_only);
}


void CallSessionManager::AcceptInviteAsync(const std::string& call_id, std::function<void(Roe<void>)> on_done,
                                           InitiationChargeDecision charge_decision) {
  workflow_.AcceptInviteAsync(call_id, charge_decision, std::move(on_done));
}





Roe<void> CallSessionManager::DeclineInvite(const std::string& call_id) {
  return workflow_.DeclineInvite(call_id);
}


Roe<void> CallSessionManager::MaybeRotateMediaKey(const std::string& call_id, const std::string& leaver_identity) {
  return workflow_.MaybeRotateMediaKey(call_id, leaver_identity);
}


Roe<void> CallSessionManager::EndCallLocal(CallSession& session, const std::optional<int64_t>& duration_ms,
                                           const LiveCallEndReason reason) {
  auto result = workflow_.EndCallLocal(session, duration_ms, reason);
  MaybeCatchUpAfterCall();
  return result;
}


Roe<void> CallSessionManager::LeaveCall(const std::string& call_id, const LiveCallEndReason reason) {
  auto result = workflow_.LeaveCall(call_id, reason);
  MaybeCatchUpAfterCall();
  return result;
}

void CallSessionManager::MaybeCatchUpAfterCall() {
  if (!delivery_.catch_up_after_call) {
    return;
  }
  auto active = ActiveLocalCall();
  if (active && *active) {
    return;
  }
  delivery_.catch_up_after_call();
}


Roe<std::vector<PendingCallInvite>> CallSessionManager::ListPendingInvites() {
  SweepExpiredInvites();
  auto local = control_.LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  return sessions_.ListPendingInvitesForInvitee(*local);
}

Roe<std::optional<CallSession>> CallSessionManager::ActiveLocalCall() const {
  return workflow_.ActiveLocalCall();
}

Roe<std::optional<PendingCallInvite>> CallSessionManager::TopPendingInvite() {
  return workflow_.TopPendingInvite();
}

Roe<std::optional<std::string>> CallSessionManager::PeerIdentityForCall(const std::string& call_id) const {
  auto local = control_.LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto participants = sessions_.ListParticipants(call_id);
  if (!participants) {
    return participants.error();
  }
  for (const CallParticipant& row : *participants) {
    if (row.identity != *local && !row.identity.empty()) {
      return std::optional<std::string>{row.identity};
    }
  }
  return std::optional<std::string>{};
}

Roe<std::optional<bool>> CallSessionManager::PeerVideoEnabledForCall(const std::string& call_id) const {
  auto local = control_.LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto participants = sessions_.ListParticipants(call_id);
  if (!participants) {
    return participants.error();
  }
  for (const CallParticipant& row : *participants) {
    if (row.identity != *local && !row.identity.empty()) {
      return std::optional<bool>{row.media.video_enabled};
    }
  }
  return std::optional<bool>{};
}

Roe<std::optional<bool>> CallSessionManager::VideoAllowedForCall(const std::string& call_id) const {
  auto session = sessions_.LoadSession(call_id);
  if (!session) {
    return session.error();
  }
  if (!session->has_value()) {
    return std::optional<bool>{};
  }
  return std::optional<bool>{(*session)->video_allowed};
}

Roe<bool> CallSessionManager::AwaitingExplicitAnswerForCall(const std::string& call_id) const {
  auto participants = sessions_.ListParticipants(call_id);
  if (!participants) {
    return participants.error();
  }
  for (const CallParticipant& p : *participants) {
    auto invite = sessions_.LoadPendingInvite(call_id, p.identity);
    if (invite && invite->has_value() && (*invite)->status == "accepted_implicit") {
      return true;
    }
  }
  return false;
}

Roe<std::vector<CallParticipant>> CallSessionManager::ListJoinedParticipants(const std::string& call_id) const {
  auto participants = sessions_.ListParticipants(call_id);
  if (!participants) {
    return participants.error();
  }
  std::vector<CallParticipant> joined;
  joined.reserve(participants->size());
  for (const CallParticipant& row : *participants) {
    if (row.state == CallParticipantState::Joined) {
      joined.push_back(row);
    }
  }
  return joined;
}

bool CallSessionManager::IsAwaitingSfuRecovery() const {
  return topology_.IsAwaitingSfuRecovery();
}

bool CallSessionManager::IsSoftMigrateInFlight() const {
  return topology_.IsSoftMigrateInFlight();
}

bool CallSessionManager::IsSfuAttachWaitActive() const {
  return topology_.IsSfuAttachWaitActive();
}

bool CallSessionManager::IsP2pConnectFailed() const {
  const auto direct_media = direct_media_.Get();
  return direct_media->is_connect_failed && direct_media->is_connect_failed();
}

bool CallSessionManager::P2pConnectMissingMic() const {
  const auto direct_media = direct_media_.Get();
  return direct_media->connect_missing_mic && direct_media->connect_missing_mic();
}

void CallSessionManager::PollP2pConnectHealth() {
  const auto direct_media = direct_media_.Get();
  if (direct_media->poll_connect_health) {
    direct_media->poll_connect_health();
  }
}

Roe<void> CallSessionManager::RetryP2pMedia(const std::string& call_id) {
  const auto direct_media = direct_media_.Get();
  CallMediaCoordinator* call_media = live_calls_.Media(call_id);
  if (call_media && direct_media->media_attempted && direct_media->media_attempted(call_id)) {
    return call_media->Retry();
  }
  return Error("Call media retry unavailable");
}

Roe<void> CallSessionManager::ResumeP2pMedia(const std::string& call_id) {
  CallMediaCoordinator* call_media = live_calls_.Media(call_id);
  return call_media ? call_media->ResumeFromInbound() : Roe<void>(Error("Call media resume unavailable"));
}

Roe<std::optional<CallSession>> CallSessionManager::SessionForCall(const std::string& call_id) const {
  return sessions_.LoadSession(call_id);
}

void CallSessionManager::SweepExpiredInvites() {
  workflow_.SweepExpiredInvites();
}


void CallSessionManager::AbandonOrphanedCallsAfterRestart() {
  workflow_.AbandonOrphanedCallsAfterRestart();
}


bool CallSessionManager::MediaAttemptedThisProcess(const std::string& call_id) const {
  const auto direct_media = direct_media_.Get();
  return direct_media->media_attempted && direct_media->media_attempted(call_id);
}

void CallSessionManager::ClearMediaCallbacks() {
  // Drop accepts waiting on their park before CallStack drains / resets CSM (PR #216 follow-up).
  workflow_.DropFollowUps();
}

Roe<void> CallSessionManager::HandleInboundInvite(const std::string& detail_json,
                                                  const std::string& sender_identity,
                                                  const ThreadMessage& message,
                                                  const std::optional<int64_t> relay_created_at_ms,
                                                  const std::optional<int64_t> relay_server_time_ms,
                                                  const std::string& local_identity) {
  return workflow_.HandleInboundInvite(detail_json, sender_identity, message, relay_created_at_ms, relay_server_time_ms, local_identity);
}


Roe<void> CallSessionManager::HandleInboundAccept(const std::string& detail_json,
                                                  const std::string& sender_identity,
                                                  const std::string& local_identity) {
  return workflow_.HandleInboundAccept(detail_json, sender_identity, local_identity);
}


Roe<void> CallSessionManager::HandleInboundDecline(const std::string& detail_json,
                                                   const std::string& sender_identity) {
  return workflow_.HandleInboundDecline(detail_json, sender_identity);
}


Roe<void> CallSessionManager::HandleInboundLeave(const std::string& detail_json,
                                                 const std::string& sender_identity,
                                                 const std::string& local_identity) {
  return workflow_.HandleInboundLeave(detail_json, sender_identity, local_identity);
}


Roe<void> CallSessionManager::HandleInboundRoster(const std::string& detail_json) {
  return workflow_.HandleInboundRoster(detail_json);
}


Roe<void> CallSessionManager::HandleInboundSfuAttach(const std::string& detail_json,
                                                    const std::string& sender_identity) {
  return workflow_.HandleInboundSfuAttach(detail_json, sender_identity);
}


Roe<void> CallSessionManager::HandleInboundSfuAttachFailed(const std::string& detail_json,
                                                           const std::string& sender_identity) {
  return workflow_.HandleInboundSfuAttachFailed(detail_json, sender_identity);
}


Roe<void> CallSessionManager::HandleInboundHopRefuse(const std::string& detail_json) {
  return workflow_.HandleInboundHopRefuse(detail_json);
}


Roe<void> CallSessionManager::HandleInboundVideoRefresh(const std::string& detail_json,
                                                        const std::string& sender_identity) {
  return workflow_.HandleInboundVideoRefresh(detail_json, sender_identity);
}

Roe<void> CallSessionManager::HandleInboundEnded(const std::string& detail_json,
                                                 const std::string& local_identity) {
  return workflow_.HandleInboundEnded(detail_json, local_identity);
}


Roe<void> CallSessionManager::ApplyInboundControl(ThreadMessage& message, const std::string& sender_identity,
                                                  const std::optional<int64_t> relay_created_at_ms,
                                                  const std::optional<int64_t> relay_server_time_ms) {
  auto type = CallControlCodec::ControlTypeFromMessage(message);
  if (!type) {
    return {};
  }
  auto payload = TryParseObject(message.payload_json);
  auto detail = payload ? payload->getString("detail") : std::nullopt;
  if (!detail) {
    return Error("Call control missing detail");
  }
  const std::string detail_json = *detail;
  auto local = control_.LocalRelayIdentity();
  if (!local) {
    return local.error();
  }

  switch (*type) {
  case CallControlType::CallInvite:
    return HandleInboundInvite(detail_json, sender_identity, message, relay_created_at_ms, relay_server_time_ms,
                               *local);
  case CallControlType::CallAccept:
    return HandleInboundAccept(detail_json, sender_identity, *local);
  case CallControlType::CallDecline:
    return HandleInboundDecline(detail_json, sender_identity);
  case CallControlType::CallLeave:
    return HandleInboundLeave(detail_json, sender_identity, *local);
  case CallControlType::CallRoster:
    return HandleInboundRoster(detail_json);
  case CallControlType::CallMediaKey:
    return key_exchange_.HandleInbound(detail_json, sender_identity);
  case CallControlType::CallSdp:
  case CallControlType::CallIce:
    log().debug << "Ignoring legacy call_sdp/call_ice from " << sender_identity;
    return {};
  case CallControlType::CallSfuAttach:
    return HandleInboundSfuAttach(detail_json, sender_identity);
  case CallControlType::CallSfuAttachFailed:
    return HandleInboundSfuAttachFailed(detail_json, sender_identity);
  case CallControlType::CallHopRefuse:
    return HandleInboundHopRefuse(detail_json);
  case CallControlType::CallVideoRefresh:
    return HandleInboundVideoRefresh(detail_json, sender_identity);
  case CallControlType::CallCircuitR1:
    return reach_signals_.HandleInboundCircuitR1(detail_json);
  case CallControlType::CallCapsUpdate:
    return reach_signals_.HandleInboundCapsUpdate(detail_json);
  case CallControlType::CallPunchOffer:
    return reach_signals_.HandleInboundPunchOffer(detail_json, sender_identity);
  case CallControlType::CallPunchAnswer:
    return reach_signals_.HandleInboundPunchAnswer(detail_json);
  case CallControlType::CallEnded:
    return HandleInboundEnded(detail_json, *local);
  case CallControlType::CallStarted:
    return {};
  }
  return {};
}


Roe<std::string> CallSessionManager::TopologyLocalIdentity() const {
  return control_.LocalRelayIdentity();
}

Roe<void> CallSessionManager::TopologyLeaveCall(const std::string& call_id) {
  // Topology leaves when the group media path cannot be kept (attach-wait timeout, failed migrate).
  return LeaveCall(call_id, LiveCallEndReason::MediaUnavailable);
}

Roe<void> CallSessionManager::TopologyFanOutToJoined(const std::string& call_id, CallControlType type,
                                                     const std::string& detail_json, const std::string& display,
                                                     const std::string& skip_identity) {
  return control_.FanOutToJoined(call_id, type, detail_json, display, skip_identity);
}

Roe<void> CallSessionManager::TopologySendDirect(const std::string& peer_identity, CallControlType type,
                                                 const std::string& detail_json, const std::string& display) {
  return control_.SendDirect(peer_identity, type, detail_json, display);
}

void CallSessionManager::TopologyNotifyRingChanged() {
  NotifyRingChanged();
}

void CallSessionManager::TopologySetLastMediaError(std::string message) {
  last_media_error_ = std::move(message);
}

void CallSessionManager::TopologySetMediaActivity(std::string message) {
  media_activity_ = std::move(message);
}

void CallSessionManager::TopologyClearMediaActivity() {
  media_activity_.clear();
}

std::string CallSessionManager::PeekMediaActivity() const {
  return media_activity_;
}

void CallSessionManager::ClearMediaActivity() {
  media_activity_.clear();
}

void CallSessionManager::TopologyNoteMediaAttempted(const std::string& call_id) {
  const auto direct_media = direct_media_.Get();
  if (direct_media->note_media_attempted) {
    direct_media->note_media_attempted(call_id);
  }
}

void CallSessionManager::TopologyClearMediaPeerIdentity() {
}

void CallSessionManager::TopologyRequestInboxSync() {
  if (delivery_.sync_inbox_from_wake) {
    delivery_.sync_inbox_from_wake(true);
  }
}

Roe<std::string> CallSessionManager::P2pLocalIdentity() const {
  return control_.LocalRelayIdentity();
}

Roe<void> CallSessionManager::P2pSendDirect(const std::string& peer_identity, CallControlType type,
                                            const std::string& detail_json, const std::string& display) {
  return control_.SendDirect(peer_identity, type, detail_json, display);
}

void CallSessionManager::P2pNotifyRingChanged() {
  NotifyRingChanged();
}

void CallSessionManager::P2pSetLastMediaError(std::string message) {
  last_media_error_ = std::move(message);
}

Roe<std::optional<std::string>> CallSessionManager::P2pPeerIdentityForCall(const std::string& call_id) const {
  return PeerIdentityForCall(call_id);
}

Roe<std::optional<std::string>> CallSessionManager::MeshPeerIdForAccount(const std::string& account) const {
  return peer_accounts_.PeerIdForAccount(account);
}

Roe<std::optional<std::string>> CallSessionManager::RelayIdentityForMeshPeerId(
    const std::string& call_id, const std::string& peer_id) const {
  // Prefer the call's participants whose contact carries the inbound stream's PeerId.
  std::vector<std::string> participants;
  if (!call_id.empty() && !peer_id.empty()) {
    auto rows = sessions_.ListParticipants(call_id);
    if (!rows) {
      return rows.error();
    }
    for (const CallParticipant& row : *rows) {
      participants.push_back(row.identity);
    }
  }
  return peer_accounts_.AccountForPeerId(peer_id, participants);
}

void CallSessionManager::P2pResendMediaKey(const std::string& call_id, const std::string& peer_identity) {
  if (call_id.empty() || peer_identity.empty()) {
    return;
  }
  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value() || (*session)->state == CallSessionState::Ended) {
    return;
  }
  (void)key_exchange_.SendCurrent(call_id, peer_identity);
}

bool CallSessionManager::HopCarriesMedia() const {
  return topology_.IsSfuAttached();  // an atomic flag: safe from the transport's I/O
}

void CallSessionManager::P2pNoteInboundHello(const std::string& call_id, const std::string& identity,
                                             const std::string& peer_id) {
  auto local = control_.LocalRelayIdentity();
  if (!local) {
    return;
  }
  if (auto applied = workflow_.ApplyImplicitAccept(call_id, identity, peer_id, *local); !applied) {
    log().warning << "implicit accept failed call_id=" << call_id << " err=" << applied.error().message;
  }
}

void CallSessionManager::P2pRequestInboxSync() {
  if (delivery_.sync_inbox_from_wake) {
    delivery_.sync_inbox_from_wake(true);
  }
}

} // namespace pbr
