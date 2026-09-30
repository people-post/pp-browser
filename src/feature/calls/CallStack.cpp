#include "feature/calls/CallStack.h"
#include "feature/calls/CallsThread.h"

#include "foundation/data/MeshRole.h"
#include "domain/mesh/host/MeshPorts.h"
#include "domain/messaging/CallTypes.h"
#include "domain/messaging/PeerCapsLogic.h"
#include "domain/people/ContactsStore.h"
#include "domain/people/IdentityStore.h"
#include "foundation/runtime/AppRuntime.h"
#include "domain/messaging/SqlitePskSessionStore.h"
#include "domain/mesh/reachability/Reachability.h"
#include "domain/mesh/reachability/punch/AmpPunchCoordinator.h"
#include "domain/mesh/reachability/AmpObservedAddrs.h"
#include "feature/calls/CallMediaBridge.h"
#include "domain/messaging/CallLifecycleTypes.h"

#include <functional>
#include <optional>
#include <type_traits>
#include <variant>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

CallStack::CallStack() {
  redirectLogger("CallStack");
  media_plane_ = std::make_unique<CallMediaPlane>();
  publish_hook_ = CallsThread::AddAfterTaskHook([this]() { PublishUiState(); });
}

CallStack::~CallStack() {
  loop_.DropPending();  // inputs / wakes still queued would run against a dying stack
  chrome_self_.Invalidate();
  Shutdown();
  // On the owner: hooks run only there, so none is mid-flight on this stack once this returns.
  CallsThread::RunAndWait([this]() { CallsThread::RemoveAfterTaskHook(publish_hook_); });
}

// --- hub-facing lifecycle edges: run on the calls owner, the caller waits (t2b-3) ---------------

void CallStack::BuildSessions(const CallStackDeps& deps) {
  CallsThread::RunAndWait([&]() { BuildSessionsOnOwner(deps); });
}

void CallStack::OnMeshServicesStarted() {
  CallsThread::RunAndWait([this]() { OnMeshServicesStartedOnOwner(); });
}

void CallStack::PrepareForMeshStop(const std::function<void()>& abort_inflight_circuit) {
  CallsThread::RunAndWait([&]() { PrepareForMeshStopOnOwner(abort_inflight_circuit); });
}

void CallStack::FinishMeshStop() {
  CallsThread::RunAndWait([this]() { FinishMeshStopOnOwner(); });
}

// --- edge adapters: other threads enqueue; Dispatch routes on the owner ------------------------------

void CallStack::OnLocalNetwork(const MobilityAttachment& attachment, const bool changed, const bool moved) {
  loop_.Enqueue(calls_event::LocalNetworkChanged{attachment, changed, moved});
}

void CallStack::OnObservedAddressChanged() {
  loop_.Enqueue(calls_event::ObservedAddressChanged{});
}

void CallStack::ReloadMobilityOverride() {
  loop_.Enqueue(calls_event::MobilityOverrideChanged{});
}

