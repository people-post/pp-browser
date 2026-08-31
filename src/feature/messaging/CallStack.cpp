#include "feature/messaging/CallStack.h"

#include "base/data/Libp2pRole.h"
#include "base/messaging/CallTypes.h"
#include "base/messaging/DirectChatTarget.h"
#include "base/people/ContactTypes.h"
#include "base/people/ContactsStore.h"
#include "base/people/IdentityStore.h"
#include "base/people/MeshHopPolicy.h"
#include "base/runtime/AppRuntime.h"
#include "feature/messaging/P2pMessagingService.h"
#include "feature/messaging/SqlitePskSessionStore.h"
#include "base/p2p/Reachability.h"

#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

CallStack::CallStack() {
  redirectLogger("CallStack");
}

CallStack::~CallStack() {
  Shutdown();
}

NodeRuntime* CallStack::Runtime() const {
  MeshHost* m = mesh();
  return m ? m->Runtime() : nullptr;
}

PeerSessionManager* CallStack::Sessions() const {
  NodeRuntime* rt = Runtime();
  return rt ? rt->Sessions() : nullptr;
}

MediaRelayService* CallStack::MediaRelay() const {
  MeshHost* m = mesh();
  return m ? m->MediaRelay() : nullptr;
}

CircuitRelayService* CallStack::CircuitRelay() const {
  MeshHost* m = mesh();
  return m ? m->CircuitRelay() : nullptr;
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
                                                        *call_session_store_, *call_media_keys_, *deps_.p2p,
                                                        *deps_.psk, *call_media_engine_);
  deps_.p2p->SetCallSessionManager(call_sessions_.get());
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
  call_sessions_->SetLocalLibp2pPeerIdProvider([this]() -> std::string {
    if (!Runtime() || !Runtime()->Host()) {
      return {};
    }
    if (auto local = Runtime()->Host()->LocalPeerIdBase58()) {
      return *local;
    }
    return {};
  });
  call_sessions_->SetLocalPeerCapsProvider([this]() {
    CallPeerCaps caps;
    caps.v = kCallPeerCapsVersion;
    caps.present = true;
    // Durable Node host only — never advertise media_relay for ephemeral listen-only (V030).
    caps.media_relay = ResolveLibp2pRole(config().libp2p) == Libp2pRole::Node &&
                       config().libp2p.capabilities.media_relay &&
                       ((mesh() && mesh()->Amp() && mesh()->AmpMediaRelayCoord() &&
                         mesh()->AmpMediaRelayCoord()->IsStarted()) ||
                        (MediaRelay() && MediaRelay()->IsStarted()));
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
  if (!Runtime() || !Runtime()->IsRunning()) {
    return;
  }
  call_media_direct_.reset();
  call_media_amp_.reset();

  MeshHost* m = mesh();
  if (m && m->Amp()) {
    auto pump = [m]() { m->Tick(); };
    CallMediaAmpTransport::WorkerPost worker;
    if (Libp2pHost* host = Runtime()->Host()) {
      worker = [host](std::function<void()> task) {
        PostLibp2pWorker(*host, WorkerLane::Normal, std::move(task));
      };
    }
    call_media_amp_ = std::make_unique<CallMediaAmpTransport>(m->Amp()->Runtime(), std::move(pump),
                                                              std::move(worker));
    call_media_amp_->Start();
    log().info << "call-media transport=amp";
  } else if (Runtime()->Host() && Runtime()->Sessions()) {
    call_media_direct_ = std::make_unique<CallMediaDirectService>(*Runtime()->Host(), *Runtime()->Sessions());
    call_media_direct_->Start();
    log().info << "call-media transport=libp2p";
  }
  WireMediaRelayDeps();
}

ICallMediaTransport* CallStack::CallMediaTransport() {
  if (call_media_amp_) {
    return call_media_amp_.get();
  }
  return call_media_direct_.get();
}

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
    media_relay_client_ = std::make_unique<MediaRelayServiceClient>(MediaRelay());
    log().info << "media-relay transport=libp2p";
  }
  PeerSessionManager* sessions = Runtime() ? Runtime()->Sessions() : nullptr;
  // Keep dial registry + libp2p media bridge stable across N025 listen sync — recreating them
  // mid-call drops pending answerer state and dangling dial pointers.
  if (!dial_registry_) {
    dial_registry_ = std::make_unique<PeerSessionDialRegistry>(sessions);
  } else {
    dial_registry_->SetSessions(sessions);
  }
  dial_registry_->SetAmpLinks(use_amp_relay && m->Amp() ? &m->Amp()->Links() : nullptr);
  if (sessions && config().libp2p.capabilities.circuit_relay) {
    if (!circuit_hop_reach_) {
      circuit_hop_reach_ = std::make_unique<CircuitHopReachClient>(
          [this](const std::string& hop_peer_id) { return TryEnsureCircuitHopReachable(hop_peer_id); },
          [this](const std::string& peer_key) { return TryEnsureCallMediaReachable(peer_key); });
    }
  } else {
    circuit_hop_reach_.reset();
  }
  CallSessionManager::MediaRelayDeps deps;
  deps.relay = media_relay_client_.get();
  deps.dial = dial_registry_.get();
  deps.circuit_reach = circuit_hop_reach_.get();
  Libp2pConfig libp2p = config().libp2p;
  NormalizeLibp2pConfig(libp2p);
  deps.bootstrap_peers = libp2p.bootstrap_peers;
  deps.prefer_contacts = libp2p.prefer_contacts_for_routing;
  // PreferLocal = durable Node hosting only. Mobile ephemeral Start() must not SoftMigrate-self
  // into the SFU hop (V028 / dogfood: Android hop crash → peer Connection reset).
  deps.prefer_local_as_hop = ResolveLibp2pRole(config().libp2p) == Libp2pRole::Node &&
                             libp2p.capabilities.media_relay &&
                             ((use_amp_relay && m->AmpMediaRelayCoord()->IsStarted()) ||
                              (!use_amp_relay && MediaRelay() && MediaRelay()->IsStarted()));
  if (Runtime() && !Runtime()->BoundListenMultiaddr().empty()) {
    deps.local_listen_multiaddr = Runtime()->BoundListenMultiaddr();
  } else {
    deps.local_listen_multiaddr = libp2p.listen_multiaddr;
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
    const bool sessions_changed = (libp2p_bridge_bound_sessions_ != call_sessions_.get());
    if (!call_libp2p_bridge_ || sessions_changed) {
      call_libp2p_bridge_ = std::make_unique<CallLibp2pMediaBridge>(
          call_sessions_->AsMediaHost(), *call_session_store_, *call_media_keys_, *call_media_engine_,
          *transport, dial_registry_.get(), circuit_hop_reach_.get());
      call_sessions_->SetLibp2pMediaBridge(call_libp2p_bridge_.get());
      libp2p_bridge_bound_sessions_ = call_sessions_.get();
      EnsureCallLifecycleBound();
      call_libp2p_bridge_->SetLifecycle(call_lifecycle_.get());
      log().info << "CallLibp2pMediaBridge bound (sessions_changed=" << (sessions_changed ? 1 : 0)
                 << " transport=" << (call_media_amp_ ? "amp" : "libp2p") << ")";
    } else {
      call_libp2p_bridge_->SetReachDeps(dial_registry_.get(), circuit_hop_reach_.get());
      if (call_lifecycle_) {
        call_libp2p_bridge_->SetLifecycle(call_lifecycle_.get());
      }
    }
  } else {
    call_libp2p_bridge_.reset();
    libp2p_bridge_bound_sessions_ = nullptr;
    call_sessions_->SetLibp2pMediaBridge(nullptr);
  }
}

