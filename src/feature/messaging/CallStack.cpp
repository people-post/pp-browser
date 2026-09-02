#include "feature/messaging/CallStack.h"

#include "base/data/MeshRole.h"
#include "base/mesh/host/MeshPorts.h"
#include "base/messaging/CallTypes.h"
#include "base/people/DirectChatTargetFromContact.h"
#include "base/people/ContactTypes.h"
#include "base/people/ContactsStore.h"
#include "base/people/IdentityStore.h"
#include "base/people/MeshHopPolicy.h"
#include "base/mesh/l4/circuit/AmpCircuitHopRegistry.h"
#include "base/mesh/reachability/Reachability.h"
#include "base/runtime/AppRuntime.h"
#include "feature/messaging/MeshMessagingService.h"
#include "feature/messaging/SqlitePskSessionStore.h"

#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

CallStack::CallStack() {
  redirectLogger("CallStack");
}

CallStack::~CallStack() {
  Shutdown();
}

const AppConfig& CallStack::config() const {
  return deps_.config();
}

Roe<void> CallStack::InitializeStores(const std::string& profile_db_path, const std::string& profile_id) {
  call_session_store_ = std::make_unique<CallSessionStore>(profile_db_path);
  call_media_keys_ = std::make_unique<CallMediaKeyStore>(profile_db_path, profile_id);
  call_media_engine_ = std::make_unique<CallMediaEngine>();
  return {};
}