void CallStack::Dispatch(CallStackEvent& event) {
  const auto now = CallPathMobility::Clock::now();
  std::visit(
      [this, now](auto& e) {
        using E = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<E, calls_event::LocalNetworkChanged>) {
          AfterMobilityEvent(mobility_.OnAttachment(e.attachment, e.changed, now));
          if (e.moved && call_sessions_) {
            call_sessions_->OnLocalNetworkMoved();
          }
        } else if constexpr (std::is_same_v<E, calls_event::ObservedAddressChanged>) {
          AfterMobilityEvent(mobility_.OnObservedAddressChanged(now));
        } else if constexpr (std::is_same_v<E, calls_event::MobilityOverrideChanged>) {
          ApplyMobilityOverrideOnOwner();
        } else if constexpr (std::is_same_v<E, calls_event::MobilityWake>) {
          mobility_wake_.Fired();
          AfterMobilityEvent(mobility_.OnWake(now));
        } else if constexpr (std::is_same_v<E, calls_event::CallControlReceived>) {
          if (!call_sessions_) {
            return;
          }
          if (auto applied = call_sessions_->ApplyInboundControl(e.message, e.sender_identity, e.relay_created_at_ms,
                                                                 e.relay_server_time_ms);
              !applied) {
            log().warning << "inbound call control failed message_id=" << e.message.id
                          << " err=" << applied.error().message;
          }
        } else if constexpr (std::is_same_v<E, calls_event::RelayChosen>) {
          if (call_sessions_) {
            call_sessions_->ReachSignals().AnnounceCircuitR1(e.circuit_r1);
          }
        } else if constexpr (std::is_same_v<E, calls_event::MeshPeerIdLearned>) {
          if (call_sessions_) {
            call_sessions_->NoteMeshPeerIdForRelay(e.account_identity, e.peer_id);
          }
        } else if constexpr (std::is_same_v<E, calls_event::SessionsCommand>) {
          e.run(call_sessions_.get());
        } else if constexpr (std::is_same_v<E, calls_event::ForSessions>) {
          if (call_sessions_ && e.generation == sessions_generation_) {
            call_sessions_->Handle(e.event);
          }
        } else if constexpr (std::is_same_v<E, calls_event::SignalingPunchRequested>) {
          if (!call_sessions_) {
            e.done(Error("Calls unavailable"));
            return;
          }
          call_sessions_->ReachSignals().RequestSignalingPunch(e.target_peer_id, e.my_addrs, std::move(e.done));
        } else {
          static_assert(!sizeof(E), "route every CallStackEvent");
        }
      },
      event);
}

void CallStack::ApplyMobilityOverrideOnOwner() {
  const auto cfg = mesh_config();
  const auto pinned = ResolveMobilityOverride(cfg ? cfg->mobility : std::string("auto"));
  AfterMobilityEvent(mobility_.SetOverride(pinned, CallPathMobility::Clock::now()));
}

void CallStack::AfterMobilityEvent(const bool local_class_changed) {
  mobility_wake_.ArmAt(mobility_.NextWakeAt(CallPathMobility::Clock::now()));
  if (!local_class_changed || !call_sessions_) {
    return;
  }
  // Our class flipped mid-call: tell the peer (caps_update) and re-plan the live call.
  if (auto active = call_sessions_->ActiveLocalCall(); active && active->has_value()) {
    call_sessions_->ReachSignals().AnnounceCapsUpdate();
    NotifyPathPolicyChangedOnOwner((*active)->call_id);
  }
}

void CallStack::NotifyPathPolicyChangedOnOwner(const std::string& call_id) {
  if (call_sessions_) {
    call_sessions_->OnPathPolicyChanged(call_id);
  }
}

CallPathPolicy CallStack::PathPolicyFor(const std::string& call_id) const {
  return mobility_.PolicyFor(call_id);
}

void CallStack::DetachMeshMedia() {
  CallsThread::RunAndWait([this]() { DetachMeshMediaOnOwner(); });
}

void CallStack::RebindMeshMedia() {
  CallsThread::RunAndWait([this]() { RebindMeshMediaOnOwner(); });
}

void CallStack::ResetSessions() {
  CallsThread::RunAndWait([this]() { ResetSessionsOnOwner(); });
}

void CallStack::AbortCallMediaForShutdown() {
  CallsThread::RunAndWait([this]() { AbortCallMediaForShutdownOnOwner(); });
}

void CallStack::RegisterCallPeerListenMultiaddrs(const std::string& identity,
                                                 const std::vector<std::string>& multiaddrs) {
  CallsThread::RunAndWait([&]() { RegisterCallPeerListenMultiaddrsOnOwner(identity, multiaddrs); });
}

void CallStack::PostToSessions(std::function<void(CallSessionManager* calls)> run) {
  loop_.Enqueue(calls_event::SessionsCommand{std::move(run)});
}

void CallStack::RunOnOwner(const std::function<void(CallSessionManager&)>& op) {
  CallsThread::RunAndWait([&]() {
    if (call_sessions_) {
      op(*call_sessions_);
    }
  });
}

