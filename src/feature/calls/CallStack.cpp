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
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

CallStack::CallStack() {
  redirectLogger("CallStack");
  media_plane_ = std::make_unique<CallMediaPlane>();
  call_lifecycle_ = std::make_unique<CallLifecycle>();
  publish_hook_ = CallsThread::AddAfterTaskHook([this]() { PublishUiState(); });
}

CallStack::~CallStack() {
  mobility_alive_->store(false, std::memory_order_release);
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

void CallStack::OnLocalNetwork(const MobilityAttachment& attachment, const bool changed, const bool moved) {
  CallsThread::Post([this, attachment, changed, moved]() {
    local_mobility_.OnAttachment(attachment, changed, MobilityClassifier::Clock::now());
    ReevaluateLocalMobilityOnOwner();
    if (moved && media_plane_) {
      if (CallMediaBridge* bridge = media_plane_->Bridge()) {
        bridge->OnLocalNetworkChanged();
      }
    }
  });
}

void CallStack::OnObservedAddressChanged() {
  CallsThread::Post([this]() {
    local_mobility_.OnObservedAddressChanged(MobilityClassifier::Clock::now());
    ReevaluateLocalMobilityOnOwner();
  });
}

void CallStack::ReloadMobilityOverride() {
  CallsThread::Post([this]() { ApplyMobilityOverrideOnOwner(); });
}

void CallStack::ApplyMobilityOverrideOnOwner() {
  const auto cfg = mesh_config();
  const auto pinned = ResolveMobilityOverride(cfg ? cfg->mobility : std::string("auto"));
  local_mobility_.SetOverride(pinned);
  if (pinned) {
    log().info << "mobility pinned to " << MobilityClassWire(*pinned);
  }
  ReevaluateLocalMobilityOnOwner();
}

void CallStack::ReevaluateLocalMobilityOnOwner() {
  const MobilityClass before = local_mobility_published_.load(std::memory_order_acquire);
  const MobilityClass now = local_mobility_.Evaluate(MobilityClassifier::Clock::now());
  local_mobility_published_.store(now, std::memory_order_release);
  ScheduleMobilityReevaluationOnOwner();
  if (now == before) {
    return;
  }
  log().info << "mobility " << MobilityClassWire(before) << " -> " << MobilityClassWire(now);
  if (!call_sessions_) {
    return;
  }
  auto active = call_sessions_->ActiveLocalCall();
  if (active && active->has_value()) {
    call_sessions_->AnnounceCapsUpdate();
    NotifyPathPolicyChangedOnOwner((*active)->call_id);
  }
}

void CallStack::ScheduleMobilityReevaluationOnOwner() {
  CancelMobilityReevaluationOnOwner();
  const auto at = local_mobility_.NextReevaluationAt(MobilityClassifier::Clock::now());
  if (!at) {
    return;
  }
  const auto delay = std::max(std::chrono::duration_cast<std::chrono::milliseconds>(
                                  *at - MobilityClassifier::Clock::now()),
                              std::chrono::milliseconds(1));
  mobility_timer_id_ = AppRuntime::ScheduleCoordinatorOneShot(delay, [this, alive = mobility_alive_]() {
    CallsThread::Post([this, alive]() {
      if (alive->load(std::memory_order_acquire)) {
        mobility_timer_id_ = 0;
        ReevaluateLocalMobilityOnOwner();
      }
    });
  });
}

void CallStack::CancelMobilityReevaluationOnOwner() {
  if (mobility_timer_id_ != 0) {
    AppRuntime::CancelCoordinatorTimer(mobility_timer_id_);
    mobility_timer_id_ = 0;
  }
}

void CallStack::NoteRemoteMobilityOnOwner(const std::string& call_id, const MobilityClass mobility) {
  if (call_id.empty()) {
    return;
  }
  if (remote_mobility_.size() > 32 && !remote_mobility_.contains(call_id)) {
    remote_mobility_.clear();  // one live call at a time; old entries are history
  }
  auto [it, inserted] = remote_mobility_.try_emplace(call_id, mobility);
  if (!inserted && it->second == mobility) {
    return;
  }
  it->second = mobility;
  NotifyPathPolicyChangedOnOwner(call_id);
}

void CallStack::NotifyPathPolicyChangedOnOwner(const std::string& call_id) {
  if (media_plane_) {
    if (CallMediaBridge* bridge = media_plane_->Bridge()) {
      bridge->OnPathPolicyChanged(call_id);
    }
  }
}

CallPathPolicy CallStack::PathPolicyFor(const std::string& call_id) const {
  const auto it = remote_mobility_.find(call_id);
  return DecideCallPathPolicy(local_mobility_.Class(),
                              it == remote_mobility_.end() ? MobilityClass::Unknown : it->second);
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

void CallStack::RunOnOwner(const std::function<void(CallSessionManager&)>& op) {
  CallsThread::RunAndWait([&]() {
    if (call_sessions_) {
      op(*call_sessions_);
    }
  });
}

void CallStack::PublishUiState() {
  CallUiState state;
  if (call_lifecycle_) {
    state.phase = call_lifecycle_->Phase();
    state.media_status = call_lifecycle_->Status();
    state.active_call_id = call_lifecycle_->ActiveCallId();
    state.accepting_call_id = call_lifecycle_->AcceptingCallId();
    state.last_ring_call_id = call_lifecycle_->LastRingCallId();
    state.last_error = call_lifecycle_->LastError();
  }
  if (call_sessions_) {
    if (const LiveCall* ended = call_sessions_->Live().LastEnded(); ended && ended->EndedByPeer()) {
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
  if (call_media_seat_) {
    state.seat_bound_call_id = call_media_seat_->BoundCallId();
    state.seat_state = call_media_seat_->State();
    state.seat_live = call_media_seat_->IsLive(state.seat_bound_call_id);
  }
  state.want_ephemeral_listen = call_lifecycle_ && call_lifecycle_->WantEphemeralListen();
  state.connect_in_flight = media_plane_ && media_plane_->IsConnectWorkerInflight();
  state.sessions_identity = call_sessions_.get();
  state.available = call_sessions_ != nullptr && call_lifecycle_ != nullptr;
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
  plane_deps.note_mesh_peer_id_for_relay = [this](const std::string& account,
                                                 const std::string& peer_id) {
    if (call_sessions_) {
      call_sessions_->NoteMeshPeerIdForRelay(account, peer_id);
    }
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
  shared->SetOnRelayChosen([this](const std::string& circuit_r1) {
    CallsThread::Post([this, circuit_r1]() {
      if (call_sessions_) {
        call_sessions_->AnnounceCircuitR1(circuit_r1);
      }
    });
  });
  // H012: when Amp introducers are exhausted, exchange punch candidates over call-control.
  shared->SetSignalingPunch([this](const std::string& target_peer_id, const std::vector<std::string>& my_addrs,
                                   std::function<void(Roe<void>)> on_done) {
    CallsThread::Post([this, target_peer_id, my_addrs, on_done = std::move(on_done)]() mutable {
      if (!call_sessions_) {
        on_done(Error("Calls unavailable"));
        return;
      }
      call_sessions_->RequestSignalingPunch(target_peer_id, my_addrs, std::move(on_done));
    });
  });
}

void CallStack::DetachMeshMediaOnOwner() {
  if (call_sessions_) {
    call_sessions_->SetMediaRelayDeps({});
  }
  if (media_plane_) {
    media_plane_->DetachFromMeshMedia();
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
  CallMediaBridgeBindArgs args;
  args.host = &call_sessions_->AsMediaHost();
  args.session_store = call_session_store_.get();
  args.media_keys = call_media_keys_.get();
  args.media_engine = call_media_engine_.get();
  args.sessions_key = call_sessions_.get();
  media_plane_->BindBridge(args);
  if (CallMediaBridge* bridge = media_plane_->Bridge()) {
    bridge->SetPathPolicyProvider([this](const std::string& call_id) { return PathPolicyFor(call_id); });
  }
  call_sessions_->SetMediaRelayDeps(media_plane_->BuildMediaRelayDeps());
  call_sessions_->SetDirectMediaPorts(
      MakeDirectMediaPorts());
  call_sessions_->SetDirectDriver(media_plane_ ? media_plane_->Bridge() : nullptr);
  if (call_media_seat_) {
    call_sessions_->SetTopologySeatPorts(MakeTopologySeatPorts());
    call_sessions_->SetMediaSeatPorts(call_sessions_->MakeSeatPorts(call_media_seat_.get()));
    call_sessions_->SetCallMediaSeat(call_media_seat_.get());
  }
  // Lifecycle ↔ sessions / bridge ports (mesh stop cleared them): one bind point with BuildSessions.
  EnsureCallLifecycleBound();
  if (CallMediaBridge* bridge = media_plane_->Bridge()) {
    bridge->SetSeatPorts(MakeDirectSeatPorts());
  }
  PublishUiState();
}

CallLifecycleSignalingPorts CallStack::MakeLifecycleSignalingPorts() {
  CallLifecycleSignalingPorts ports;
  ports.accept_invite = [this](const std::string& call_id, std::function<void(Roe<void>)> done) {
    if (!call_sessions_) {
      done(Error("Calls unavailable"));
      return;
    }
    call_sessions_->AcceptInviteAsync(call_id, std::move(done));
  };
  ports.decline_invite = [this](const std::string& call_id) -> Roe<void> {
    if (!call_sessions_) {
      return Error("Calls unavailable");
    }
    return call_sessions_->DeclineInvite(call_id);
  };
  ports.leave_call = [this](const std::string& call_id) -> Roe<void> {
    if (!call_sessions_) {
      return Error("Calls unavailable");
    }
    return call_sessions_->LeaveCall(call_id);
  };
  ports.retry_p2p_media = [this](const std::string& call_id) -> Roe<void> {
    if (!call_sessions_) {
      return Error("Calls unavailable");
    }
    return call_sessions_->RetryP2pMedia(call_id);
  };
  ports.resume_p2p_media = [this](const std::string& call_id) -> Roe<void> {
    if (!call_sessions_) {
      return Error("Calls unavailable");
    }
    return call_sessions_->ResumeP2pMedia(call_id);
  };
  ports.kick_answerer_direct_media = [this](const std::string& call_id) {
    if (call_sessions_) {
      call_sessions_->KickAnswererDirectMediaIfArmed(call_id);
    }
  };
  ports.media_active_for_call = [this](const std::string& call_id) {
    return call_sessions_ && call_sessions_->Media().IsActive() &&
           call_sessions_->Media().ActiveCallId() == call_id;
  };
  return ports;
}

void CallStack::BindSeatTeardown() {
  if (!call_media_seat_) {
    return;
  }
  call_media_seat_->SetTeardownHooks(
      [this](const std::string& call_id) {
        if (call_sessions_) {
          call_sessions_->TopologyOnMediaStoppedForSeat(call_id);
        }
      },
      [this](const std::string& call_id, uint64_t epoch_at_post, bool force) {
        if (!force && call_media_seat_ && call_media_seat_->Epoch() != epoch_at_post) {
          log().info << "MediaSeat stop skip stale call_id=" << call_id
                     << " posted_epoch=" << epoch_at_post
                     << " seat_epoch=" << call_media_seat_->Epoch();
          return;
        }
        if (media_plane_ && media_plane_->Bridge()) {
          media_plane_->StopMeshMedia(call_id);
          return;
        }
        if (!call_media_engine_) {
          return;
        }
        if (!call_media_engine_->IsActive() && !call_media_engine_->IsSfuMode()) {
          return;
        }
        call_media_engine_->Stop();
      });
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
  call_media_seat_ = std::make_unique<CallMediaSeat>();
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
  if (call_media_seat_) {
    call_sessions_->SetTopologySeatPorts(MakeTopologySeatPorts());
    call_sessions_->SetMediaSeatPorts(call_sessions_->MakeSeatPorts(call_media_seat_.get()));
    call_sessions_->SetCallMediaSeat(call_media_seat_.get());
    BindSeatTeardown();
  }
  if (deps_.bind_call_control) {
    CallControlInboundPorts inbound;
    // Receive threads hand call control to the calls owner (thread-ownership t2a, T003): fire and
    // forget with a copy — ApplyInboundControl only reads the message; failures are logged there.
    inbound.apply_inbound_control = [this](ThreadMessage& message, const std::string& sender_identity,
                                           std::optional<int64_t> relay_created_at_ms,
                                           std::optional<int64_t> relay_server_time_ms) -> Roe<void> {
      CallsThread::Post([this, message, sender_identity, relay_created_at_ms,
                                                                  relay_server_time_ms]() mutable {
        if (!call_sessions_) {
          return;
        }
        if (auto applied = call_sessions_->ApplyInboundControl(message, sender_identity, relay_created_at_ms,
                                                               relay_server_time_ms);
            !applied) {
          log().warning << "inbound call control failed message_id=" << message.id
                        << " err=" << applied.error().message;
        }
      });
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
  call_sessions_->AbandonOrphanedCallsAfterRestart();
  call_sessions_->SetOnRingChangedMesh([this]() { SyncHubEphemeralListen(); });
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
    NoteRemoteMobilityOnOwner(call_id, caps.mobility);
  });
  call_sessions_->SetLocalPeerCapsProvider([this]() {
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
  });
  call_sessions_->SetRegisterPeerListenMultiaddrs(
      [this](const std::string& identity, const std::vector<std::string>& multiaddrs) {
        RegisterCallPeerListenMultiaddrs(identity, multiaddrs);
      });
  // Connectivity: park on org hops so this peer is ServeDial-reachable (shared mesh media).
  call_sessions_->SetEnsureCircuitReady([this]() {
    if (MeshMediaPlane* shared = mesh_media()) {
      shared->Rendezvous().ReserveOnBootstrapSeeds();
    }
  });
  // Park completes on the Amp IO thread (or at a coordinator deadline): back onto the calls owner.
  call_sessions_->SetParkCircuit([this](int timeout_ms, std::function<void(bool)> done) {
    auto on_owner = [done = std::move(done)](bool ready) {
      CallsThread::Post([done, ready]() { done(ready); });
    };
    if (MeshMediaPlane* shared = mesh_media()) {
      shared->Rendezvous().EnsureBootstrapSeedParkedAsync(std::move(on_owner), timeout_ms);
    } else {
      on_owner(false);
    }
  });
  call_sessions_->SetPreferLateReserve([this](const std::string& relay_peer_id) {
    if (MeshMediaPlane* shared = mesh_media()) {
      shared->Rendezvous().PreferLateReserve(relay_peer_id);
    }
  });
  call_sessions_->SetLocalPunchAddrsProvider(
      [this]() -> std::vector<std::string> { return LocalMeshView()->punch_candidate_addrs; });
  call_sessions_->SetSignalingPunchBurst(
      [this](const std::vector<std::string>& peer_addrs, int window_ms, std::function<void(Roe<void>)> on_done) {
        MeshMediaPlane* shared = mesh_media();
        if (!shared) {
          if (on_done) {
            on_done(Error("amp punch unavailable"));
          }
          return;
        }
        shared->SignalingPunchBurstAsync(peer_addrs, window_ms, std::move(on_done));
      });
  EnsureCallLifecycleBound();
  RebindMeshMedia();
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
    return call_lifecycle_ && call_lifecycle_->WantEphemeralListen();
  }
  return UiState()->want_ephemeral_listen;
}

void CallStack::PrepareForMeshStopOnOwner(const std::function<void()>& abort_inflight_circuit) {
  PBR_ASSERT_ON_OWNER(OwnerThreadId::MediaSessions);
  if (call_lifecycle_) {
    call_lifecycle_->ClearBinding();
  }
  if (call_sessions_) {
    call_sessions_->SetDirectMediaPorts({});
    call_sessions_->SetDirectDriver(nullptr);
    call_sessions_->SetLifecyclePorts({});
    call_sessions_->SetMediaSeatPorts({});
    call_sessions_->SetCallMediaSeat(nullptr);
    call_sessions_->SetTopologyHopArmingPorts({});
    call_sessions_->SetTopologySeatPorts({});
  }
  DetachMeshMedia();
  if (media_plane_) {
    if (CallMediaBridge* bridge = media_plane_->Bridge()) {
      bridge->SetDirectArmingPorts({});
      bridge->SetSeatPorts({});
    }
    media_plane_->PrepareForMeshStop(abort_inflight_circuit);
  } else if (abort_inflight_circuit) {
    abort_inflight_circuit();
    abort_inflight_circuit();
  }
}

void CallStack::FinishMeshStopOnOwner() {
  if (call_sessions_) {
    call_sessions_->SetDirectDriver(nullptr);  // the bridge goes with the plane's mesh stop
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
  if (media_plane_) {
    media_plane_->AbortBridgeAndTransport();
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

CallLifecycle* CallStack::Lifecycle() {
  // Accessor only: created with the stack, bound at BuildSessions / BindMediaProducts (the owner's
  // bind points) — never created or rebound on access from another thread.
  return call_lifecycle_.get();
}

void CallStack::EnsureCallLifecycleBound() {
  PBR_ASSERT_ON_OWNER(OwnerThreadId::MediaSessions);
  if (!call_sessions_) {
    if (call_lifecycle_) {
      call_lifecycle_->ClearBinding();
    }
    return;
  }
  if (!call_lifecycle_) {
    call_lifecycle_ = std::make_unique<CallLifecycle>();
  }
  lifecycle_port_binds_.fetch_add(1, std::memory_order_relaxed);
  call_lifecycle_->BindSignalingPorts(MakeLifecycleSignalingPorts());
  call_lifecycle_->SetOnListenDesireChanged([this](bool want) { SetEphemeralListenDesire(want); });
  call_sessions_->SetTopologyHopArmingPorts(MakeHopArmingPorts());
  call_sessions_->SetLifecyclePorts(MakeSessionLifecyclePorts());
  if (media_plane_) {
    if (CallMediaBridge* bridge = media_plane_->Bridge()) {
      bridge->SetDirectArmingPorts(MakeDirectArmingPorts());
      bridge->SetSeatPorts(MakeDirectSeatPorts());
    }
  }
}

void CallStack::SetEphemeralListenDesire(bool /*want*/) {
  // Desire already stored on CallLifecycle; this only wakes Hub N025 listen execution.
  SyncHubEphemeralListen();
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
  if (call_lifecycle_) {
    call_lifecycle_->ClearBinding();  // its ports point at the sessions being dropped
  }
  call_sessions_.reset();
  PublishUiState();
}

void CallStack::Shutdown() {
  CallsThread::RunAndWait([this]() {
    if (call_sessions_) {
      call_sessions_->ClearMediaCallbacks();
    }
    if (call_lifecycle_) {
      call_lifecycle_->ClearBinding();
    }
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
  CancelMobilityReevaluationOnOwner();
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
    call_sessions_->SetDirectDriver(nullptr);
  }
  if (media_plane_) {
    media_plane_->Clear();
  }
  call_lifecycle_.reset();
  call_sessions_.reset();
  if (call_media_seat_) {
    call_media_seat_->SetTeardownHooks({}, {});
  }
  call_media_seat_.reset();
  call_media_engine_.reset();
  call_media_keys_.reset();
  call_session_store_.reset();
  PublishUiState();
}


CallHopArmingPorts CallStack::MakeHopArmingPorts() const {
  CallHopArmingPorts ports;
  CallLifecycle* lifecycle = call_lifecycle_.get();
  if (!lifecycle) {
    return ports;
  }
  ports.hop_ops_allowed = [lifecycle]() { return lifecycle->AllowsHopPath(); };
  ports.soft_migrate_may_arm = [lifecycle]() {
    const CallMediaStatus st = lifecycle->Status();
    return st == CallMediaStatus::DirectLive || st == CallMediaStatus::DirectConnecting ||
           st == CallMediaStatus::DegradedTxOnly || st == CallMediaStatus::Deciding ||
           st == CallMediaStatus::None;
  };
  ports.media_cancel_gen = [lifecycle]() { return lifecycle->MediaCancelGen(); };
  ports.report_progress = [lifecycle](CallHopPlannerPhase phase, const std::string& call_id) {
    CallMediaStatus mapped = CallMediaStatus::None;
    switch (phase) {
    case CallHopPlannerPhase::WaitingAttach:
      mapped = CallMediaStatus::HopWaiting;
      break;
    case CallHopPlannerPhase::Attaching:
      mapped = CallMediaStatus::HopAttaching;
      break;
    case CallHopPlannerPhase::Live:
      mapped = CallMediaStatus::HopLive;
      break;
    case CallHopPlannerPhase::Migrating:
      mapped = CallMediaStatus::Migrating;
      break;
    case CallHopPlannerPhase::Idle:
    case CallHopPlannerPhase::Stopping:
      return;
    }
    lifecycle->SetMediaStatus(mapped, call_id);
  };
  ports.arming_debug_name = [lifecycle]() { return CallMediaStatusName(lifecycle->Status()); };
  return ports;
}

CallDirectArmingPorts CallStack::MakeDirectArmingPorts() const {
  CallDirectArmingPorts ports;
  CallLifecycle* lifecycle = call_lifecycle_.get();
  if (!lifecycle) {
    return ports;
  }
  ports.direct_ops_allowed = [lifecycle]() { return lifecycle->AllowsDirectPath(); };
  ports.request_direct_arming = [lifecycle](const std::string& call_id) {
    if (lifecycle->AllowsDirectPath()) {
      return;
    }
    const CallPhase phase = lifecycle->Phase();
    if (phase == CallPhase::Accepting || phase == CallPhase::JoinedLocal ||
        phase == CallPhase::MediaPending || phase == CallPhase::MediaConnecting) {
      lifecycle->SetMediaStatus(CallMediaStatus::DirectConnecting, call_id);
    }
  };
  ports.report_progress = [lifecycle](CallDirectPlannerPhase phase, const std::string& call_id) {
    CallMediaStatus mapped = CallMediaStatus::None;
    switch (phase) {
    case CallDirectPlannerPhase::Arming:
    case CallDirectPlannerPhase::Connecting:
    case CallDirectPlannerPhase::KeyWait:
      mapped = CallMediaStatus::DirectConnecting;
      break;
    case CallDirectPlannerPhase::Live:
      return;
    case CallDirectPlannerPhase::DegradedTxOnly:
      mapped = CallMediaStatus::DegradedTxOnly;
      break;
    case CallDirectPlannerPhase::Reconnecting:
      mapped = CallMediaStatus::Reconnecting;
      break;
    case CallDirectPlannerPhase::Idle:
    case CallDirectPlannerPhase::Stopping:
      return;
    }
    lifecycle->SetMediaStatus(mapped, call_id);
  };
  ports.on_connected = [lifecycle](const std::string& call_id) {
    lifecycle->Apply(CallLifecycleEvent::DirectConnected, call_id);
  };
  ports.on_connect_failed = [lifecycle](const std::string& call_id) {
    lifecycle->Apply(CallLifecycleEvent::ConnectFailedEvt, call_id);
  };
  ports.on_peer_reconnected = [lifecycle](const std::string& call_id) {
    lifecycle->Apply(CallLifecycleEvent::PeerReconnected, call_id);
  };
  ports.on_media_deferred = [lifecycle](const std::string& call_id) {
    lifecycle->Apply(CallLifecycleEvent::MediaDeferred, call_id);
  };
  ports.on_media_key_ready = [lifecycle](const std::string& call_id) {
    lifecycle->Apply(CallLifecycleEvent::MediaKeyReady, call_id);
  };
  ports.arming_debug_name = [lifecycle]() { return CallMediaStatusName(lifecycle->Status()); };
  return ports;
}

CallSessionLifecyclePorts CallStack::MakeSessionLifecyclePorts() const {
  CallSessionLifecyclePorts ports;
  CallLifecycle* lifecycle = call_lifecycle_.get();
  if (!lifecycle) {
    return ports;
  }
  ports.allows_direct_path = [lifecycle]() { return lifecycle->AllowsDirectPath(); };
  ports.status_name = [lifecycle]() { return CallMediaStatusName(lifecycle->Status()); };
  ports.armed_planner_name = [lifecycle]() {
    return CallArmedPlannerName(lifecycle->ArmedPlanner());
  };
  ports.set_direct_connecting = [lifecycle](const std::string& call_id) {
    lifecycle->SetMediaStatus(CallMediaStatus::DirectConnecting, call_id);
  };
  ports.apply_outbound_started = [lifecycle](const std::string& call_id) {
    lifecycle->Apply(CallLifecycleEvent::OutboundStarted, call_id);
  };
  ports.accepting_call_id = [lifecycle]() { return lifecycle->AcceptingCallId(); };
  ports.active_call_id = [lifecycle]() { return lifecycle->ActiveCallId(); };
  ports.apply_remote_ended = [lifecycle](const std::string& call_id) {
    lifecycle->Apply(CallLifecycleEvent::RemoteEnded, call_id);
  };
  ports.is_outbound_calling = [lifecycle]() {
    return lifecycle->Phase() == CallPhase::OutboundCalling;
  };
  return ports;
}

CallDirectMediaPorts CallStack::MakeDirectMediaPorts() const {
  CallDirectMediaPorts ports;
  CallMediaBridge* bridge = media_plane_ ? media_plane_->Bridge() : nullptr;
  if (!bridge) {
    return ports;
  }
  ports.media_path_kind = [bridge]() { return bridge->MediaPathKind(); };
  ports.note_peer_id_relay_mapping = [bridge](const std::string& peer_id,
                                              const std::string& relay_identity) {
    bridge->NotePeerIdRelayMapping(peer_id, relay_identity);
  };
  ports.stop_mesh_media = [bridge](const std::string& call_id) { bridge->StopMeshMedia(call_id); };
  ports.is_connect_failed = [bridge]() { return bridge->IsMeshConnectFailed(); };
  ports.connect_missing_mic = [bridge]() {
    return bridge->IsMeshConnectFailed() && bridge->MeshConnectMissingMic();
  };
  ports.poll_connect_health = [bridge]() { bridge->PollMeshConnectHealth(); };
  ports.retry_mesh_media = [bridge](const std::string& call_id) {
    return bridge->RetryMeshMedia(call_id);
  };
  ports.resume_mesh_media = [bridge](const std::string& call_id) {
    return bridge->ResumeMeshMediaFromInbound(call_id);
  };
  ports.media_attempted = [bridge](const std::string& call_id) {
    return bridge->MediaAttempted(call_id);
  };
  ports.note_media_attempted = [bridge](const std::string& call_id) {
    bridge->NoteMediaAttempted(call_id);
  };
  ports.on_media_key_ready = [bridge](const std::string& call_id) {
    bridge->OnMediaKeyReady(call_id);
  };
  return ports;
}

CallDirectSeatPorts CallStack::MakeDirectSeatPorts() const {
  CallDirectSeatPorts ports;
  CallMediaSeat* seat = call_media_seat_.get();
  if (!seat) {
    return ports;
  }
  ports.acquire = [seat](const std::string& call_id) { return seat->Acquire(call_id); };
  ports.allows_path_op = [seat](const CallMediaSeat::Token& token) {
    return seat->AllowsPathOp(token);
  };
  ports.current_token = [seat]() { return seat->CurrentToken(); };
  ports.bound_call_id = [seat]() { return seat->BoundCallId(); };
  ports.note_connecting = [seat](const std::string& call_id) { seat->NoteConnecting(call_id); };
  ports.note_start = [seat](const std::string& call_id) { seat->NoteStart(call_id); };
  ports.note_path = [seat](CallMediaSeat::PathKind kind) { seat->NotePath(kind); };
  ports.note_live = [seat](const std::string& call_id) { seat->NoteLive(call_id); };
  ports.note_failed = [seat](const std::string& call_id) { seat->NoteFailed(call_id); };
  return ports;
}

CallTopologySeatPorts CallStack::MakeTopologySeatPorts() const {
  CallTopologySeatPorts ports;
  CallMediaSeat* seat = call_media_seat_.get();
  if (!seat) {
    return ports;
  }
  ports.is_bound = [seat](const std::string& call_id) { return seat->IsBound(call_id); };
  ports.bound_call_id = [seat]() { return seat->BoundCallId(); };
  ports.acquire = [seat](const std::string& call_id) { return seat->Acquire(call_id); };
  ports.allows_path_op = [seat](const CallMediaSeat::Token& token) {
    return seat->AllowsPathOp(token);
  };
  ports.begin_attach = [seat](const std::string& call_id, const std::string& hop,
                              CallMediaSeat::AttachTicket* ticket) {
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
  ports.cancel_attach_for_call = [seat](const std::string& call_id) {
    seat->CancelAttachForCall(call_id);
  };
  return ports;
}

} // namespace pbr
