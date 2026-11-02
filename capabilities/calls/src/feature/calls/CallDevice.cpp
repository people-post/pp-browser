#include "feature/calls/CallDevice.h"

#include "lib/calls/CallSessionQueries.h"
#include "feature/calls/CallControlClient.h"
#include "lib/calls/CallListenAddrsLogic.h"
#include "lib/calls/CallAnswererKickLogic.h"
#include "lib/calls/CallMediaPlannerSelectLogic.h"

#include "foundation/crypto/CryptoUtil.h"
#include "lib/calls/CallMediaAdaptation.h"
#include "lib/calls/CallSessionLogic.h"
#include "lib/delivery/DirectChatTargetFromContact.h"
#include "lib/calls/PeerCapsLogic.h"
#include "lib/delivery/SendRelayOptions.h"
#include "lib/people/ContactIdentity.h"
#include "lib/people/ContactTypes.h"
#include "lib/people/MeshHopPolicy.h"
#include "common/runtime/AppRuntime.h"
#include "common/Utilities.h"

#include <algorithm>
#include <type_traits>
#include <variant>

#include "common/ValueJson.h"
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

namespace {

void PrefetchReachForIdentity(const std::function<void(const std::string&)>& fn, const std::string& identity) {
  if (!identity.empty() && fn) {
    fn(identity);
  }
}

void NoteCapsForIdentity(CallDevice& sessions, const IContactBook& contacts, const std::string& identity,
                         const CallPeerCaps& caps, const std::vector<std::string>& listen_multiaddrs) {
  if (!caps.present && listen_multiaddrs.empty()) {
    return;
  }
  std::vector<std::string> peer_ids = PeerIdsFromListenMultiaddrs(listen_multiaddrs);
  if (peer_ids.empty() && !identity.empty()) {
    if (auto found = contacts.FindByIdentity(identity, mesh::ContactIdKind::Account); found && found->has_value()) {
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

CallDevice::CallDevice(const IThreadCatalog& threads, IThreadTranscript& transcript, IContactBook& contacts,
                       const ILocalAccount& account, ICallSessionStore& sessions, const CallReads& reads,
                       ICallMediaKeyStore& media_keys, CallDeliveryPorts delivery, MediaEngine& media)
    : contacts_(contacts),
      sessions_(sessions),
      reads_(reads),
      media_keys_(media_keys),
      delivery_(std::move(delivery)),
      media_(media),
      hop_service_(sessions, contacts),
      control_(transcript, account, sessions, delivery_),
      key_exchange_(
          sessions, media_keys,
          CallMediaKeyExchange::Transport{
              [this](const std::string& peer) { return control_.SessionKeyWith(peer); },
              [this](const std::string& call_id, const std::string& peer, uint32_t epoch, const std::string& key_id,
                     const ByteVector& key) { return control_.SendMediaKey(call_id, peer, epoch, key_id, key); }}),
      billing_(account),
      reach_signals_(control_),
      workflow_(threads, sessions, reads, hop_service_, key_exchange_, billing_, live_calls_, call_media_),
      peer_accounts_(contacts) {
  redirectLogger("CallDevice");
  key_exchange_.SetOnKeyReady([this](const std::string& call_id) {
    if (doorway_) {
      doorway_->NotifyKeyAvailable(); // wake inbound hellos waiting for a key
    }
    if (CallMedia* call = call_media_.Find(call_id)) {
      call->OnMediaKeyReady();
    }
  });
  BindSeat();
  call_media_.BindHopPathFactory([this](const std::string& call_id, const uint64_t serial,
                                        std::shared_ptr<CallEngine> engine,
                                        std::shared_ptr<std::atomic<bool>> carries) {
    return MakeHopPath(call_id, serial, std::move(engine), std::move(carries));
  });
  live_calls_.SetOnChanged([this]() { NotifyCallStateChanged(); });
  BindWorkflowHostPorts();
  BindReachSignalPorts();
}

CallHopPath::HostPorts CallDevice::MakeTopologyHostPorts() {
  CallHopPath::HostPorts ports;
  ports.local_relay_identity = [this]() { return LocalIdentity(); };
  ports.call_media = [this](const std::string& call_id) { return call_media_.Media(call_id); };
  ports.fan_out_joined = [this](const std::string& call_id, CallControlType type, const std::string& detail,
                                const std::string& display, const std::string& skip) {
    return TopologyFanOutToJoined(call_id, type, detail, display, skip);
  };
  ports.send_direct = [this](const std::string& peer, CallControlType type, const std::string& detail,
                             const std::string& display) { return SendDirect(peer, type, detail, display); };
  return ports;
}

void CallDevice::OnHopReport(const HopReport& report) {
  std::visit(
      [this](const auto& r) {
        using R = std::decay_t<decltype(r)>;
        if constexpr (std::is_same_v<R, hop_report::Progress>) {
          live_calls_.ReportHopProgress(r.call_id, r.phase);
        } else if constexpr (std::is_same_v<R, hop_report::Changed>) {
          NotifyRingChanged();
        } else if constexpr (std::is_same_v<R, hop_report::MediaError>) {
          SetLastMediaError(r.message);
        } else if constexpr (std::is_same_v<R, hop_report::Activity>) {
          live_calls_.NoteActivity(r.call_id, r.text);
        } else if constexpr (std::is_same_v<R, hop_report::MediaAttempted>) {
          TopologyNoteMediaAttempted(r.call_id);
        } else if constexpr (std::is_same_v<R, hop_report::InboxSyncWanted>) {
          RequestInboxSync();
        } else if constexpr (std::is_same_v<R, hop_report::LeaveWanted>) {
          (void)TopologyLeaveCall(r.call_id);
        } else {
          static_assert(!sizeof(R), "take every HopReport");
        }
      },
      report);
}

void CallDevice::BindWorkflowHostPorts() {
  CallSessionWorkflow::HostPorts ports;
  ports.wire = MakeWorkflowWirePorts();
  ports.duplex.schedule_start_direct = [this](const std::string& call_id, const std::string& peer, bool offerer) {
    ScheduleStartDirectMedia(call_id, peer, offerer);
  };
  ports.reach = MakeWorkflowReachPorts();
  workflow_.SetHostPorts(std::move(ports));
}

CallSessionWorkflow::WirePorts CallDevice::MakeWorkflowWirePorts() {
  CallSessionWorkflow::WirePorts ports;
  ports.local_relay_identity = [this]() { return control_.LocalRelayIdentity(); };
  ports.send_direct = [this](const std::string& peer, CallControlType type, const std::string& detail,
                             const std::string& display) { return control_.SendDirect(peer, type, detail, display); };
  ports.fan_out_joined = [this](const std::string& call_id, CallControlType type, const std::string& detail,
                                const std::string& display, const std::string& skip) {
    return control_.FanOutToJoined(call_id, type, detail, display, skip);
  };
  ports.fan_out_joined_and_ringing = [this](const std::string& call_id, CallControlType type, const std::string& detail,
                                            const std::string& display, const std::string& skip) {
    return control_.FanOutToJoinedAndRinging(call_id, type, detail, display, skip);
  };
  ports.send_summary = [this](const std::string& thread_id, const CallSummaryDetail& summary, bool local_only,
                              SummaryWrite write) {
    return control_.SendSummary(thread_id, summary, local_only, write);
  };
  ports.resolve_call_thread = [this](const std::string& inviter,
                                     const std::optional<std::string>& group_id) -> std::optional<std::string> {
    if (group_id && !group_id->empty()) {
      return delivery_.resolve_group_thread ? delivery_.resolve_group_thread(*group_id) : std::nullopt;
    }
    if (!delivery_.resolve_peer_thread) {
      return std::nullopt;
    }
    auto thread = delivery_.resolve_peer_thread(inviter);
    if (!thread) {
      log().warning << "no thread for the call from " << inviter << ": " << thread.error().message;
      return std::nullopt;
    }
    return *thread;
  };
  ports.build_roster_detail = [this](const std::string& call_id) { return BuildRosterDetail(call_id); };
  ports.group_members = delivery_.group_members;
  return ports;
}

void CallDevice::OnWorkflowReport(const WorkflowReport& report) {
  std::visit(
      [this](const auto& r) {
        using R = std::decay_t<decltype(r)>;
        if constexpr (std::is_same_v<R, workflow_report::Changed>) {
          NotifyRingChanged();
        } else if constexpr (std::is_same_v<R, workflow_report::Notice>) {
          last_error_ = r.message;
          NotifyCallStateChanged();
        } else if constexpr (std::is_same_v<R, workflow_report::InboxSyncFromWake>) {
          if (delivery_.sync_inbox_from_wake) {
            delivery_.sync_inbox_from_wake(true);
          }
        } else {
          static_assert(!sizeof(R), "take every WorkflowReport");
        }
      },
      report);
}

CallSessionWorkflow::ReachPorts CallDevice::MakeWorkflowReachPorts() {
  CallSessionWorkflow::ReachPorts ports;
  ports.register_peer_listen = [this](const std::string& identity, const std::vector<std::string>& mas) {
    if (mesh_.register_peer_listen_multiaddrs) {
      mesh_.register_peer_listen_multiaddrs(identity, mas);
    }
  };
  ports.note_caps_for_identity = [this](const std::string& identity, const CallPeerCaps& caps,
                                        const std::vector<std::string>& listen) {
    NoteCapsForIdentity(*this, contacts_, identity, caps, listen);
  };
  ports.note_call_peer_caps = [this](const std::string& call_id, const CallPeerCaps& caps) {
    if (mesh_.on_peer_caps) {
      mesh_.on_peer_caps(call_id, caps);
    }
  };
  ports.parked_relays = [this]() -> std::vector<std::string> {
    return mesh_.parked_relays ? mesh_.parked_relays() : std::vector<std::string>{};
  };
  ports.note_call_peer_parked_relays = [this](const std::string& call_id, const std::vector<std::string>& relays) {
    if (mesh_.on_peer_parked_relays) {
      mesh_.on_peer_parked_relays(call_id, relays);
    }
  };
  ports.prefetch_reach = [this](const std::string& identity) {
    PrefetchReachForIdentity(mesh_.prefetch_peer_reach, identity);
  };
  ports.ensure_circuit_ready = [this]() {
    if (mesh_.ensure_circuit_ready) {
      mesh_.ensure_circuit_ready();
    }
  };
  ports.park_circuit = [this](int timeout_ms, std::function<void(bool)> done) {
    if (mesh_.park_circuit) {
      mesh_.park_circuit(timeout_ms, std::move(done));
    } else {
      done(false);
    }
  };
  ports.note_mesh_peer_id_for_relay = [this](const std::string& relay, const std::string& peer_id) {
    NoteMeshPeerIdForRelay(relay, peer_id);
  };
  ports.local_listen_multiaddrs = [this]() -> std::vector<std::string> {
    return mesh_.local_listen_multiaddrs ? mesh_.local_listen_multiaddrs() : std::vector<std::string>{};
  };
  ports.may_learn_our_address = [this](const std::string& peer) {
    return AllowsDirect(mesh_.address_disclosure, peer);
  };
  ports.local_peer_caps = [this]() -> CallPeerCaps {
    return mesh_.local_peer_caps ? mesh_.local_peer_caps() : CallPeerCaps{};
  };
  ports.local_mesh_peer_id = [this]() -> std::string {
    return mesh_.local_mesh_peer_id ? mesh_.local_mesh_peer_id() : std::string{};
  };
  return ports;
}

void CallDevice::BindReachSignalPorts() {
  CallReachSignals::CallPorts ports;
  ports.active_call_id = [this]() -> std::optional<std::string> {
    auto active = reads_.ActiveLocalCall();
    if (!active || !active->has_value()) {
      return std::nullopt;
    }
    return (*active)->call_id;
  };
  ports.call_peer = [this](const std::string& call_id) -> std::optional<std::string> {
    auto peer = reads_.PeerIdentityForCall(call_id);
    if (!peer || !peer->has_value()) {
      return std::nullopt;
    }
    return **peer;
  };
  ports.local_identity = [this]() -> std::string {
    auto local = LocalIdentity();
    return local ? *local : std::string{};
  };
  ports.local_caps = [this]() -> std::optional<CallPeerCaps> {
    return mesh_.local_peer_caps ? std::optional<CallPeerCaps>{mesh_.local_peer_caps()} : std::nullopt;
  };
  ports.local_listen_addrs = [this]() {
    return mesh_.local_listen_multiaddrs ? mesh_.local_listen_multiaddrs() : std::vector<std::string>{};
  };
  ports.local_peer_id = [this]() { return mesh_.local_mesh_peer_id ? mesh_.local_mesh_peer_id() : std::string{}; };
  ports.may_learn_our_address = [this](const std::string& peer) {
    return AllowsDirect(mesh_.address_disclosure, peer);
  };
  ports.register_listen = [this](const std::string& key, const std::vector<std::string>& addrs) {
    if (mesh_.register_peer_listen_multiaddrs) {
      mesh_.register_peer_listen_multiaddrs(key, addrs);
    }
  };
  ports.on_peer_caps = [this](const std::string& call_id, const CallPeerCaps& caps) {
    if (mesh_.on_peer_caps) {
      mesh_.on_peer_caps(call_id, caps);
    }
  };
  reach_signals_.SetCallPorts(std::move(ports));
}

// --- UI intents (V037) ---------------------------------------------------------------------------

void CallDevice::NotifyCallStateChanged() {
  if (on_call_state_changed_) {
    on_call_state_changed_();
  }
}

std::string CallDevice::AcceptingCallId() const {
  return live_calls_.AcceptingCallId();
}

void CallDevice::KickAnswererAfterAccept(const std::string& call_id) {
  if (!live_calls_.AllowsDirectPath(call_id)) {
    log().info << "AcceptSucceeded skip KickAnswerer StartDuplex call_id=" << call_id
               << " status=" << CallMediaStatusName(live_calls_.Status(call_id));
    return;
  }
  log().info << "AcceptSucceeded KickAnswererDirectMedia StartDuplex arm call_id=" << call_id;
  KickAnswererDirectMediaIfArmed(call_id);
  // Once more, as the next event: the first kick can land before the 1:1 start is armed.
  outbox_.Emit(session_event::AnswererKickRetry{call_id});
}

void CallDevice::RestartMedia(const std::string& call_id_arg, const bool resume) {
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

void CallDevice::RunMediaRestart(const std::string& call_id, const bool resume) {
  const char* what = resume ? "PeerReconnected" : "RetryClicked";
  const Roe<void> restarted = resume ? ResumeDirectMedia(call_id) : RetryDirectMedia(call_id);
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

void CallDevice::RetryAnswererKick(const std::string& call_id) {
  const LiveCall* active = live_calls_.Active();
  if (!active || active->Id() != call_id || !live_calls_.AllowsDirectPath(call_id)) {
    return;
  }
  if (media_.IsActive() && media_.ActiveSessionId() == call_id) {
    return;
  }
  log().info << "AcceptSucceeded retry KickAnswererDirectMedia StartDuplex call_id=" << call_id;
  KickAnswererDirectMediaIfArmed(call_id);
}

void CallDevice::SetOutbox(OwnerOutbox<SessionEvent> outbox) {
  outbox_ = std::move(outbox);
  workflow_.SetOutbox(outbox_.For<WorkflowEvent>([](WorkflowEvent event) { return SessionEvent{std::move(event)}; }));
  workflow_.SetReports(outbox_.For<WorkflowReport>(
      [](WorkflowReport report) { return SessionEvent{session_event::FromWorkflow{std::move(report)}}; }));
  hop_service_.SetOutbox(outbox_.For<PlanningEvent>(
      [](PlanningEvent event) { return SessionEvent{session_event::ForHopService{std::move(event)}}; }));
}

void CallDevice::Handle(SessionEvent& event) {
  std::visit(
      [this](auto& e) {
        using E = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<E, session_event::AnswererKickRetry>) {
          RetryAnswererKick(e.call_id);
        } else if constexpr (std::is_same_v<E, session_event::MediaRestart>) {
          RunMediaRestart(e.call_id, e.resume);
        } else if constexpr (std::is_same_v<E, WorkflowEvent>) {
          workflow_.Handle(e);
        } else if constexpr (std::is_same_v<E, session_event::ForHopPath>) {
          if (CallMedia* call = call_media_.Find(e.call_id)) {
            call->HandleHopEvent(e.serial, e.event);
          }
        } else if constexpr (std::is_same_v<E, session_event::ForDirectPath>) {
          if (CallMedia* call = call_media_.Find(e.call_id)) {
            call->HandleDirectEvent(e.serial, e.event);
          }
        } else if constexpr (std::is_same_v<E, session_event::FromDirectPath>) {
          if (CallMedia* call = call_media_.Find(e.call_id); call && call->IsDirectPath(e.serial)) {
            OnDirectReport(e.report);
          }
        } else if constexpr (std::is_same_v<E, session_event::ForHopService>) {
          hop_service_.Handle(e.event);
        } else if constexpr (std::is_same_v<E, session_event::ForDoorway>) {
          if (doorway_ && e.generation == doorway_generation_) {
            doorway_->Handle(e.event);
          }
        } else if constexpr (std::is_same_v<E, session_event::FromHopPath>) {
          OnHopReport(e.report);
        } else if constexpr (std::is_same_v<E, session_event::FromWorkflow>) {
          OnWorkflowReport(e.report);
        } else {
          static_assert(!sizeof(E), "route every SessionEvent");
        }
      },
      event);
}

void CallDevice::OnDirectReport(const DirectReport& report) {
  std::visit(
      [this](const auto& r) {
        using R = std::decay_t<decltype(r)>;
        if constexpr (std::is_same_v<R, direct_report::Progress>) {
          live_calls_.ReportDirectProgress(r.call_id, r.phase);
        } else if constexpr (std::is_same_v<R, direct_report::Connected>) {
          live_calls_.NoteMediaConnected(r.call_id);
        } else if constexpr (std::is_same_v<R, direct_report::HandedToHop>) {
          live_calls_.NoteDirectHandedToHop(r.call_id);
        } else if constexpr (std::is_same_v<R, direct_report::ConnectFailed>) {
          live_calls_.NoteMediaFailed(r.call_id);
        } else if constexpr (std::is_same_v<R, direct_report::GaveUpReaching>) {
          SendConnectGiveUp(r.call_id, r.peer_identity);
        } else if constexpr (std::is_same_v<R, direct_report::PeerReconnected>) {
          RestartMedia(r.call_id, true);
        } else if constexpr (std::is_same_v<R, direct_report::MediaAttempted>) {
          live_calls_.NoteMediaAttempted(r.call_id);
        } else if constexpr (std::is_same_v<R, direct_report::MediaDeferred>) {
          live_calls_.NoteMediaDeferred(r.call_id);
        } else if constexpr (std::is_same_v<R, direct_report::MediaKeyReady>) {
          OnAnswererKeyReady(r.call_id, r.peer_identity);
        } else if constexpr (std::is_same_v<R, direct_report::Changed>) {
          NotifyRingChanged();
        } else if constexpr (std::is_same_v<R, direct_report::MediaError>) {
          SetLastMediaError(r.message);
        } else if constexpr (std::is_same_v<R, direct_report::MediaKeyResendWanted>) {
          ResendMediaKey(r.call_id, r.peer_identity);
        } else if constexpr (std::is_same_v<R, direct_report::InboxSyncWanted>) {
          RequestInboxSync();
        } else if constexpr (std::is_same_v<R, direct_report::InboundHello>) {
          ApplyInboundHello(r.call_id, r.identity, r.peer_id);
        } else {
          static_assert(!sizeof(R), "take every DirectReport");
        }
      },
      report);
}

void CallDevice::OnAnswererKeyReady(const std::string& call_id, const std::string& peer_identity) {
  live_calls_.NoteMediaKeyReady(call_id);
  if (peer_identity.empty()) {
    return;
  }
  // The deferred answerer goes ahead on the 1:1 path: arm it there, then start it (calls down).
  live_calls_.RequestDirectArming(call_id);
  ScheduleStartDirectMedia(call_id, peer_identity, /*offerer=*/false);
}

void CallDevice::SetInitiationBillingStore(IInitiationBillingBook* store) {
  billing_.SetStore(store);
}

void CallDevice::SetMediaRelayDeps(MediaRelayDeps deps) {
  call_media_.ForEachHopPath([](CallHopDriver& hop) { hop.UnwatchRelay(); }); // the old relay is still alive
  hop_service_.SetMediaRelayDeps(std::move(deps));
  call_media_.ForEachHopPath([](CallHopDriver& hop) { hop.OnRelayDepsChanged(); });
}

// Port setters swap a snapshot; the workflow / topology host ports (bound once in the ctor) read the
// current snapshot per call — never rebuilt under a running caller (thread-ownership t2a).

CallHopArmingPorts CallDevice::MakeHopArmingPorts() {
  CallHopArmingPorts ports;
  ports.hop_ops_allowed = [this]() { return live_calls_.AllowsHopPath(); };
  ports.direct_failed = [this]() { return live_calls_.Status() == CallMediaStatus::Failed; };
  ports.soft_migrate_may_arm = [this]() { return live_calls_.SoftMigrateMayArm(); };
  ports.media_cancel_gen = [this]() { return live_calls_.MediaCancelGen(); };
  ports.arming_debug_name = [this]() { return CallMediaStatusName(live_calls_.Status()); };
  return ports;
}

CallDirectArmingPorts CallDevice::DirectArmingPorts() {
  CallDirectArmingPorts ports;
  ports.direct_ops_allowed = [this]() { return live_calls_.AllowsDirectPath(); };
  ports.arming_debug_name = [this]() { return CallMediaStatusName(live_calls_.Status()); };
  return ports;
}

void CallDevice::BindSeat() {
  call_media_.BindMediaResources(&media_, &seat_);
  seat_.SetTeardownHooks(
      [this](const std::string& call_id) {
        if (CallMedia* call = call_media_.Find(call_id)) {
          call->OnMediaStopped();
        }
      },
      [this](const std::string& call_id, uint64_t epoch_at_post, bool force) {
        StopMediaForSeat(call_id, epoch_at_post, force);
      });
}

void CallDevice::StopMediaForSeat(const std::string& call_id, const uint64_t epoch_at_post, const bool force) {
  if (!force && seat_.Epoch() != epoch_at_post) {
    log().info << "MediaSeat stop skip stale call_id=" << call_id << " posted_epoch=" << epoch_at_post
               << " seat_epoch=" << seat_.Epoch();
    return;
  }
  if (CallMedia* call = call_media_.Find(call_id)) {
    call->StopMedia();
    return;
  }
  if (media_.IsActive() || media_.IsPacketMode()) { // a leftover with no live call here
    media_.Stop();
  }
}

void CallDevice::AttachDirectPath(CallDirectPathDeps deps) {
  if (!deps.IsUsable()) {
    DropDirectPath();
    return;
  }
  if (direct_deps_.transport != deps.transport) {
    DropDirectPath(); // the paths and the doorway rode the old transport
    BuildDoorway(*deps.transport);
  } else {
    call_media_.ForEachDirectPath([&deps](CallDirectDriver& path) { path.AttachReach(deps); });
  }
  direct_deps_ = std::move(deps);
  call_media_.BindDirectPathFactory([this](const std::string& call_id, const uint64_t serial,
                                           std::shared_ptr<CallEngine> engine,
                                           std::shared_ptr<std::atomic<bool>> carries) {
    return MakeDirectPath(call_id, serial, std::move(engine), std::move(carries));
  });
}

std::unique_ptr<CallDirectDriver> CallDevice::MakeDirectPath(const std::string& call_id, const uint64_t serial,
                                                             std::shared_ptr<CallEngine> engine,
                                                             std::shared_ptr<std::atomic<bool>> hop_carries_media) {
  if (!direct_deps_.IsUsable()) {
    return nullptr;
  }
  auto path = std::make_unique<CallDirectPath>(call_id, AsDirectPathQueries(), sessions_, media_keys_,
                                               std::move(engine), *direct_deps_.transport, direct_deps_.Reach());
  path->SetOutbox(outbox_.For<DirectPathEvent>([call_id, serial](DirectPathEvent event) {
    return SessionEvent{session_event::ForDirectPath{call_id, serial, std::move(event)}};
  }));
  path->SetReports(outbox_.For<DirectReport>([call_id, serial](DirectReport report) {
    return SessionEvent{session_event::FromDirectPath{call_id, serial, std::move(report)}};
  }));
  path->AttachReach(direct_deps_);
  path->SetDirectArmingPorts(DirectArmingPorts());
  path->SetSeatPorts(DirectSeatPorts());
  path->SetHopCarriesMediaFlag(std::move(hop_carries_media));
  return path;
}

std::unique_ptr<CallHopDriver> CallDevice::MakeHopPath(const std::string& call_id, const uint64_t serial,
                                                       std::shared_ptr<CallEngine> engine,
                                                       std::shared_ptr<std::atomic<bool>> hop_carries_media) {
  auto path = std::make_unique<CallHopPath>(call_id, sessions_, workflow_, contacts_, std::move(engine), hop_service_);
  path->SetCarriesMediaFlag(std::move(hop_carries_media));
  path->SetOutbox(outbox_.For<HopPathEvent>([call_id, serial](HopPathEvent event) {
    return SessionEvent{session_event::ForHopPath{call_id, serial, std::move(event)}};
  }));
  path->SetReports(outbox_.For<HopReport>(
      [](HopReport report) { return SessionEvent{session_event::FromHopPath{std::move(report)}}; }));
  path->SetMediaKeyStore(&media_keys_);
  path->SetHostPorts(MakeTopologyHostPorts());
  path->SetHopArmingPorts(MakeHopArmingPorts());
  path->SetSeatPorts(MakeTopologySeatPorts());
  path->OnRelayDepsChanged(); // watch the relay client the device has now
  return path;
}

CallMedia::View CallDevice::ActivePathsView() const {
  const LiveCall* active = live_calls_.Active();
  const CallMedia* call = active ? call_media_.Peek(active->Id()) : nullptr;
  return call ? call->PathsView() : CallMedia::View{};
}

void CallDevice::SetDirectPathFactoryForTest(CallDirectPathFactory make) {
  call_media_.DropDirectPaths();
  if (make) {
    call_media_.BindDirectPathFactory(std::move(make));
  } else if (direct_deps_.IsUsable()) {
    call_media_.BindDirectPathFactory([this](const std::string& call_id, const uint64_t serial,
                                             std::shared_ptr<CallEngine> engine,
                                             std::shared_ptr<std::atomic<bool>> carries) {
      return MakeDirectPath(call_id, serial, std::move(engine), std::move(carries));
    });
  } else {
    call_media_.BindDirectPathFactory({});
  }
}

void CallDevice::DetachDirectPathReach() {
  call_media_.ForEachDirectPath([](CallDirectDriver& path) { path.DetachReach(); });
}

void CallDevice::PrepareDirectPathForStop(const int wait_ms) {
  call_media_.BindDirectPathFactory({}); // no new path while the mesh stops
  call_media_.ForEachDirectPath([wait_ms](CallDirectDriver& path) { path.PrepareForTeardown(wait_ms); });
  if (doorway_) {
    doorway_->Shutdown(); // waiting hellos released, the handler dropped: late streams reach nothing
  }
}

void CallDevice::BuildDoorway(ICallMediaTransport& transport) {
  doorway_ = std::make_unique<CallMediaDoorway>(transport);
  const uint64_t generation = ++doorway_generation_;
  doorway_->SetOutbox(outbox_.For<DoorwayEvent>([generation](DoorwayEvent event) {
    return SessionEvent{session_event::ForDoorway{generation, std::move(event)}};
  }));
  doorway_->SetPorts(MakeDoorwayPorts());
}

CallMediaInboundPorts CallDevice::MakeDoorwayPorts() {
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
  ports.request_key = [this](const std::string& /*call_id*/) { RequestInboxSync(); };
  ports.on_accepted = [this](const CallMediaInboundHello& hello) {
    // The call's own media takes it (its 1:1 path built now when it has none yet).
    CallMedia* call = call_media_.Media(hello.call_id);
    return call ? call->AcceptInbound(hello) : CallMediaDirectCallbacks{};
  };
  return ports;
}

void CallDevice::DropDirectPath() {
  call_media_.BindDirectPathFactory({});
  doorway_.reset();
  call_media_.DropDirectPaths();
  direct_deps_ = {};
}

void CallDevice::OnLocalNetworkMoved() {
  const LiveCall* active = live_calls_.Active();
  if (CallMedia* call = active ? call_media_.Find(active->Id()) : nullptr) {
    call->OnLocalNetworkMoved();
  }
}

void CallDevice::OnPathPolicyChanged(const std::string& call_id) {
  if (CallMedia* call = call_media_.Find(call_id)) {
    call->OnPathPolicyChanged();
  }
}

bool CallDevice::IsDirectConnectInFlight() const {
  return ActivePathsView().direct.connect_inflight;
}

CallDirectSeatPorts CallDevice::DirectSeatPorts() {
  CallDirectSeatPorts ports;
  CallMediaSeat* seat = &seat_;
  ports.acquire = [seat](const std::string& call_id) { return seat->Acquire(call_id); };
  ports.allows_path_op = [seat](const CallMediaSeat::Token& token) { return seat->AllowsPathOp(token); };
  ports.current_token = [seat]() { return seat->CurrentToken(); };
  ports.bound_call_id = [seat]() { return seat->BoundCallId(); };
  ports.note_start = [seat](const std::string& call_id) { seat->NoteStart(call_id); };
  return ports;
}

CallHopPathSeatPorts CallDevice::MakeTopologySeatPorts() {
  CallHopPathSeatPorts ports;
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
  ports.note_start = [seat](const std::string& call_id) { seat->NoteStart(call_id); };
  ports.cancel_attach_for_call = [seat](const std::string& call_id) { seat->CancelAttachForCall(call_id); };
  return ports;
}

void CallDevice::ScheduleStartDirectMedia(const std::string& call_id, const std::string& peer_identity, bool offerer) {
  if (!live_calls_.AllowsDirectPath(call_id)) {
    log().info << "ScheduleStartDirectMedia skipped (Status disallows Bridge) call_id=" << call_id
               << " status=" << CallMediaStatusName(live_calls_.Status(call_id))
               << " armed=" << CallArmedPlannerName(live_calls_.ArmedPlanner(call_id));
    return;
  }
  // The device is signaling-only for duplex start — the call's `CallMedia` takes the seat and has the
  // direct driver connect.
  log().info << "ScheduleStartDirectMedia libp2p role=" << (offerer ? "offerer" : "answerer") << " call_id=" << call_id
             << " peer=" << peer_identity;
  CallMedia* call_media = call_media_.Media(call_id);
  if (auto begun =
          call_media ? call_media->BeginDirect(peer_identity, offerer) : Roe<void>(Error("no live call for media"));
      !begun) {
    log().error << "ScheduleStartDirectMedia: " << begun.error().message << " call_id=" << call_id;
    last_media_error_ = "Call media unavailable";
    NotifyRingChanged();
  }
}

void CallDevice::KickAnswererDirectMediaIfArmed(const std::string& call_id) {
  if (call_id.empty()) {
    log().info << "KickAnswererDirectMediaIfArmed skip (empty call_id)";
    return;
  }
  std::string peer;
  if (!workflow_.PeekPendingAnswererKick(call_id, &peer)) {
    auto resolved = reads_.PeerIdentityForCall(call_id);
    if (resolved && resolved->has_value()) {
      peer = **resolved;
    }
  }
  CallAnswererKickDecisionInput in;
  in.allows_direct_path = live_calls_.AllowsDirectPath(call_id);
  // IsActive alone — do not require tx/connected (capture lags StartDuplex; dogfood e157 thrash).
  in.media_already_active_same_call = media_.IsActive() && media_.ActiveSessionId() == call_id;
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

CallHopHealth CallDevice::HopHealth() const {
  return ActivePathsView().hop_health;
}

CallLinkCounters CallDevice::MediaLinkCounters() const {
  return ActivePathsView().direct.link;
}

std::string CallDevice::MediaPathKind() const {
  return ActivePathsView().direct.media_path_kind;
}

bool CallDevice::IsSfuAttached() const {
  return ActivePathsView().hop_attached;
}

void CallDevice::ClearLastMediaErrorIf(const std::string& seen) {
  if (last_media_error_ && *last_media_error_ == seen) {
    last_media_error_.reset();
  }
}

std::optional<std::string> CallDevice::TakeLastMediaError() {
  auto out = std::move(last_media_error_);
  last_media_error_.reset();
  return out;
}

void CallDevice::SetOnRingChanged(RingChangedFn callback) {
  on_ring_changed_ = std::move(callback);
}

void CallDevice::SetOnRingChangedMesh(RingChangedFn callback) {
  on_ring_changed_mesh_ = std::move(callback);
}

void CallDevice::NotePeerMediaRelayCap(const std::string& peer_id, bool media_relay) {
  // Topology owns SoftMigrate nudge (N≥3 / attach-wait only — V038).
  if (media_relay_caps_.Note(peer_id, media_relay)) {
    if (auto active = reads_.ActiveLocalCall(); active && active->has_value()) {
      if (CallMedia* call = call_media_.Find((*active)->call_id)) {
        call->OnPeerMediaRelayCapLearned(peer_id);
      }
    }
  }
}

void CallDevice::NoteMeshPeerIdForRelay(const std::string& relay_identity, const std::string& peer_id) {
  if (mesh_.note_peer_device && relay_identity.rfind("account:", 0) == 0) {
    mesh_.note_peer_device(relay_identity, peer_id);
  }
  if (!peer_accounts_.Learn(relay_identity, peer_id)) {
    return;
  }
  call_media_.ForEachDirectPath([&](CallDirectDriver& path) { path.NotePeerIdRelayMapping(peer_id, relay_identity); });
}

bool CallDevice::PeerHasMediaRelayCap(const std::string& peer_id) const {
  return media_relay_caps_.Has(peer_id);
}

std::vector<std::string> CallDevice::ListMediaRelayCapablePeerIds() const {
  return media_relay_caps_.ListCapable();
}

void CallDevice::NotifyRingChanged() {
  if (on_ring_changed_mesh_) {
    on_ring_changed_mesh_();
  }
  if (on_ring_changed_) {
    on_ring_changed_();
  }
}

Roe<CallRosterDetail> CallDevice::BuildRosterDetail(const std::string& call_id) const {
  return call_queries::RosterDetail(sessions_, call_id);
}

void CallDevice::StopMediaIfCall(const std::string& call_id) {
  // The device is signaling-only for duplex stop: the call registry releases the seat (Detach-then-Stop).
  call_media_.StopMedia(call_id);
}

Roe<void> CallDevice::MaybeRotateMediaKey(const std::string& call_id, const std::string& leaver_identity) {
  return workflow_.MaybeRotateMediaKey(call_id, leaver_identity);
}

Roe<void> CallDevice::EndCallLocal(CallSession& session, const std::optional<int64_t>& duration_ms,
                                   const LiveCallEndReason reason) {
  auto result = workflow_.EndCallLocal(session, duration_ms, reason);
  MaybeCatchUpAfterCall();
  return result;
}

Roe<void> CallDevice::LeaveCall(const std::string& call_id, const LiveCallEndReason reason) {
  auto result = workflow_.LeaveCall(call_id, reason);
  MaybeCatchUpAfterCall();
  return result;
}

void CallDevice::MaybeCatchUpAfterCall() {
  if (!delivery_.catch_up_after_call) {
    return;
  }
  auto active = reads_.ActiveLocalCall();
  if (active && *active) {
    return;
  }
  delivery_.catch_up_after_call();
}

bool CallDevice::IsAwaitingSfuRecovery() const {
  return ActivePathsView().hop_in_flight;
}

CallRecovery CallDevice::Recovery() const {
  return ActivePathsView().recovery;
}

bool CallDevice::IsDirectConnectFailed() const {
  return ActivePathsView().direct.connect_failed;
}

bool CallDevice::DirectConnectMissingMic() const {
  return ActivePathsView().direct.missing_mic;
}

bool CallDevice::DirectConnectSeedUnreachable() const {
  return ActivePathsView().direct.seed_unreachable;
}

Roe<void> CallDevice::RetryDirectMedia(const std::string& call_id) {
  CallMedia* call_media = call_media_.Media(call_id);
  if (call_media && live_calls_.MediaAttempted(call_id)) {
    return call_media->Retry();
  }
  return Error("Call media retry unavailable");
}

Roe<void> CallDevice::ResumeDirectMedia(const std::string& call_id) {
  CallMedia* call_media = call_media_.Media(call_id);
  return call_media ? call_media->ResumeFromInbound() : Roe<void>(Error("Call media resume unavailable"));
}

void CallDevice::DropFollowUps() {
  // Hub teardown, before the store and delivery go: queued follow-ups (a roster fan-out) must not run (PR #216).
  workflow_.DropFollowUps();
}

void CallDevice::SendConnectGiveUp(const std::string& call_id, const std::string& peer_identity) {
  CallConnectGiveUpDetail detail;
  detail.call_id = call_id;
  if (auto local = control_.LocalRelayIdentity()) {
    detail.identity = *local;
  }
  auto encoded = CallControlCodec::EncodeConnectGiveUp(detail);
  if (!encoded) {
    return;
  }
  if (auto sent = control_.SendDirect(peer_identity, CallControlType::CallConnectGiveUp, *encoded, ""); !sent) {
    log().warning << "CallConnectGiveUp send failed call_id=" << call_id << " err=" << sent.error().message;
    return;
  }
  log().info << "CallConnectGiveUp sent call_id=" << call_id << " peer=" << peer_identity;
}

Roe<std::string> CallDevice::LocalIdentity() const {
  return control_.LocalRelayIdentity();
}

Roe<void> CallDevice::TopologyLeaveCall(const std::string& call_id) {
  // Topology leaves when the group media path cannot be kept (attach-wait timeout, failed migrate).
  return LeaveCall(call_id, LiveCallEndReason::MediaUnavailable);
}

Roe<void> CallDevice::TopologyFanOutToJoined(const std::string& call_id, CallControlType type,
                                             const std::string& detail_json, const std::string& display,
                                             const std::string& skip_identity) {
  return control_.FanOutToJoined(call_id, type, detail_json, display, skip_identity);
}

Roe<void> CallDevice::SendDirect(const std::string& peer_identity, CallControlType type, const std::string& detail_json,
                                 const std::string& display) {
  return control_.SendDirect(peer_identity, type, detail_json, display);
}

void CallDevice::SetLastMediaError(std::string message) {
  last_media_error_ = std::move(message);
}

void CallDevice::ClearMediaActivity() {
  if (const LiveCall* shown = live_calls_.Shown()) {
    live_calls_.NoteActivity(shown->Id(), {});
  }
}

void CallDevice::TopologyNoteMediaAttempted(const std::string& call_id) {
  live_calls_.NoteMediaAttempted(call_id);
}

void CallDevice::RequestInboxSync() {
  if (delivery_.sync_inbox_from_wake) {
    delivery_.sync_inbox_from_wake(true);
  }
}

Roe<std::optional<std::string>> CallDevice::PeerIdentityForCall(const std::string& call_id) const {
  return reads_.PeerIdentityForCall(call_id);
}

Roe<std::optional<std::string>> CallDevice::MeshPeerIdForAccount(const std::string& account) const {
  return peer_accounts_.PeerIdForAccount(account);
}

Roe<std::optional<std::string>> CallDevice::RelayIdentityForMeshPeerId(const std::string& call_id,
                                                                       const std::string& peer_id) const {
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

void CallDevice::ResendMediaKey(const std::string& call_id, const std::string& peer_identity) {
  if (call_id.empty() || peer_identity.empty()) {
    return;
  }
  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value() || (*session)->state == CallSessionState::Ended) {
    return;
  }
  (void)key_exchange_.SendCurrent(call_id, peer_identity);
}

void CallDevice::ApplyInboundHello(const std::string& call_id, const std::string& identity,
                                   const std::string& peer_id) {
  auto local = control_.LocalRelayIdentity();
  if (!local) {
    return;
  }
  if (auto applied = workflow_.ApplyImplicitAccept(call_id, identity, peer_id, *local); !applied) {
    log().warning << "implicit accept failed call_id=" << call_id << " err=" << applied.error().message;
  }
}

} // namespace pbr