void CallStack::PublishUiState() {
  CallUiState state;
  if (call_sessions_) {
    // What the calls show (V037), projected from the calls this device has.
    const LiveCalls& live = call_sessions_->Live();
    state.phase = live.Phase();
    state.media_status = live.Status();
    if (const LiveCall* shown = live.Shown()) {
      state.active_call_id = shown->Id();
    }
    state.accepting_call_id = call_sessions_->AcceptingCallId();
    if (const LiveCall* ring = live.TheRing()) {
      state.last_ring_call_id = ring->Id();
    }
    state.last_error = call_sessions_->LastError();
    if (const LiveCall* ended = call_sessions_->Live().LastEndedByPeer()) {
      state.remote_ended_call_id = ended->Id();
      state.remote_ended_declined = ended->EndReason() == LiveCallEndReason::DeclinedByPeer;
    }
    state.awaiting_sfu_recovery = call_sessions_->IsAwaitingSfuRecovery();
    state.soft_migrate_in_flight = call_sessions_->IsSoftMigrateInFlight();
    state.sfu_attach_wait_active = call_sessions_->IsSfuAttachWaitActive();
    state.p2p_connect_failed = call_sessions_->IsP2pConnectFailed();
    state.p2p_connect_missing_mic = call_sessions_->P2pConnectMissingMic();
    state.media_activity = call_sessions_->PeekMediaActivity();
    state.last_media_error = call_sessions_->PeekLastMediaError();
    state.hop_health = call_sessions_->HopHealth();
    state.media_path_kind = call_sessions_->MediaPathKind();
  }
  if (call_sessions_) {
    const CallMediaSeat& seat = call_sessions_->Seat();
    state.seat_bound_call_id = seat.BoundCallId();
    state.seat_state = seat.State();
    state.seat_live = seat.IsLive(state.seat_bound_call_id);
  }
  state.want_ephemeral_listen = want_ephemeral_listen_;
  state.connect_in_flight = call_sessions_ && call_sessions_->IsDirectConnectInFlight();
  state.sessions_identity = call_sessions_.get();
  state.available = call_sessions_ != nullptr;
  ui_state_.Set(std::move(state));
}

std::shared_ptr<const MeshConfig> CallStack::mesh_config() const {
  auto cfg = deps_.mesh_config ? deps_.mesh_config() : nullptr;
  return cfg ? cfg : std::make_shared<const MeshConfig>();
}

void CallStack::SyncMediaPlaneDeps() {
  if (!media_plane_) {
    return;
  }
  CallMediaPlaneDeps plane_deps;
  plane_deps.mesh = deps_.mesh;
  plane_deps.mesh_config = deps_.mesh_config;
  plane_deps.list_directory_nodes = deps_.list_directory_nodes;
  plane_deps.list_dht_nodes = deps_.list_dht_nodes;
  plane_deps.seed_dial_ok = deps_.seed_dial_ok;
  plane_deps.local_listen_multiaddrs = [this]() { return LocalCallListenMultiaddrs(); };
  plane_deps.peer_has_media_relay = [this](const std::string& peer_id) {
    if (call_sessions_ && call_sessions_->PeerHasMediaRelayCap(peer_id)) {
      return true;
    }
    // Mesh directory / DHT ads — SoftMigrate must not wait for Invite/Accept caps (V030).
    if (deps_.list_directory_nodes &&
        PeerHasMediaRelayInDirectory(peer_id, deps_.list_directory_nodes())) {
      return true;
    }
    if (deps_.list_dht_nodes && PeerHasMediaRelayInDirectory(peer_id, deps_.list_dht_nodes())) {
      return true;
    }
    return false;
  };
  plane_deps.list_media_relay_peers = [this]() {
    std::vector<std::string> from_caps;
    if (call_sessions_) {
      from_caps = call_sessions_->ListMediaRelayCapablePeerIds();
    }
    const std::vector<MeshDirectoryNode> directory =
        deps_.list_directory_nodes ? deps_.list_directory_nodes() : std::vector<MeshDirectoryNode>{};
    const std::vector<MeshDirectoryNode> dht =
        deps_.list_dht_nodes ? deps_.list_dht_nodes() : std::vector<MeshDirectoryNode>{};
    return MergeMediaRelayCapablePeerIds(from_caps, directory, dht);
  };
  plane_deps.note_mesh_peer_id_for_relay = [this](const std::string& account, const std::string& peer_id) {
    loop_.Enqueue(calls_event::MeshPeerIdLearned{account, peer_id});  // connectivity owner → calls owner
  };
  media_plane_->SetDeps(std::move(plane_deps));
  media_plane_->SetMeshMedia(mesh_media());
  BindMeshMediaHooks();
}