void CallStack::PrepareForMeshStop(const std::function<void()>& abort_inflight_circuit) {
  ephemeral_listen_desired_ = false;
  if (call_lifecycle_) {
    call_lifecycle_->ClearBinding();
  }
  if (call_sessions_) {
    call_sessions_->SetLibp2pMediaBridge(nullptr);
    call_sessions_->SetMediaRelayDeps({});
  }
  // Abort circuit waiters before joining/destroying Connect workers (same as AbortCallMediaForShutdown).
  if (abort_inflight_circuit) {
    abort_inflight_circuit();
  }
  // Connect worker holds `this` on the bridge — abort + wait before delete (shutdown segfault).
  // Detach completes in-flight Connect() immediately; dial/reachability loops check generation.
  if (call_libp2p_bridge_) {
    call_libp2p_bridge_->PrepareForTeardown(2000);
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
  // Keep bridge + dial registry alive until the libp2p host joins its workers — inbound
  // CallMediaKey wait and OpenStream completions may still touch them.
  call_libp2p_bridge_.reset();
  libp2p_bridge_bound_sessions_ = nullptr;
  call_media_direct_.reset();
  call_media_amp_.reset();
  dial_registry_.reset();
}

void CallStack::AbortCallMediaForShutdown() {
  // Unblock Connect workers stuck in circuit RequestBridge (~8–10s) BEFORE waiting/joining.
  // Old order waited 250ms then aborted circuit — WorkerPool::Shutdown joined a still-blocked
  // Critical task and both peers looked hung until force-quit.
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
  if (call_libp2p_bridge_) {
    // LeaveCall already bumps connect_generation_; wait for the worker to observe abort.
    call_libp2p_bridge_->PrepareForTeardown(2000);
  }
  if (ICallMediaTransport* transport = CallMediaTransport()) {
    transport->Detach();
  }
}

std::vector<std::string> CallStack::LocalCallListenMultiaddrs() const {
  if (!Runtime() || !Runtime()->Host() || !Runtime()->IsRunning()) {
    return {};
  }
  const bool listening =
      Runtime()->EphemeralListenActive() || ResolveLibp2pRole(config().libp2p) == Libp2pRole::Node;
  if (!listening) {
    return {};
  }
  std::string peer_id;
  if (auto local = Runtime()->Host()->LocalPeerIdBase58()) {
    peer_id = *local;
  }
  const std::string bound = Runtime()->BoundListenMultiaddr();
  if (peer_id.empty() || bound.empty()) {
    return {};
  }
  std::vector<std::string> addrs = BuildMobileCallScopedAdvertisedAddrs(bound, peer_id);
  if (MeshHost* m = mesh(); m && !m->AmpListenMultiaddr().empty()) {
    addrs.push_back(m->AmpListenMultiaddr());
  }
  return addrs;
}

void CallStack::RegisterCallPeerListenMultiaddrs(const std::string& identity,
                                                 const std::vector<std::string>& multiaddrs) {
  if (identity.empty() || multiaddrs.empty()) {
    return;
  }
  PeerSessionManager* sessions = Sessions();
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
    if (sessions) {
      (void)sessions->RegisterEndpoint(identity, ma);
      sessions->ClearDialBackoff(identity);
      if (!peer_id.empty()) {
        (void)sessions->UpsertBookEntry(peer_id, ma, PeerAddrSource::Manual);
        (void)sessions->RegisterEndpoint(peer_id, ma);
        sessions->ClearDialBackoff(peer_id);
        if (peer_id != identity) {
          (void)sessions->RegisterEndpoint(identity, ma);
        }
        if (deps_.note_lan_mdns_peer_id) {
          deps_.note_lan_mdns_peer_id(peer_id);
        }
      }
    }
    if (deps_.p2p) {
      deps_.p2p->RegisterPeerDirectEndpoint(identity, ma);
      if (!peer_id.empty() && peer_id != identity) {
        deps_.p2p->RegisterPeerDirectEndpoint(peer_id, ma);
      }
    }
    if (!peer_id.empty() && identity.rfind("account:", 0) == 0 && call_sessions_) {
      call_sessions_->NoteLibp2pPeerIdForRelay(identity, peer_id);
    }
    log().info << "Call listen addr registered dial_key=" << identity << " ma=" << ma;
  }
}

