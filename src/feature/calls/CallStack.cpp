#include "feature/calls/CallStack.h"

#include "foundation/data/MeshRole.h"
#include "domain/mesh/host/MeshPorts.h"
#include "domain/messaging/CallTypes.h"
#include "domain/messaging/PeerCapsLogic.h"
#include "domain/people/ContactsStore.h"
#include "domain/people/IdentityStore.h"
#include "foundation/runtime/AppRuntime.h"
#include "domain/mesh/host/MeshControlDispatch.h"
#include "domain/messaging/SqlitePskSessionStore.h"
#include "domain/mesh/reachability/Reachability.h"
#include "feature/calls/CallMediaBridge.h"
#include "feature/calls/CallMediaPaths.h"
#include "domain/messaging/CallLifecycleTypes.h"

#include <functional>
#include <optional>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

CallStack::CallStack() {
  redirectLogger("CallStack");
  media_plane_ = std::make_unique<CallMediaPlane>();
}

CallStack::~CallStack() {
  Shutdown();
}

const AppConfig& CallStack::config() const {
  return deps_.config();
}

void CallStack::SyncMediaPlaneDeps() {
  if (!media_plane_) {
    return;
  }
  CallMediaPlaneDeps plane_deps;
  plane_deps.contacts = deps_.contacts;
  plane_deps.mesh = deps_.mesh;
  plane_deps.config = deps_.config;
  plane_deps.list_directory_nodes = deps_.list_directory_nodes;
  plane_deps.list_dht_nodes = deps_.list_dht_nodes;
  plane_deps.seed_dial_ok = deps_.seed_dial_ok;
  plane_deps.note_lan_mdns_peer_id = deps_.note_lan_mdns_peer_id;
  plane_deps.register_peer_direct_endpoint = deps_.delivery.register_peer_direct_endpoint;
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
  plane_deps.announce_circuit_r1 = [this](const std::string& circuit_r1) {
    if (call_sessions_) {
      call_sessions_->AnnounceCircuitR1(circuit_r1);
    }
  };
  media_plane_->SetDeps(std::move(plane_deps));
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
  call_sessions_->SetMediaRelayDeps(media_plane_->BuildMediaRelayDeps());
  call_sessions_->SetDirectMediaPorts(
      MakeDirectMediaPorts());
  if (call_media_seat_) {
    call_sessions_->SetTopologySeatPorts(MakeTopologySeatPorts());
    call_sessions_->SetMediaSeatPorts(call_sessions_->MakeSeatPorts(call_media_seat_.get()));
  }
  if (call_lifecycle_) {
    call_sessions_->SetTopologyHopArmingPorts(MakeHopArmingPorts());
    call_sessions_->SetLifecyclePorts(MakeSessionLifecyclePorts());
    if (CallMediaBridge* bridge = media_plane_->Bridge()) {
      bridge->SetDirectArmingPorts(MakeDirectArmingPorts());
    }
  }
  if (CallMediaBridge* bridge = media_plane_->Bridge()) {
    bridge->SetSeatPorts(MakeDirectSeatPorts());
  }
}