void CallStack::BindMeshMediaHooks() {
  MeshMediaPlane* shared = mesh_media();
  if (!shared) {
    return;
  }
  // The plane runs these on the connectivity owner / Amp IO: hop to the calls owner.
  // H011: the rendezvous R1 our circuit reach chose is announced to the call peer.
  shared->SetOnRelayChosen(
      [this](const std::string& circuit_r1) { loop_.Enqueue(calls_event::RelayChosen{circuit_r1}); });
  // H012: when Amp introducers are exhausted, exchange punch candidates over call-control.
  shared->SetSignalingPunch([this](const std::string& target_peer_id, const std::vector<std::string>& my_addrs,
                                   std::function<void(Roe<void>)> on_done) {
    loop_.Enqueue(calls_event::SignalingPunchRequested{target_peer_id, my_addrs, std::move(on_done)});
  });
}

void CallStack::DetachMeshMediaOnOwner() {
  if (call_sessions_) {
    call_sessions_->SetMediaRelayDeps({});
    call_sessions_->DetachDirectPathReach();
  }
}

void CallStack::RebindMeshMediaOnOwner() {
  SyncMediaPlaneDeps();
  BindMediaProducts();
  ApplyMobilityOverrideOnOwner();
}

void CallStack::BindMediaProducts() {
  if (!media_plane_ || !call_sessions_ || !call_session_store_ || !call_media_keys_ ||
      !call_media_engine_) {
    return;
  }
  CallDirectPathDeps direct = media_plane_->DirectPathDeps();
  direct.path_policy = [this](const std::string& call_id) { return PathPolicyFor(call_id); };
  call_sessions_->AttachDirectPath(std::move(direct));
  call_sessions_->SetMediaRelayDeps(media_plane_->BuildMediaRelayDeps());
  // Call state ports (mesh stop cleared them): one bind point with BuildSessions.
  BindCallState();
  PublishUiState();
}

Roe<void> CallStack::InitializeStores(const std::string& profile_db_path, const std::string& profile_id) {
  Roe<void> result;
  CallsThread::RunAndWait([&]() { result = InitializeStoresOnOwner(profile_db_path, profile_id); });
  return result;
}

Roe<void> CallStack::InitializeStoresOnOwner(const std::string& profile_db_path, const std::string& profile_id) {
  call_session_store_ = std::make_unique<CallSessionStore>(profile_db_path);
  call_media_keys_ = std::make_unique<CallMediaKeyStore>(profile_db_path, profile_id);
  call_media_engine_ = std::make_unique<CallMediaEngine>();
  if (!media_plane_) {
    media_plane_ = std::make_unique<CallMediaPlane>();
  }
  return {};
}

void CallStack::BuildSessionsOnOwner(const CallStackDeps& deps) {
  PBR_ASSERT_ON_OWNER(OwnerThreadId::MediaSessions);
  deps_ = deps;
  call_sessions_ = std::make_unique<CallSessionManager>(*deps_.store, *deps_.contacts, *deps_.identity,
                                                        *call_session_store_, *call_media_keys_, deps_.delivery,
                                                        *deps_.psk, *call_media_engine_);
  BindSessionOutbox();
  BindCallControlInbound();
  call_sessions_->AbandonOrphanedCallsAfterRestart();
  call_sessions_->SetOnRingChangedMesh([this]() { SyncHubEphemeralListen(); });
  BindSessionProviders();
  BindSessionMeshReach();
  BindCallState();
  RebindMeshMedia();
}