std::vector<std::string> CallStack::CollectDialableCircuitRelayIds(const std::string& exclude_peer_id) const {
  std::vector<std::string> relay_ids;
  if (!Runtime() || !Runtime()->Sessions()) {
    return relay_ids;
  }
  PeerSessionManager& sessions = *Runtime()->Sessions();
  std::vector<Contact> contacts;
  if (deps_.contacts) {
    if (auto listed = deps_.contacts->List()) {
      contacts = std::move(*listed);
    }
  }
  Libp2pConfig libp2p = config().libp2p;
  NormalizeLibp2pConfig(libp2p);
  auto hops = OrderCircuitHops(CollectContactHopCandidates(contacts),
                               CollectSeedHopCandidates(libp2p.bootstrap_peers),
                               libp2p.prefer_contacts_for_routing);
  relay_ids.reserve(hops.size());
  for (const MeshHopCandidate& hop : hops) {
    if (hop.peer_id.empty() || hop.peer_id == exclude_peer_id) {
      continue;
    }
    if (hop.multiaddr.empty()) {
      if (auto ma = sessions.PreferredPeerMultiaddr(hop.peer_id)) {
        (void)sessions.RegisterEndpoint(hop.peer_id, *ma);
      }
    } else {
      (void)sessions.RegisterEndpoint(hop.peer_id, hop.multiaddr);
    }
    sessions.ClearDialBackoff(hop.peer_id);
    if (sessions.IsDialable(hop.peer_id)) {
      relay_ids.push_back(hop.peer_id);
    }
  }
  return relay_ids;
}