CallLifecycleSignalingPorts CallStack::MakeLifecycleSignalingPorts() {
  CallLifecycleSignalingPorts ports;
  ports.accept_invite = [this](const std::string& call_id) -> Roe<void> {
    if (!call_sessions_) {
      return Error("Calls unavailable");
    }
    return call_sessions_->AcceptInvite(call_id);
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
  call_session_store_ = std::make_unique<CallSessionStore>(profile_db_path);
  call_media_keys_ = std::make_unique<CallMediaKeyStore>(profile_db_path, profile_id);
  call_media_engine_ = std::make_unique<CallMediaEngine>();
  call_media_seat_ = std::make_unique<CallMediaSeat>();
  if (!media_plane_) {
    media_plane_ = std::make_unique<CallMediaPlane>();
  }
  return {};
}

void CallStack::BuildSessions(const CallStackDeps& deps) {
  deps_ = deps;
  call_sessions_ = std::make_unique<CallSessionManager>(*deps_.store, *deps_.contacts, *deps_.identity,
                                                        *call_session_store_, *call_media_keys_, deps_.delivery,
                                                        *deps_.psk, *call_media_engine_);
  if (call_media_seat_) {
    call_sessions_->SetTopologySeatPorts(MakeTopologySeatPorts());
    call_sessions_->SetMediaSeatPorts(call_sessions_->MakeSeatPorts(call_media_seat_.get()));
    BindSeatTeardown();
  }
  if (deps_.bind_call_control) {
    CallControlInboundPorts inbound;
    inbound.apply_inbound_control = [this](ThreadMessage& message, const std::string& sender_identity,
                                           std::optional<int64_t> relay_created_at_ms,
                                           std::optional<int64_t> relay_server_time_ms) -> Roe<void> {
      if (!call_sessions_) {
        return {};
      }
      return call_sessions_->ApplyInboundControl(message, sender_identity, relay_created_at_ms,
                                                 relay_server_time_ms);
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
  call_sessions_->SetOnRingChangedMesh([this]() {
    EnsureCallLifecycleBound();
    // Invite ingest runs on IO — never Sync N025 on the IO thread itself.
    if (AppRuntime::CurrentlyOnUI()) {
      if (deps_.sync_mobile_ephemeral_listen) {
        deps_.sync_mobile_ephemeral_listen();
      }
    } else {
      AppRuntime::PostUI([this]() {
        if (deps_.sync_mobile_ephemeral_listen) {
          deps_.sync_mobile_ephemeral_listen();
        }
      });
    }
  });
  call_sessions_->SetPrefetchPeerReachability([this](const std::string& identity) {
    // Warm only; must not run RequestBridge on Critical ahead of AcceptInvite.
    MeshControlDispatch::Post([this, identity]() {
      if (deps_.prefetch_peer_reachability) {
        deps_.prefetch_peer_reachability(identity);
      }
    });
  });
  call_sessions_->SetLocalListenMultiaddrsProvider([this]() { return LocalCallListenMultiaddrs(); });
  call_sessions_->SetLocalMeshPeerIdProvider([this]() -> std::string {
    if (MeshHost* m = mesh(); m && m->Amp()) {
      return m->Amp()->LocalPeerId();
    }
    return {};
  });
  call_sessions_->SetLocalPeerCapsProvider([this]() {
    CallPeerCaps caps;
    caps.v = kCallPeerCapsVersion;
    caps.present = true;
    // Durable Node host only — never advertise media_relay for ephemeral listen-only (V030).
    caps.media_relay = ResolveMeshRole(config().mesh) == MeshRole::Node &&
                       config().mesh.capabilities.media_relay && mesh() && mesh()->Amp() &&
                       mesh()->AmpMediaRelayCoord() && mesh()->AmpMediaRelayCoord()->IsStarted();
    return caps;
  });
  call_sessions_->SetRegisterPeerListenMultiaddrs(
      [this](const std::string& identity, const std::vector<std::string>& multiaddrs) {
        RegisterCallPeerListenMultiaddrs(identity, multiaddrs);
      });
  call_sessions_->SetEnsureCircuitReady([this]() {
    if (media_plane_) {
      media_plane_->EnsureCircuitReady();
    }
  });
  call_sessions_->SetAwaitCircuitReady([this](int timeout_ms) {
    return media_plane_ ? media_plane_->AwaitCircuitReady(timeout_ms) : false;
  });
  call_sessions_->SetPreferLateReserve([this](const std::string& relay_peer_id) {
    if (media_plane_) {
      media_plane_->PreferLateReserve(relay_peer_id);
    }
  });
  EnsureCallLifecycleBound();
  WireMediaRelayDeps();
}

void CallStack::OnMeshServicesStarted() {
  SyncMediaPlaneDeps();
  if (media_plane_) {
    media_plane_->OnMeshStarted();
  }
  BindMediaProducts();
}

void CallStack::BindTestMediaPath(ICallMediaTransport* transport, IDialRegistry* dial) {
  BindTestMediaPath(transport, dial, nullptr);
}

void CallStack::BindTestMediaPath(ICallMediaTransport* transport, IDialRegistry* dial,
                                  ICircuitHopReach* circuit_reach) {
  SyncMediaPlaneDeps();
  if (media_plane_) {
    media_plane_->BindTestMediaPath(transport, dial, circuit_reach);
  }
  BindMediaProducts();
}

bool CallStack::HasActiveLocalCall() {
  if (call_lifecycle_ && call_lifecycle_->WantEphemeralListen()) {
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
  return call_lifecycle_ && call_lifecycle_->WantEphemeralListen();
}

void CallStack::WireMediaRelayDeps() {
  SyncMediaPlaneDeps();
  if (media_plane_) {
    media_plane_->Wire();
  }
  BindMediaProducts();
}

void CallStack::PrepareForMeshStop(const std::function<void()>& abort_inflight_circuit) {
  if (call_lifecycle_) {
    call_lifecycle_->ClearBinding();
  }
  if (call_sessions_) {
    call_sessions_->SetDirectMediaPorts({});
    call_sessions_->SetLifecyclePorts({});
    call_sessions_->SetMediaSeatPorts({});
    call_sessions_->SetTopologyHopArmingPorts({});
    call_sessions_->SetTopologySeatPorts({});
    call_sessions_->SetMediaRelayDeps({});
  }
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

void CallStack::FinishMeshStop() {
  if (media_plane_) {
    media_plane_->FinishMeshStop();
  }
}

void CallStack::AbortCallMediaForShutdown() {
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
      (void)call_sessions_->LeaveCall((*active)->call_id);
    }
  }
  if (media_plane_) {
    media_plane_->AbortBridgeAndTransport();
  }
}

bool CallStack::IsConnectWorkerInflight() const {
  return media_plane_ && media_plane_->IsConnectWorkerInflight();
}

std::vector<std::string> CallStack::LocalCallListenMultiaddrs() const {
  MeshHost* m = mesh();
  const bool amp_up = m && m->Amp() && !m->AmpListenMultiaddr().empty();
  if (!amp_up) {
    return {};
  }

  const bool listening =
      ResolveMeshRole(config().mesh) == MeshRole::Node || amp_up || WantEphemeralListen();
  if (!listening) {
    return {};
  }

  std::vector<std::string> addrs = m->AdvertisedListenMultiaddrs();
  if (addrs.empty() && !m->AmpListenMultiaddr().empty()) {
    addrs.push_back(m->AmpListenMultiaddr());
  }
  return RankAmpDialMultiaddrs(std::move(addrs));
}

void CallStack::RegisterCallPeerListenMultiaddrs(const std::string& identity,
                                                 const std::vector<std::string>& multiaddrs) {
  if (media_plane_) {
    media_plane_->RegisterCallPeerListenMultiaddrs(identity, multiaddrs);
  }
}

Roe<void> CallStack::TryEnsureCircuitHopReachable(const std::string& hop_peer_id) {
  if (!media_plane_) {
    return Error("Amp circuit reach required");
  }
  return media_plane_->TryEnsureCircuitHopReachable(hop_peer_id);
}

Roe<void> CallStack::TryEnsureCallMediaReachable(const std::string& peer_key) {
  if (!media_plane_) {
    return Error("Amp circuit reach required");
  }
  return media_plane_->TryEnsureCallMediaReachable(peer_key);
}

Roe<void> CallStack::TryUpgradeCallMediaToDirect(const std::string& peer_key) {
  if (!media_plane_) {
    return Error("amp circuit reach required");
  }
  return media_plane_->TryUpgradeCallMediaToDirect(peer_key);
}

void CallStack::WarmBootstrapSeedSessions() {
  if (media_plane_) {
    media_plane_->WarmBootstrapSeedSessions();
  }
}

void CallStack::ReserveOnBootstrapSeeds() {
  if (media_plane_) {
    media_plane_->ReserveOnBootstrapSeeds();
  }
}

CallSessionManager* CallStack::Calls() {
  return call_sessions_.get();
}

CallLifecycle* CallStack::Lifecycle() {
  EnsureCallLifecycleBound();
  return call_lifecycle_.get();
}

void CallStack::EnsureCallLifecycleBound() {
  if (!call_sessions_) {
    if (call_lifecycle_) {
      call_lifecycle_->ClearBinding();
    }
    return;
  }
  if (!call_lifecycle_) {
    call_lifecycle_ = std::make_unique<CallLifecycle>();
  }
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
  if (deps_.sync_mobile_ephemeral_listen) {
    deps_.sync_mobile_ephemeral_listen();
  }
}

void CallStack::ResetRelayClients() {
  if (media_plane_) {
    media_plane_->ResetRelayClients();
  }
}

void CallStack::ResetSessions() {
  if (deps_.bind_call_control) {
    deps_.bind_call_control({});
  }
  call_sessions_.reset();
}

void CallStack::Shutdown() {
  if (call_sessions_) {
    call_sessions_->ClearMediaCallbacks();
  }
  if (call_lifecycle_) {
    call_lifecycle_->ClearBinding();
  }
  // LeaveCall / DeclineInvite workers must finish while sessions_ / seat still live.
  if (!AppRuntime::DrainWorkersThenUI(std::chrono::milliseconds(2000))) {
    log().warning << "CallStack::Shutdown: DrainWorkersThenUI budget exceeded";
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
  CallMediaSeat* seat = call_media_seat_.get();
  if (!bridge) {
    return ports;
  }
  auto make_path = [bridge, seat]() {
    CallDirectPath::Ops ops;
    ops.schedule_start = [bridge](const std::string& cid, const std::string& p, bool off) {
      if (off) {
        bridge->ScheduleStartMediaAsOfferer(cid, p);
      } else {
        bridge->ScheduleStartMediaAsAnswerer(cid, p);
      }
    };
    ops.release_transport = [bridge](const CallMediaSeat::Token& token) {
      bridge->ReleaseDirectTransport(token);
    };
    if (seat) {
      ops.acquire = [seat](const std::string& cid) { return seat->Acquire(cid); };
      ops.allows_path_op = [seat](const CallMediaSeat::Token& token) {
        return seat->AllowsPathOp(token);
      };
      ops.note_path = [seat](CallMediaSeat::PathKind kind) { seat->NotePath(kind); };
    }
    return CallDirectPath(std::move(ops));
  };
  ports.schedule_start = [make_path](const std::string& call_id, const std::string& peer, bool offerer) {
    make_path().ScheduleStart(call_id, peer, offerer);
  };
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
  ports.media_attempted = [bridge](const std::string& call_id) {
    return bridge->MediaAttempted(call_id);
  };
  ports.note_media_attempted = [bridge](const std::string& call_id) {
    bridge->NoteMediaAttempted(call_id);
  };
  ports.release_direct_transport = [bridge, seat, make_path]() {
    if (seat) {
      (void)make_path().ReleaseTransport(seat->CurrentToken());
      return;
    }
    bridge->ReleaseDirectTransport();
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