void CallStack::BindSessionOutbox() {
  // The manager's events come back through the queue; one from a manager since rebuilt is dropped.
  const uint64_t generation = ++sessions_generation_;
  OwnerOutbox<SessionEvent>::Sink sink;
  sink.emit = [this, generation](SessionEvent event) {
    loop_.Enqueue(calls_event::ForSessions{generation, std::move(event)});
  };
  sink.after = [this, generation](std::chrono::milliseconds delay, SessionEvent event) {
    return loop_.After(delay, calls_event::ForSessions{generation, std::move(event)});
  };
  sink.cancel = [this](OwnerExecutor::TimerId id) { loop_.Cancel(id); };
  call_sessions_->SetOutbox(OwnerOutbox<SessionEvent>(std::move(sink)));
}

void CallStack::BindCallControlInbound() {
  if (!deps_.bind_call_control) {
    return;
  }
  CallControlInboundPorts inbound;
  // Receive threads hand call control to the calls owner (thread-ownership t2a, T003): fire and
  // forget with a copy — ApplyInboundControl only reads the message; failures are logged there.
  inbound.apply_inbound_control = [this](ThreadMessage& message, const std::string& sender_identity,
                                         std::optional<int64_t> relay_created_at_ms,
                                         std::optional<int64_t> relay_server_time_ms) -> Roe<void> {
    loop_.Enqueue(calls_event::CallControlReceived{message, sender_identity, relay_created_at_ms, relay_server_time_ms});
    return {};
  };
  inbound.has_active_local_call = [this]() { return HasActiveLocalCall(); };
  inbound.active_call_origin_thread_id = [this]() -> std::optional<std::string> {
    if (!call_sessions_) {
      return std::nullopt;
    }
    auto active = call_sessions_->ActiveLocalCall();
    if (!active || !*active || !(*active)->origin_thread_id) {
      return std::nullopt;
    }
    return *(*active)->origin_thread_id;
  };
  deps_.bind_call_control(std::move(inbound));
}

/** What the sessions read about this node: listen addrs, mesh PeerId, caps; and where peers' addrs go. */
void CallStack::BindSessionProviders() {
  call_sessions_->SetPrefetchPeerReachability([prefetch = deps_.prefetch_peer_reachability](const std::string& identity) {
    // Warm only (async association / DHT lookup) — on the hub's thread, which owns what it reads.
    // Captures the hub's port, not this stack: a teardown may race the post.
    if (prefetch) {
      AppRuntime::PostUI([prefetch, identity]() { prefetch(identity); });
    }
  });
  call_sessions_->SetLocalListenMultiaddrsProvider([this]() { return LocalCallListenMultiaddrs(); });
  // Providers read the connectivity owner's published view of this node's mesh (never the
  // MeshHost the hub may be tearing down under a running call flow).
  call_sessions_->SetLocalMeshPeerIdProvider([this]() -> std::string { return LocalMeshView()->local_peer_id; });
  call_sessions_->SetCallPeerCapsSink([this](const std::string& call_id, const CallPeerCaps& caps) {
    if (mobility_.NoteRemote(call_id, caps.mobility)) {
      NotifyPathPolicyChangedOnOwner(call_id);
    }
  });
  call_sessions_->SetLocalPeerCapsProvider([this]() { return LocalPeerCaps(); });
  call_sessions_->SetRegisterPeerListenMultiaddrs(
      [this](const std::string& identity, const std::vector<std::string>& multiaddrs) {
        RegisterCallPeerListenMultiaddrs(identity, multiaddrs);
      });
}

/** This node's caps for invite / accept / caps update (V030, k6). */
CallPeerCaps CallStack::LocalPeerCaps() const {
  CallPeerCaps caps;
  caps.v = kCallPeerCapsVersion;
  caps.present = true;
  caps.mobility = LocalMobility();
  // Durable Node host only — never advertise media_relay for ephemeral listen-only (V030).
  const auto view = LocalMeshView();
  const auto cfg = mesh_config();
  caps.media_relay = ResolveMeshRole(*cfg) == MeshRole::Node && cfg->capabilities.media_relay && view->amp_up &&
                     view->media_relay_started;
  return caps;
}

