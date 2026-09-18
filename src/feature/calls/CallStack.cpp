#include "feature/calls/CallStack.h"

#include "foundation/data/MeshRole.h"
#include "domain/mesh/host/MeshPorts.h"
#include "domain/messaging/CallTypes.h"
#include "domain/people/ContactsStore.h"
#include "domain/people/IdentityStore.h"
#include "foundation/runtime/AppRuntime.h"
#include "domain/mesh/host/MeshControlDispatch.h"
#include "domain/messaging/SqlitePskSessionStore.h"
#include "domain/mesh/reachability/Reachability.h"
#include "feature/calls/CallLifecyclePorts.h"
#include "feature/calls/CallDirectMediaPorts.h"
#include "feature/calls/CallSessionLifecyclePorts.h"
#include "feature/calls/CallHopArmingPorts.h"
#include "feature/calls/CallDirectArmingPorts.h"
#include "feature/calls/CallTopologySeatPorts.h"

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
    return call_sessions_ && call_sessions_->PeerHasMediaRelayCap(peer_id);
  };
  plane_deps.list_media_relay_peers = [this]() {
    if (!call_sessions_) {
      return std::vector<std::string>{};
    }
    return call_sessions_->ListMediaRelayCapablePeerIds();
  };
  plane_deps.note_mesh_peer_id_for_relay = [this](const std::string& account,
                                                 const std::string& peer_id) {
    if (call_sessions_) {
      call_sessions_->NoteMeshPeerIdForRelay(account, peer_id);
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
  args.seat = call_media_seat_.get();
  args.sessions_key = call_sessions_.get();
  media_plane_->BindBridge(args);
  call_sessions_->SetMediaRelayDeps(media_plane_->BuildMediaRelayDeps());
  call_sessions_->SetDirectMediaPorts(
      MakeCallDirectMediaPorts(media_plane_->Bridge(), call_media_seat_.get()));
  if (call_media_seat_) {
    call_sessions_->SetTopologySeatPorts(MakeCallTopologySeatPorts(call_media_seat_.get()));
    call_sessions_->SetMediaSeatPorts(call_sessions_->MakeSeatPorts(call_media_seat_.get()));
  }
  if (call_lifecycle_) {
    call_sessions_->SetTopologyHopArmingPorts(MakeCallHopArmingPorts(call_lifecycle_.get()));
    call_sessions_->SetLifecyclePorts(MakeCallSessionLifecyclePorts(call_lifecycle_.get()));
    if (CallMediaBridge* bridge = media_plane_->Bridge()) {
      bridge->SetDirectArmingPorts(MakeCallDirectArmingPorts(call_lifecycle_.get()));
    }
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
    call_sessions_->SetTopologySeatPorts(MakeCallTopologySeatPorts(call_media_seat_.get()));
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
  SyncMediaPlaneDeps();
  if (media_plane_) {
    media_plane_->BindTestMediaPath(transport, dial);
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
  call_sessions_->SetTopologyHopArmingPorts(MakeCallHopArmingPorts(call_lifecycle_.get()));
  call_sessions_->SetLifecyclePorts(MakeCallSessionLifecyclePorts(call_lifecycle_.get()));
  if (media_plane_) {
    if (CallMediaBridge* bridge = media_plane_->Bridge()) {
      bridge->SetDirectArmingPorts(MakeCallDirectArmingPorts(call_lifecycle_.get()));
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

} // namespace pbr