void CallStack::BuildSessions(const CallStackDeps& deps) {
  deps_ = deps;
  call_sessions_ = std::make_unique<CallSessionManager>(*deps_.store, *deps_.contacts, *deps_.identity,
                                                        *call_session_store_, *call_media_keys_, *deps_.mesh_messaging,
                                                        *deps_.psk, *call_media_engine_);
  deps_.mesh_messaging->SetCallSessionManager(call_sessions_.get());
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
    // Warm only; must not run RequestBridge on Browser IO ahead of AcceptInvite.
    AppRuntime::PostWorkerNormal([this, identity]() {
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
  MeshHost* m = mesh();
  if (!m || !m->IsRunning()) {
    return;
  }
  call_media_amp_.reset();

  if (!m->Amp()) {
    log().warning << "call-media transport unavailable (Amp required)";
    WireMediaRelayDeps();
    return;
  }
  auto pump = [m]() { m->Tick(); };
  CallMediaAmpTransport::WorkerPost worker = [](std::function<void()> task) {
    AppRuntime::PostWorkerNormal(std::move(task));
  };
  call_media_amp_ =
      std::make_unique<CallMediaAmpTransport>(m->Amp()->Runtime(), std::move(pump), std::move(worker));
  call_media_amp_->Start();
  log().info << "call-media transport=amp";
  WireMediaRelayDeps();
}

ICallMediaTransport* CallStack::CallMediaTransport() { return call_media_amp_.get(); }

bool CallStack::HasActiveLocalCall() {
  if (call_lifecycle_ && call_lifecycle_->WantEphemeralListen()) {
    return true;
  }
  if (ephemeral_listen_desired_) {
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
  return ephemeral_listen_desired_ || (call_lifecycle_ && call_lifecycle_->WantEphemeralListen());
}

void CallStack::WireMediaRelayDeps() {
  if (!call_sessions_) {
    return;
  }
  MeshHost* m = mesh();
  const bool use_amp_relay =
      m && m->Amp() && m->AmpMediaRelayCoord() && m->AmpMediaRelayCoord()->IsStarted();
  if (use_amp_relay) {
    media_relay_client_ = std::make_unique<AmpMediaRelayClient>(
        *m->AmpMediaRelayCoord(), [m]() { m->Tick(); }, m->Amp()->LocalPeerId());
    log().info << "media-relay transport=amp";
  } else {
    media_relay_client_.reset();
    log().warning << "media-relay transport unavailable (Amp required)";
  }
  // Keep dial registry + media bridge stable across N025 listen sync — recreating them
  // mid-call drops pending answerer state and dangling dial pointers.
  if (!dial_registry_) {
    dial_registry_ = std::make_unique<PeerSessionDialRegistry>();
  }
  dial_registry_->SetAmpLinks(use_amp_relay && m->ChatDeps() ? &m->ChatDeps()->links : nullptr);
  dial_registry_->SetAmpCircuitHops(use_amp_relay && m->AmpCircuitHops() ? m->AmpCircuitHops() : nullptr);
  const bool use_amp_circuit =
      use_amp_relay && m->AmpCircuitTunnel() && m->AmpCircuitTunnel()->IsStarted() && m->AmpCircuitHops();
  if (config().mesh.capabilities.circuit_relay) {
    if (use_amp_circuit) {
      auto circuit = m->CircuitDeps();
      if (!circuit) {
        circuit_hop_reach_.reset();
      } else {
        circuit_hop_reach_ = std::make_unique<AmpCircuitHopReach>(
            circuit->tunnel, circuit->hops, circuit->links, [m]() { m->Tick(); },
            [this](const std::string& exclude) { return CollectDialableCircuitRelayIds(exclude); });
        log().info << "circuit-hop reach=amp";
      }
    } else {
      circuit_hop_reach_.reset();
    }
  } else {
    circuit_hop_reach_.reset();
  }
  CallSessionManager::MediaRelayDeps deps;
  deps.relay = media_relay_client_.get();
  deps.dial = dial_registry_.get();
  deps.circuit_reach = circuit_hop_reach_.get();
  MeshConfig mesh_cfg = config().mesh;
  NormalizeMeshConfig(mesh_cfg);
  deps.bootstrap_peers = mesh_cfg.bootstrap_peers;
  deps.prefer_contacts = mesh_cfg.prefer_contacts_for_routing;
  deps.list_directory_nodes = deps_.list_directory_nodes;
  deps.list_dht_nodes = deps_.list_dht_nodes;
  deps.seed_dial_ok = deps_.seed_dial_ok;
  // PreferLocal = durable Node hosting only. Mobile ephemeral Start() must not SoftMigrate-self
  // into the SFU hop (V028 / dogfood: Android hop crash → peer Connection reset).
  deps.prefer_local_as_hop = ResolveMeshRole(config().mesh) == MeshRole::Node &&
                             mesh_cfg.capabilities.media_relay && use_amp_relay &&
                             m->AmpMediaRelayCoord()->IsStarted();
  if (m && !m->AmpListenMultiaddr().empty()) {
    deps.local_listen_multiaddr = m->AmpListenMultiaddr();
  }
  // PreferLocal CallSfuAttach fan-out needs dialable LAN addrs (same as invite listen_multiaddrs).
  deps.local_advertise_multiaddrs = LocalCallListenMultiaddrs();
  deps.resolve_local_advertise = [this]() { return LocalCallListenMultiaddrs(); };
  deps.peer_has_media_relay = [this](const std::string& peer_id) {
    return call_sessions_ && call_sessions_->PeerHasMediaRelayCap(peer_id);
  };
  deps.list_media_relay_peers = [this]() {
    if (!call_sessions_) {
      return std::vector<std::string>{};
    }
    return call_sessions_->ListMediaRelayCapablePeerIds();
  };
  // Wildcard bind does not identify a LAN subnet for link-scope inference (N023 ns1).
  if (deps.local_listen_multiaddr.find("/ip4/0.0.0.0/") != std::string::npos) {
    deps.local_listen_multiaddr.clear();
  }
  call_sessions_->SetMediaRelayDeps(std::move(deps));

  if (ICallMediaTransport* transport = CallMediaTransport(); transport && dial_registry_) {
    // Rebuild when CallSessionManager was replaced (BuildMessagingStack) — bridge holds a host&
    // into that object. Keep the same bridge across N025 listen sync on the same manager.
    const bool sessions_changed = (media_bridge_bound_sessions_ != call_sessions_.get());
    if (!call_media_bridge_ || sessions_changed) {
      call_media_bridge_ = std::make_unique<CallMediaBridge>(
          call_sessions_->AsMediaHost(), *call_session_store_, *call_media_keys_, *call_media_engine_,
          *transport, dial_registry_.get(), circuit_hop_reach_.get());
      call_sessions_->SetCallMediaBridge(call_media_bridge_.get());
      media_bridge_bound_sessions_ = call_sessions_.get();
      EnsureCallLifecycleBound();
      call_media_bridge_->SetLifecycle(call_lifecycle_.get());
      log().info << "CallMediaBridge bound (sessions_changed=" << (sessions_changed ? 1 : 0)
                 << " transport=amp)";
    } else {
      call_media_bridge_->SetReachDeps(dial_registry_.get(), circuit_hop_reach_.get());
      if (call_lifecycle_) {
        call_media_bridge_->SetLifecycle(call_lifecycle_.get());
      }
    }
  } else {
    call_media_bridge_.reset();
    media_bridge_bound_sessions_ = nullptr;
    call_sessions_->SetCallMediaBridge(nullptr);
  }
}

void CallStack::PrepareForMeshStop(const std::function<void()>& abort_inflight_circuit) {
  ephemeral_listen_desired_ = false;
  if (call_lifecycle_) {
    call_lifecycle_->ClearBinding();
  }
  if (call_sessions_) {
    call_sessions_->SetCallMediaBridge(nullptr);
    call_sessions_->SetMediaRelayDeps({});
  }
  // Abort circuit waiters before joining/destroying Connect workers (same as AbortCallMediaForShutdown).
  if (abort_inflight_circuit) {
    abort_inflight_circuit();
  }
  // Connect worker holds `this` on the bridge — abort + wait before delete (shutdown segfault).
  // Detach completes in-flight Connect() immediately; dial/reachability loops check generation.
  if (call_media_bridge_) {
    call_media_bridge_->PrepareForTeardown(2000);
  }
  if (abort_inflight_circuit) {
    abort_inflight_circuit();
  }
  if (ICallMediaTransport* transport = CallMediaTransport()) {
    transport->ClearInboundHandler();
    transport->Stop();
  }
  media_relay_client_.reset();
}

void CallStack::FinishMeshStop() {
  call_media_bridge_.reset();
  media_bridge_bound_sessions_ = nullptr;
  call_media_amp_.reset();
  dial_registry_.reset();
}

void CallStack::AbortCallMediaForShutdown() {
  // Unblock Connect workers stuck in circuit RequestBridge (~8–10s) BEFORE waiting/joining.
  if (MeshHost* m = mesh()) {
    m->AbortInflightCircuitRequests();
  }
  // Group SFU: close media_relay before LeaveCall joins capture (BlockingWrite hang on quit).
  if (media_relay_client_) {
    media_relay_client_->Detach();
  }
  // Tell the peer the call ended (fire-and-forget relay Critical send) so they StopMedia /
  // leave Connecting instead of sitting on a half-open stream after we detach.
  if (call_sessions_) {
    if (auto active = call_sessions_->ActiveLocalCall(); active && active->has_value()) {
      log().info << "AbortCallMediaForShutdown LeaveCall call_id=" << (*active)->call_id;
      (void)call_sessions_->LeaveCall((*active)->call_id);
    }
  }
  if (call_media_bridge_) {
    // LeaveCall already bumps connect_generation_; wait for the worker to observe abort.
    call_media_bridge_->PrepareForTeardown(2000);
  }
  if (ICallMediaTransport* transport = CallMediaTransport()) {
    transport->Detach();
  }
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

  const std::string peer_id = m->Amp()->LocalPeerId();
  if (peer_id.empty()) {
    return {};
  }

  std::vector<std::string> addrs;
  auto amp_lan = BuildAmpLanAdvertisedAddrs(m->AmpListenMultiaddr(), peer_id);
  if (!amp_lan.empty()) {
    for (std::string& ma : amp_lan) {
      addrs.push_back(std::move(ma));
    }
  } else {
    addrs.push_back(m->AmpListenMultiaddr());
  }
  return addrs;
}

void CallStack::RegisterCallPeerListenMultiaddrs(const std::string& identity,
                                                 const std::vector<std::string>& multiaddrs) {
  if (identity.empty() || multiaddrs.empty()) {
    return;
  }
  for (const std::string& ma : multiaddrs) {
    if (ma.empty()) {
      continue;
    }
    const std::string ip = IpHostFromMultiaddrPrefix(ma);
    if (IsLikelyUndialableLanIpv4(ip)) {
      log().info << "Call listen addr skipped undialable dial_key=" << identity << " ma=" << ma;
      continue;
    }
    std::string peer_id;
    const auto p2p_pos = ma.rfind("/p2p/");
    if (p2p_pos != std::string::npos) {
      peer_id = ma.substr(p2p_pos + 5);
      const auto slash = peer_id.find('/');
      if (slash != std::string::npos) {
        peer_id.resize(slash);
      }
    }
    if (dial_registry_) {
      (void)dial_registry_->RegisterEndpoint(identity, ma);
      dial_registry_->ClearDialBackoff(identity);
      if (!peer_id.empty()) {
        (void)dial_registry_->RegisterEndpoint(peer_id, ma);
        dial_registry_->ClearDialBackoff(peer_id);
        if (deps_.note_lan_mdns_peer_id) {
          deps_.note_lan_mdns_peer_id(peer_id);
        }
      }
    }
    if (deps_.mesh_messaging) {
      deps_.mesh_messaging->RegisterPeerDirectEndpoint(identity, ma);
      if (!peer_id.empty() && peer_id != identity) {
        deps_.mesh_messaging->RegisterPeerDirectEndpoint(peer_id, ma);
      }
    }
    if (!peer_id.empty() && identity.rfind("account:", 0) == 0 && call_sessions_) {
      call_sessions_->NoteMeshPeerIdForRelay(identity, peer_id);
    }
    log().info << "Call listen addr registered dial_key=" << identity << " ma=" << ma;
  }
}

std::vector<std::string> CallStack::CollectDialableCircuitRelayIds(const std::string& exclude_peer_id) const {
  std::vector<std::string> relay_ids;
  MeshHost* m = mesh();
  IChatPeerLinks* amp_links = nullptr;
  if (m) {
    if (auto chat = m->ChatDeps()) {
      amp_links = &chat->links;
    }
  }
  AmpCircuitHopRegistry* amp_hops = m ? m->AmpCircuitHops() : nullptr;
  if (!amp_links && !amp_hops) {
    return relay_ids;
  }
  std::vector<Contact> contacts;
  if (deps_.contacts) {
    if (auto listed = deps_.contacts->List()) {
      contacts = std::move(*listed);
    }
  }
  MeshConfig mesh_cfg = config().mesh;
  NormalizeMeshConfig(mesh_cfg);
  std::vector<MeshDirectoryNode> directory_nodes;
  if (deps_.list_directory_nodes) {
    directory_nodes = deps_.list_directory_nodes();
  }
  std::vector<MeshDirectoryNode> dht_nodes;
  if (deps_.list_dht_nodes) {
    dht_nodes = deps_.list_dht_nodes();
  }
  const bool include_seeds = !deps_.seed_dial_ok || deps_.seed_dial_ok();
  auto hops = BuildCircuitHopList(contacts, directory_nodes, dht_nodes, mesh_cfg.bootstrap_peers,
                                  mesh_cfg.prefer_contacts_for_routing, include_seeds);
  relay_ids.reserve(hops.size());
  for (const MeshHopCandidate& hop : hops) {
    if (hop.peer_id.empty() || hop.peer_id == exclude_peer_id) {
      continue;
    }
    if (!hop.multiaddr.empty() && amp_links && IsAdpMultiaddr(hop.multiaddr)) {
      (void)amp_links->RegisterEndpoint(hop.peer_id, hop.multiaddr);
    } else if (hop.multiaddr.empty() && amp_links) {
      if (auto ma = amp_links->PreferredMultiaddr(hop.peer_id)) {
        (void)amp_links->RegisterEndpoint(hop.peer_id, *ma);
      }
    }
    const bool amp_ok = amp_links && amp_links->GetLinkSnapshot(hop.peer_id).has_endpoint;
    const bool hop_ok = amp_hops && amp_hops->HasAny(hop.peer_id);
    if (amp_ok || hop_ok) {
      relay_ids.push_back(hop.peer_id);
    }
  }
  return relay_ids;
}

Roe<void> CallStack::TryEnsureCircuitHopReachable(const std::string& hop_peer_id) {
  if (!circuit_hop_reach_) {
    return Error("Amp circuit reach required");
  }
  if (!config().mesh.capabilities.circuit_relay) {
    return Error("circuit-relay disabled");
  }
  return circuit_hop_reach_->TryEnsureHopReachable(hop_peer_id);
}

Roe<void> CallStack::TryEnsureCallMediaReachable(const std::string& peer_key) {
  if (!circuit_hop_reach_) {
    return Error("Amp required");
  }
  if (!config().mesh.capabilities.circuit_relay) {
    return Error("circuit-relay disabled");
  }
  if (peer_key.empty()) {
    return Error("missing call peer");
  }
  return circuit_hop_reach_->TryEnsureCallMediaReachable(peer_key);
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
  call_lifecycle_->Bind(call_sessions_.get());
  call_lifecycle_->SetOnListenDesireChanged([this](bool want) { SetEphemeralListenDesire(want); });
  if (call_media_bridge_) {
    call_media_bridge_->SetLifecycle(call_lifecycle_.get());
  }
}

void CallStack::SetEphemeralListenDesire(bool want) {
  ephemeral_listen_desired_ = want;
  if (deps_.sync_mobile_ephemeral_listen) {
    deps_.sync_mobile_ephemeral_listen();
  }
}

void CallStack::ResetRelayClients() {
  media_relay_client_.reset();
  dial_registry_.reset();
}

void CallStack::ResetSessions() {
  call_sessions_.reset();
}

void CallStack::Shutdown() {
  if (call_sessions_) {
    call_sessions_->ClearMediaCallbacks();
  }
  if (call_lifecycle_) {
    call_lifecycle_->ClearBinding();
  }
  call_media_bridge_.reset();
  media_bridge_bound_sessions_ = nullptr;
  call_media_amp_.reset();
  media_relay_client_.reset();
  dial_registry_.reset();
  circuit_hop_reach_.reset();
  call_lifecycle_.reset();
  call_sessions_.reset();
  call_media_engine_.reset();
  call_media_keys_.reset();
  call_session_store_.reset();
}

} // namespace pbr