/** Connectivity for the sessions: circuit readiness / park, and the reach signals' mesh side. */
void CallStack::BindSessionMeshReach() {
  // Connectivity: park on org hops so this peer is ServeDial-reachable (shared mesh media).
  call_sessions_->SetEnsureCircuitReady([this]() {
    if (MeshMediaPlane* shared = mesh_media()) {
      shared->Rendezvous().ReserveOnBootstrapSeeds();
    }
  });
  // Park completes on the Amp IO thread (or at a coordinator deadline); the workflow reports it as an event.
  call_sessions_->SetParkCircuit([this](int timeout_ms, std::function<void(bool)> done) {
    if (MeshMediaPlane* shared = mesh_media()) {
      shared->Rendezvous().EnsureBootstrapSeedParkedAsync(std::move(done), timeout_ms);
    } else {
      done(false);
    }
  });
  CallReachSignals::MeshPorts reach;
  reach.prefer_late_reserve = [this](const std::string& relay_peer_id) {
    if (MeshMediaPlane* shared = mesh_media()) {
      shared->Rendezvous().PreferLateReserve(relay_peer_id);
    }
  };
  reach.local_punch_addrs = [this]() -> std::vector<std::string> { return LocalMeshView()->punch_candidate_addrs; };
  reach.punch_burst = [this](const std::vector<std::string>& peer_addrs, int window_ms,
                             std::function<void(Roe<void>)> on_done) {
    MeshMediaPlane* shared = mesh_media();
    if (!shared) {
      if (on_done) {
        on_done(Error("amp punch unavailable"));
      }
      return;
    }
    shared->SignalingPunchBurstAsync(peer_addrs, window_ms, std::move(on_done));
  };
  call_sessions_->ReachSignals().SetMeshPorts(std::move(reach));
}

void CallStack::OnMeshServicesStartedOnOwner() {
  SyncMediaPlaneDeps();
  if (media_plane_) {
    media_plane_->OnMeshStarted();
  }
  BindMediaProducts();
  ApplyMobilityOverrideOnOwner();
}

void CallStack::BindTestMediaPath(ICallMediaTransport* transport, IDialRegistry* dial) {
  BindTestMediaPath(transport, dial, nullptr);
}

void CallStack::BindTestMediaPath(ICallMediaTransport* transport, IDialRegistry* dial,
                                  ICircuitHopReach* circuit_reach) {
  BindTestMediaPath(transport, dial, circuit_reach, nullptr);
}

void CallStack::BindTestMediaPath(ICallMediaTransport* transport, IDialRegistry* dial,
                                  ICircuitHopReach* circuit_reach, IMediaRelayClient* relay) {
  CallsThread::RunAndWait([&]() { BindTestMediaPathOnOwner(transport, dial, circuit_reach, relay); });
}

void CallStack::BindTestMediaPathOnOwner(ICallMediaTransport* transport, IDialRegistry* dial,
                                         ICircuitHopReach* circuit_reach, IMediaRelayClient* relay) {
  SyncMediaPlaneDeps();
  DetachMeshMedia();
  if (media_plane_) {
    media_plane_->BindTestMediaPath(transport);
  }
  if (MeshMediaPlane* shared = mesh_media()) {
    shared->BindTestPath(dial, circuit_reach, relay);
  }
  BindMediaProducts();
}

bool CallStack::HasActiveLocalCall() {
  // Any thread (hub, receive paths, shutdown marks): the owner's snapshot, then the durable store.
  if (UiState()->want_ephemeral_listen) {
    return true;
  }
  if (!call_sessions_) {
    return false;
  }
  if (auto active = call_sessions_->ActiveLocalCall(); active && active->has_value()) {
    return true;
  }
  return false;
}