Roe<void> CallStack::TryEnsureCircuitHopReachable(const std::string& hop_peer_id) {
  if (!CircuitRelay() || !Runtime() || !Runtime()->Sessions()) {
    return Error("circuit-relay not available");
  }
  if (!config().libp2p.capabilities.circuit_relay) {
    return Error("circuit-relay disabled");
  }
  PeerSessionManager& sessions = *Runtime()->Sessions();
  const std::vector<std::string> relay_ids = CollectDialableCircuitRelayIds(hop_peer_id);
  if (relay_ids.empty()) {
    return Error("no dialable circuit relays");
  }
  return sessions.TryEnsureHopViaCircuit(hop_peer_id, *CircuitRelay(), relay_ids, kMediaRelayProtocolId, 8000);
}

Roe<void> CallStack::TryEnsureCallMediaReachable(const std::string& peer_key) {
  if (!CircuitRelay() || !Runtime() || !Runtime()->Sessions()) {
    return Error("circuit-relay not available");
  }
  if (!config().libp2p.capabilities.circuit_relay) {
    return Error("circuit-relay disabled");
  }
  if (peer_key.empty()) {
    return Error("missing call peer");
  }

  PeerSessionManager& sessions = *Runtime()->Sessions();
  std::string target = peer_key;
  if (deps_.contacts) {
    if (auto hit = deps_.contacts->FindByIdentity(peer_key, ContactIdKind::Account)) {
      if (hit->has_value()) {
        const std::string peer_id = PeerIdFromContact(**hit);
        if (!peer_id.empty()) {
          target = peer_id;
        }
      }
    }
    if (target == peer_key) {
      if (auto hit = deps_.contacts->FindByIdentity(peer_key, ContactIdKind::RelayUser)) {
        if (hit->has_value()) {
          const std::string peer_id = PeerIdFromContact(**hit);
          if (!peer_id.empty()) {
            target = peer_id;
          }
        }
      }
    }
    if (target == peer_key) {
      if (auto hit = deps_.contacts->FindByIdentity(peer_key, ContactIdKind::PeerId)) {
        if (hit->has_value()) {
          const std::string peer_id = PeerIdFromContact(**hit);
          if (!peer_id.empty()) {
            target = peer_id;
          }
        }
      }
    }
  }

  if (sessions.IsDialable(peer_key) || sessions.IsDialable(target)) {
    if (target != peer_key) {
      if (auto ma = sessions.PreferredPeerMultiaddr(target)) {
        (void)sessions.RegisterEndpoint(peer_key, *ma);
      }
    }
    return {};
  }

  const std::vector<std::string> relay_ids = CollectDialableCircuitRelayIds(target);
  if (relay_ids.empty()) {
    return Error("no dialable circuit relays");
  }
  auto reached =
      sessions.TryEnsureHopViaCircuit(target, *CircuitRelay(), relay_ids, kCallMediaDirectProtocolId, 8000);
  if (reached && target != peer_key) {
    if (auto ma = sessions.PreferredPeerMultiaddr(target)) {
      (void)sessions.RegisterEndpoint(peer_key, *ma);
    }
  }
  return reached;
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
  if (call_libp2p_bridge_) {
    call_libp2p_bridge_->SetLifecycle(call_lifecycle_.get());
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
  call_libp2p_bridge_.reset();
  libp2p_bridge_bound_sessions_ = nullptr;
  call_media_direct_.reset();
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