bool CallStack::WantEphemeralListen() const {
  if (CallsThread::IsCurrent()) {
    return want_ephemeral_listen_;
  }
  return UiState()->want_ephemeral_listen;
}

void CallStack::PrepareForMeshStopOnOwner(const std::function<void()>& abort_inflight_circuit) {
  PBR_ASSERT_ON_OWNER(OwnerThreadId::MediaSessions);
  DetachMeshMedia();
  // The 1:1 path lets go of its streams between two circuit aborts, then the transport stops.
  if (abort_inflight_circuit) {
    abort_inflight_circuit();
  }
  if (call_sessions_) {
    call_sessions_->PrepareDirectPathForStop();
  }
  if (abort_inflight_circuit) {
    abort_inflight_circuit();
  }
  if (media_plane_) {
    media_plane_->StopTransport();
  }
}

void CallStack::FinishMeshStopOnOwner() {
  if (call_sessions_) {
    call_sessions_->DropDirectPath();  // before its transport goes
  }
  if (media_plane_) {
    media_plane_->FinishMeshStop();
  }
}

void CallStack::AbortCallMediaForShutdownOnOwner() {
  // Unblock Connect workers stuck in circuit RequestBridge (~8–10s) BEFORE waiting/joining.
  if (MeshHost* m = mesh()) {
    m->AbortInflightCircuitRequests();
  }
  // Group SFU: close media_relay before LeaveCall joins capture (BlockingWrite hang on quit).
  if (media_plane_) {
    media_plane_->DetachRelayClient();
  }
  // Tell the peer the call ended (fire-and-forget relay Critical send) so they StopMedia /
  // leave Connecting instead of sitting on a half-open stream after we detach.
  if (call_sessions_) {
    if (auto active = call_sessions_->ActiveLocalCall(); active && active->has_value()) {
      log().info << "AbortCallMediaForShutdown LeaveCall call_id=" << (*active)->call_id;
      (void)call_sessions_->LeaveCall((*active)->call_id, LiveCallEndReason::Shutdown);
    }
  }
  if (call_sessions_) {
    call_sessions_->PrepareDirectPathForStop();
  }
  if (media_plane_) {
    media_plane_->DetachTransport();
  }
}

bool CallStack::IsConnectWorkerInflight() const {
  return UiState()->connect_in_flight;
}

std::shared_ptr<const MeshLocalView> CallStack::LocalMeshView() const {
  MeshMediaPlane* shared = mesh_media();
  return shared ? shared->LocalView() : std::make_shared<const MeshLocalView>();
}

std::vector<std::string> CallStack::LocalCallListenMultiaddrs() const {
  const auto view = LocalMeshView();
  const bool amp_up = view->amp_up && !view->amp_listen_multiaddr.empty();
  if (!amp_up) {
    return {};
  }

  const bool listening =
      ResolveMeshRole(*mesh_config()) == MeshRole::Node || amp_up || WantEphemeralListen();
  if (!listening) {
    return {};
  }

  std::vector<std::string> addrs = view->advertised_listen_multiaddrs;
  if (addrs.empty()) {
    addrs.push_back(view->amp_listen_multiaddr);
  }
  // B40: a peer dialed our 169.254.x link-local address first. Never advertise addresses the
  // peer cannot dial (link-local, wildcard, loopback) in call signaling.
  std::erase_if(addrs, [](const std::string& ma) { return !IsUsableAdpListen(ma); });
  return RankAmpDialMultiaddrs(std::move(addrs));
}

void CallStack::RegisterCallPeerListenMultiaddrsOnOwner(const std::string& identity,
                                                 const std::vector<std::string>& multiaddrs) {
  if (media_plane_) {
    media_plane_->RegisterCallPeerListenMultiaddrs(identity, multiaddrs);
  }
}

CallSessionManager* CallStack::Calls() {
  return call_sessions_.get();
}

void CallStack::BindCallState() {
  PBR_ASSERT_ON_OWNER(OwnerThreadId::MediaSessions);
  if (!call_sessions_) {
    return;
  }
  call_state_binds_.fetch_add(1, std::memory_order_relaxed);
  call_sessions_->SetOnCallStateChanged([this]() { OnCallStateChangedOnOwner(); });
  OnCallStateChangedOnOwner();
}

void CallStack::SetOnChromeRefresh(std::function<void()> fn) {
  CallsThread::RunAndWait([this, &fn]() { on_chrome_refresh_ = std::move(fn); });
}

void CallStack::OnCallStateChangedOnOwner() {
  const bool want = call_sessions_ && call_sessions_->Live().Phase() != CallPhase::Idle;
  if (want != want_ephemeral_listen_) {
    want_ephemeral_listen_ = want;
    log().info << "WantEphemeralListen=" << (want ? 1 : 0)
               << " phase=" << CallPhaseName(call_sessions_ ? call_sessions_->Live().Phase() : CallPhase::Idle);
    SyncHubEphemeralListen();
  }
  // GUI boundary: the chrome refresh runs on UI.
  if (!on_chrome_refresh_) {
    return;
  }
  AppRuntime::PostUI(chrome_self_.Bind([refresh = on_chrome_refresh_]() { refresh(); }));
}

void CallStack::SyncHubEphemeralListen() {
  if (!deps_.sync_mobile_ephemeral_listen) {
    return;
  }
  // The hub reads WantEphemeralListen from the snapshot: publish before it looks.
  if (CallsThread::IsCurrent()) {
    PublishUiState();
  }
  if (AppRuntime::CurrentlyOnUI()) {
    deps_.sync_mobile_ephemeral_listen();
    return;
  }
  // Owner / receive threads: the hub (N025 listen) runs on UI.
  AppRuntime::PostUI([sync = deps_.sync_mobile_ephemeral_listen]() { sync(); });
}

void CallStack::ResetSessionsOnOwner() {
  PBR_ASSERT_ON_OWNER(OwnerThreadId::MediaSessions);
  if (deps_.bind_call_control) {
    deps_.bind_call_control({});
  }
  call_sessions_.reset();
  ++sessions_generation_;       // its events still queued go nowhere
  chrome_self_.Invalidate();    // refreshes queued for the dropped sessions (a later bind posts fresh ones)
  OnCallStateChangedOnOwner();  // nothing is shown any more
  PublishUiState();
}

void CallStack::Shutdown() {
  CallsThread::RunAndWait([this]() {
    if (call_sessions_) {
      call_sessions_->ClearMediaCallbacks();
      call_sessions_->SetOnCallStateChanged({});
    }
    chrome_self_.Invalidate();  // no chrome refresh after shutdown
  });
  // LeaveCall / DeclineInvite workers must finish while sessions_ / seat still live. From the
  // caller, not the owner: the drain pumps UI and the owners.
  if (!AppRuntime::DrainWorkersThenUI(std::chrono::milliseconds(2000))) {
    log().warning << "CallStack::Shutdown: DrainWorkersThenUI budget exceeded";
  }
  CallsThread::RunAndWait([this]() { ReleaseOnOwner(); });
}

void CallStack::ReleaseOnOwner() {
  PBR_ASSERT_ON_OWNER(OwnerThreadId::MediaSessions);
  mobility_wake_.Disarm();
  DetachMeshMedia();
  if (MeshMediaPlane* shared = mesh_media()) {
    shared->SetOnRelayChosen({});
    shared->SetSignalingPunch({});
  }
  // Capture / receive threads call into the bridge (send callback): stop them before it goes.
  if (call_media_engine_ && (call_media_engine_->IsActive() || call_media_engine_->IsSfuMode())) {
    call_media_engine_->Stop();
  }
  if (call_sessions_) {
    call_sessions_->DropDirectPath();  // before its transport goes with the plane
  }
  if (media_plane_) {
    media_plane_->Clear();
  }
  call_sessions_.reset();
  call_media_engine_.reset();
  call_media_keys_.reset();
  call_session_store_.reset();
  PublishUiState();
}


} // namespace pbr
