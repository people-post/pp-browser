#include "feature/calls/CallMediaPlane.h"

#include "foundation/data/MeshRole.h"
#include "domain/mesh/host/MeshPorts.h"
#include "domain/messaging/CallTypes.h"
#include "domain/people/DirectChatTargetFromContact.h"
#include "domain/people/ContactTypes.h"
#include "domain/people/ContactsStore.h"
#include "domain/people/MeshHopPolicy.h"
#include "domain/mesh/l4/circuit/AmpCircuitHopRegistry.h"
#include "domain/mesh/l4/circuit/CircuitTunnelCoordinator.h"
#include "domain/mesh/l4/circuit/CircuitRendezvousPolicy.h"
#include "domain/mesh/reachability/PunchLogic.h"
#include "domain/mesh/reachability/AmpPunchCoordinator.h"
#include "domain/mesh/reachability/Reachability.h"
#include "foundation/runtime/AppRuntime.h"
#include "domain/mesh/host/MeshControlDispatch.h"
#include "domain/mesh/shared/AmpParkUntil.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

void CompletePunch(std::function<void(Roe<void>)> on_done, AmpPunchCoordinator::PunchRoe punched,
                   const char* fail_fallback) {
  if (!on_done) {
    return;
  }
  if (!punched) {
    on_done(Error(punched.error().message));
    return;
  }
  if (!punched->ok) {
    on_done(Error(punched->error.empty() ? fail_fallback : punched->error));
    return;
  }
  on_done(Roe<void>());
}

} // namespace

CallMediaPlane::CallMediaPlane() {
  redirectLogger("CallMediaPlane");
}

CallMediaPlane::~CallMediaPlane() {
  InvalidateAsyncOps();
  Clear();
}

void CallMediaPlane::InvalidateAsyncOps() {
  deferred_.Invalidate();
}

const AppConfig& CallMediaPlane::config() const {
  return deps_.config();
}

void CallMediaPlane::SetDeps(CallMediaPlaneDeps deps) {
  deps_ = std::move(deps);
}

void CallMediaPlane::OnMeshStarted() {
  MeshHost* m = mesh();
  if (!m || !m->IsRunning()) {
    return;
  }
  call_media_amp_.reset();

  if (!m->Amp()) {
    log().warning << "call-media transport unavailable (Amp required)";
    Wire();
    return;
  }
  auto pump = [m]() { m->Tick(); };
  CallMediaAmpTransport::WorkerPost worker = [](std::function<void()> task) {
    MeshControlDispatch::Post(std::move(task));
  };
  call_media_amp_ =
      std::make_unique<CallMediaAmpTransport>(m->Amp()->Runtime(), std::move(pump), std::move(worker));
  call_media_amp_->Start();
  log().info << "call-media transport=amp";
  Wire();
}

ICallMediaTransport* CallMediaPlane::Transport() {
  if (test_media_transport_) {
    return test_media_transport_;
  }
  return call_media_amp_.get();
}

IDialRegistry* CallMediaPlane::ActiveDial() const {
  return test_dial_ ? test_dial_ : dial_registry_.get();
}

ICircuitHopReach* CallMediaPlane::ActiveCircuitReach() const {
  return test_circuit_reach_ ? test_circuit_reach_ : circuit_hop_reach_.get();
}

void CallMediaPlane::BindTestMediaPath(ICallMediaTransport* transport, IDialRegistry* dial) {
  BindTestMediaPath(transport, dial, nullptr);
}

void CallMediaPlane::BindTestMediaPath(ICallMediaTransport* transport, IDialRegistry* dial,
                                       ICircuitHopReach* circuit_reach) {
  test_media_transport_ = transport;
  test_dial_ = dial;
  test_circuit_reach_ = circuit_reach;
  Wire();
}

void CallMediaPlane::Wire() {
  MeshHost* m = mesh();
  IoPump io_pump;
  IoPost post_io;
  IoAfter post_after;
  if (auto chat = m ? m->ChatDeps() : std::nullopt) {
    post_io = chat->io.post_io;
    post_after = chat->io.post_after;
    // Exclusive Amp Drive: MakeL4IoPump is always empty. MeshPump (or harness Tick loop)
    // progresses Amp; sync waiters sleep. IoPump is not used to Tick from L4.
    io_pump = chat->io.io_pump;
  }
  const bool use_amp_relay = WireMediaRelayClient(m, io_pump, post_io, post_after);
  WireDialRegistry(m, use_amp_relay, post_io);
  WireCircuitHopReach(m, use_amp_relay, io_pump, post_io, post_after);
}

CallTopologyController::MediaRelayDeps CallMediaPlane::BuildMediaRelayDeps() const {
  MeshHost* m = mesh();
  const bool use_amp_relay =
      m && m->Amp() && m->AmpMediaRelayCoord() && m->AmpMediaRelayCoord()->IsStarted();
  CallTopologyController::MediaRelayDeps deps;
  deps.relay = media_relay_client_.get();
  deps.dial = ActiveDial();
  deps.circuit_reach = ActiveCircuitReach();
  MeshConfig mesh_cfg = config().mesh;
  NormalizeMeshConfig(mesh_cfg);
  deps.bootstrap_peers = mesh_cfg.bootstrap_peers;
  deps.prefer_contacts = mesh_cfg.prefer_contacts_for_routing;
  deps.list_directory_nodes = deps_.list_directory_nodes;
  deps.list_dht_nodes = deps_.list_dht_nodes;
  deps.seed_dial_ok = deps_.seed_dial_ok;
  deps.prefer_local_as_hop = ResolveMeshRole(config().mesh) == MeshRole::Node &&
                             mesh_cfg.capabilities.media_relay && use_amp_relay && m &&
                             m->AmpMediaRelayCoord() && m->AmpMediaRelayCoord()->IsStarted();
  const std::vector<std::string> advertised =
      deps_.local_listen_multiaddrs ? deps_.local_listen_multiaddrs() : std::vector<std::string>{};
  if (!advertised.empty()) {
    deps.local_listen_multiaddr = advertised.front();
  } else if (m && !m->AmpListenMultiaddr().empty()) {
    deps.local_listen_multiaddr = m->AmpListenMultiaddr();
  }
  deps.local_advertise_multiaddrs = advertised;
  deps.resolve_local_advertise = deps_.local_listen_multiaddrs;
  deps.peer_has_media_relay = deps_.peer_has_media_relay;
  deps.list_media_relay_peers = deps_.list_media_relay_peers;
  deps.resolve_remote_listen_by_peer = [this]() { return dial_book_.peer_listen_mas; };
  deps.peer_lan_confirmed = [this](const std::string& peer_id) { return PeerLanConfirmed(peer_id); };
  if (deps.local_listen_multiaddr.find("/ip4/0.0.0.0/") != std::string::npos ||
      deps.local_listen_multiaddr.find("/ip6/::/") != std::string::npos) {
    deps.local_listen_multiaddr.clear();
  }
  return deps;
}

void CallMediaPlane::BindBridge(const CallMediaBridgeBindArgs& args) {
  ICallMediaTransport* transport = Transport();
  IDialRegistry* dial = ActiveDial();
  if (!transport || !dial || !args.host || !args.session_store || !args.media_keys ||
      !args.media_engine) {
    call_media_bridge_.reset();
    media_bridge_bound_sessions_key_ = nullptr;
    return;
  }
  const bool sessions_changed = (media_bridge_bound_sessions_key_ != args.sessions_key);
  if (!call_media_bridge_ || sessions_changed) {
    call_media_bridge_ = std::make_unique<CallMediaBridge>(
        *args.host, *args.session_store, *args.media_keys, *args.media_engine, *transport, dial,
        ActiveCircuitReach());
    media_bridge_bound_sessions_key_ = args.sessions_key;
    log().info << "CallMediaBridge bound (sessions_changed=" << (sessions_changed ? 1 : 0)
               << " transport=" << (test_media_transport_ ? "test" : "amp") << ")";
  } else {
    call_media_bridge_->SetReachDeps(dial, ActiveCircuitReach());
  }
  call_media_bridge_->SetSeedWarm([this]() { WarmBootstrapSeedSessions(); });
  call_media_bridge_->SetSeedReserve([this]() { ReserveOnBootstrapSeeds(); });
  call_media_bridge_->SetSeedParkAwait(
      [this](std::function<void(bool)> done, int timeout_ms) {
        EnsureBootstrapSeedParkedAsync(std::move(done), timeout_ms);
      });
}

bool CallMediaPlane::WireMediaRelayClient(MeshHost* m, const IoPump& io_pump, const IoPost& post_io,
                                          const IoAfter& post_after) {
  const bool use_amp_relay =
      m && m->Amp() && m->AmpMediaRelayCoord() && m->AmpMediaRelayCoord()->IsStarted();
  if (use_amp_relay) {
    media_relay_client_ = std::make_unique<AmpMediaRelayClient>(
        *m->AmpMediaRelayCoord(), io_pump, m->Amp()->LocalPeerId(), post_io, post_after);
    log().info << "media-relay transport=amp";
  } else {
    media_relay_client_.reset();
    log().warning << "media-relay transport unavailable (Amp required)";
  }
  return use_amp_relay;
}

void CallMediaPlane::WireDialRegistry(MeshHost* m, bool use_amp_relay, const IoPost& post_io) {
  // Keep dial registry stable across N025 listen sync — recreating mid-call drops answerer state.
  if (!dial_registry_) {
    dial_registry_ = std::make_unique<PeerSessionDialRegistry>();
  }
  dial_registry_->SetAmpLinks(use_amp_relay && m && m->ChatDeps() ? &m->ChatDeps()->links : nullptr);
  dial_registry_->SetAmpCircuitHops(use_amp_relay && m && m->AmpCircuitHops() ? m->AmpCircuitHops()
                                                                              : nullptr);
  if (auto chat = m ? m->ChatDeps() : std::nullopt) {
    dial_registry_->SetPostIo(chat->io.post_io);
  } else {
    dial_registry_->SetPostIo(post_io);
  }
}

void CallMediaPlane::WireCircuitHopReach(MeshHost* m, bool use_amp_relay, const IoPump& io_pump,
                                         const IoPost& post_io, const IoAfter& post_after) {
  const bool use_amp_circuit = use_amp_relay && m && m->AmpCircuitTunnel() &&
                               m->AmpCircuitTunnel()->IsStarted() && m->AmpCircuitHops();
  if (!use_amp_circuit) {
    circuit_hop_reach_.reset();
    return;
  }
  auto circuit = m->CircuitDeps();
  if (!circuit) {
    circuit_hop_reach_.reset();
    return;
  }
  IChatPeerLinks* punch_links = &circuit->links;
  auto reach = std::make_unique<AmpCircuitHopReach>(
      circuit->tunnel, circuit->hops, circuit->links, io_pump,
      [this](const std::string& exclude) { return CollectDialableCircuitRelayIds(exclude); },
      [this, m, punch_links](const std::string& target_peer_id,
                            std::function<void(Roe<void>)> on_done) {
        TryColdPunchAsync(m, punch_links, target_peer_id, std::move(on_done));
      },
      [this, m](const std::string& introducer_peer_key, const std::string& target_peer_id,
                std::function<void(Roe<void>)> on_done) {
        TryUpgradePunchAsync(m, introducer_peer_key, target_peer_id, std::move(on_done));
      },
      post_io, post_after);
  reach->SetOnRelayChosen(deferred_.Bind([this](const std::string& relay_peer_key) {
    if (relay_peer_key.empty()) {
      return;
    }
    chosen_circuit_r1_ = relay_peer_key;
    log().info << "circuit rendezvous chosen R1=" << relay_peer_key;
    if (deps_.announce_circuit_r1) {
      deps_.announce_circuit_r1(relay_peer_key);
    }
  }));
  circuit_hop_reach_ = std::move(reach);
  log().info << "circuit-hop reach=amp";
}

void CallMediaPlane::TryColdPunchAsync(MeshHost* m, IChatPeerLinks* punch_links,
                                       const std::string& target_peer_id,
                                       std::function<void(Roe<void>)> on_done) {
  if (!on_done) {
    return;
  }
  if (!m || !punch_links) {
    on_done(Error("amp punch unavailable"));
    return;
  }
  auto* punch = m->AmpPunch();
  if (!punch || !punch->IsStarted()) {
    on_done(Error("amp punch unavailable"));
    return;
  }
  std::vector<std::string> contact_ids;
  if (deps_.contacts) {
    if (auto listed = deps_.contacts->List()) {
      for (const auto& hop : CollectContactHopCandidates(*listed)) {
        if (!hop.peer_id.empty()) {
          contact_ids.push_back(hop.peer_id);
        }
      }
    }
  }
  MeshConfig mesh_cfg = config().mesh;
  NormalizeMeshConfig(mesh_cfg);
  std::vector<std::string> seed_ids;
  for (const auto& hop : CollectSeedHopCandidates(mesh_cfg.bootstrap_peers)) {
    if (!hop.peer_id.empty()) {
      seed_ids.push_back(hop.peer_id);
    }
  }
  auto has_ep = [punch_links](const std::string& id) {
    return punch_links->GetLinkSnapshot(id).has_endpoint;
  };
  auto is_conn = [punch_links](const std::string& id) { return punch_links->IsConnected(id); };

  // B29: if the first introducer misses (target unknown / channel fail), try the next.
  struct IntroAttempt {
    std::unordered_set<std::string> tried;
    std::function<void()> try_next;
  };
  auto attempt = std::make_shared<IntroAttempt>();
  attempt->try_next = [attempt, punch, target_peer_id, contact_ids = std::move(contact_ids),
                       seed_ids = std::move(seed_ids), has_ep, is_conn,
                       on_done = std::move(on_done)]() mutable {
    auto intro =
        PickPunchIntroducer(contact_ids, seed_ids, target_peer_id, has_ep, is_conn, attempt->tried);
    if (!intro) {
      on_done(Error(attempt->tried.empty() ? "no punch introducer" : "punch introducers exhausted"));
      return;
    }
    attempt->tried.insert(*intro);
    punch->TryColdPunchAsync(
        *intro, target_peer_id, punch->LocalCandidateAddrs(),
        [attempt, on_done](AmpPunchCoordinator::PunchRoe punched) mutable {
          if (punched && punched->ok) {
            CompletePunch(std::move(on_done), std::move(punched), "punch failed");
            return;
          }
          const std::string err =
              !punched ? punched.error().message
                       : (punched->error.empty() ? std::string("punch failed") : punched->error);
          const bool try_another = err.find("unknown to introducer") != std::string::npos ||
                                   err.find("introducer") != std::string::npos ||
                                   err.find("not registered") != std::string::npos;
          if (try_another) {
            attempt->try_next();
            return;
          }
          on_done(Error(err));
        },
        2000);
  };
  attempt->try_next();
}

void CallMediaPlane::TryUpgradePunchAsync(MeshHost* m, const std::string& introducer_peer_key,
                                          const std::string& target_peer_id,
                                          std::function<void(Roe<void>)> on_done) {
  if (!on_done) {
    return;
  }
  if (!m) {
    on_done(Error("amp punch unavailable"));
    return;
  }
  auto* punch = m->AmpPunch();
  if (!punch || !punch->IsStarted()) {
    on_done(Error("amp punch unavailable"));
    return;
  }
  punch->TryUpgradePunchAsync(
      introducer_peer_key, target_peer_id, punch->LocalCandidateAddrs(),
      [on_done = std::move(on_done)](AmpPunchCoordinator::PunchRoe punched) mutable {
        CompletePunch(std::move(on_done), std::move(punched), "upgrade punch failed");
      },
      2000);
}

bool CallMediaPlane::PeerLanConfirmed(const std::string& peer_id) const {
  if (peer_id.empty()) {
    return false;
  }
  if (dial_book_.lan_confirmed_peers.count(peer_id) > 0) {
    return true;
  }
  MeshHost* host = mesh();
  if (!host) {
    return false;
  }
  if (auto chat = host->ChatDeps()) {
    if (chat->links.IsConnected(peer_id)) {
      return true;
    }
  }
  return false;
}

void CallMediaPlane::PrepareForMeshStop(const std::function<void()>& abort_inflight_circuit) {
  // Drop OnRelayChosen + bump gen before AbortInflight Finish posts reserve cbs onto IO.
  InvalidateAsyncOps();
  if (auto* amp = dynamic_cast<AmpCircuitHopReach*>(circuit_hop_reach_.get())) {
    amp->SetOnRelayChosen({});
  }
  if (abort_inflight_circuit) {
    abort_inflight_circuit();
  }
  if (call_media_bridge_) {
    call_media_bridge_->PrepareForTeardown(0);
  }
  if (abort_inflight_circuit) {
    abort_inflight_circuit();
  }
  if (ICallMediaTransport* transport = Transport()) {
    transport->ClearInboundHandler();
    transport->Stop();
  }
  media_relay_client_.reset();
}

void CallMediaPlane::FinishMeshStop() {
  call_media_bridge_.reset();
  media_bridge_bound_sessions_key_ = nullptr;
  call_media_amp_.reset();
  dial_registry_.reset();
}

void CallMediaPlane::DetachRelayClient() {
  if (media_relay_client_) {
    media_relay_client_->Detach();
  }
}

void CallMediaPlane::AbortBridgeAndTransport() {
  if (call_media_bridge_) {
    call_media_bridge_->PrepareForTeardown(0);
  }
  if (ICallMediaTransport* transport = Transport()) {
    transport->Detach();
  }
}

bool CallMediaPlane::IsConnectWorkerInflight() const {
  return call_media_bridge_ && call_media_bridge_->IsConnectWorkerInflight();
}

void CallMediaPlane::ResetRelayClients() {
  media_relay_client_.reset();
  dial_registry_.reset();
}

void CallMediaPlane::Clear() {
  InvalidateAsyncOps();
  if (auto* amp = dynamic_cast<AmpCircuitHopReach*>(circuit_hop_reach_.get())) {
    amp->SetOnRelayChosen({});
  }
  call_media_bridge_.reset();
  media_bridge_bound_sessions_key_ = nullptr;
  call_media_amp_.reset();
  test_media_transport_ = nullptr;
  test_dial_ = nullptr;
  media_relay_client_.reset();
  dial_registry_.reset();
  circuit_hop_reach_.reset();
  chosen_circuit_r1_.clear();
  dial_book_ = {};
}

void CallMediaPlane::StopMeshMedia(const std::string& call_id) {
  if (call_media_bridge_) {
    call_media_bridge_->StopMeshMedia(call_id);
  }
}

void CallMediaPlane::MergeDialBookListenAddrs(const std::string& identity,
                                             const std::vector<std::string>& ranked) {
  std::vector<std::string>& stored = dial_book_.peer_listen_mas[identity];
  for (const std::string& ma : ranked) {
    if (ma.empty()) {
      continue;
    }
    if (std::find(stored.begin(), stored.end(), ma) == stored.end()) {
      stored.push_back(ma);
    }
  }
}

std::string CallMediaPlane::PeerIdFromListenMultiaddr(const std::string& ma) {
  const auto p2p_pos = ma.rfind("/p2p/");
  if (p2p_pos == std::string::npos) {
    return {};
  }
  std::string peer_id = ma.substr(p2p_pos + 5);
  const auto slash = peer_id.find('/');
  if (slash != std::string::npos) {
    peer_id.resize(slash);
  }
  return peer_id;
}

void CallMediaPlane::RegisterCallPeerListenMultiaddrs(const std::string& identity,
                                                     const std::vector<std::string>& multiaddrs) {
  if (identity.empty() || multiaddrs.empty()) {
    return;
  }
  const std::vector<std::string> ranked = RankAmpDialMultiaddrs(multiaddrs, CollectAmpDialLocalContext());
  MergeDialBookListenAddrs(identity, ranked);
  // B28: ingest best-first via RegisterEndpoints (atomic DialBook replace). Do not reverse
  // RegisterEndpoint — PeerSessionDialRegistry posts each write async and out-of-order
  // posts scramble Preferred / candidate order.
  std::vector<std::string> dialable;
  dialable.reserve(ranked.size());
  for (const std::string& ma : ranked) {
    if (ma.empty()) {
      continue;
    }
    const std::string ip = IpHostFromMultiaddrPrefix(ma);
    if (IsLikelyUndialableLanIpv4(ip)) {
      log().info << "Call listen addr skipped undialable dial_key=" << identity << " ma=" << ma;
      continue;
    }
    dialable.push_back(ma);
  }
  if (dialable.empty()) {
    return;
  }
  const std::string peer_id = PeerIdFromListenMultiaddr(dialable.front());
  if (dial_registry_) {
    (void)dial_registry_->RegisterEndpoints(identity, dialable);
    dial_registry_->ClearDialBackoff(identity);
    if (!peer_id.empty()) {
      dial_registry_->ClearDialBackoff(peer_id);
      if (deps_.note_lan_mdns_peer_id) {
        deps_.note_lan_mdns_peer_id(peer_id);
      }
    }
  } else if (deps_.register_peer_direct_endpoint) {
    // No dial registry: fall back to worst→best single RegisterEndpoint for Preferred.
    for (auto it = dialable.rbegin(); it != dialable.rend(); ++it) {
      deps_.register_peer_direct_endpoint(identity, *it);
      if (!peer_id.empty() && peer_id != identity) {
        deps_.register_peer_direct_endpoint(peer_id, *it);
      }
    }
  }
  if (!peer_id.empty() && identity.rfind("account:", 0) == 0 && deps_.note_mesh_peer_id_for_relay) {
    deps_.note_mesh_peer_id_for_relay(identity, peer_id);
  }
  for (const std::string& ma : dialable) {
    log().info << "Call listen addr registered dial_key=" << identity << " ma=" << ma;
  }
}

std::vector<MeshHopCandidate> CallMediaPlane::BuildCircuitRendezvousCandidates(
    const std::string& exclude_peer_id) const {
  std::vector<MeshHopCandidate> out;
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
  const auto effective_seeds = ResolveEffectiveBootstrapPeers(mesh_cfg, directory_nodes);
  auto hops = BuildCircuitHopList(contacts, directory_nodes, dht_nodes, effective_seeds,
                                  mesh_cfg.prefer_contacts_for_routing, include_seeds);
  out.reserve(hops.size());
  for (auto& hop : hops) {
    if (hop.peer_id.empty() || hop.peer_id == exclude_peer_id) {
      continue;
    }
    out.push_back(std::move(hop));
  }
  return out;
}

std::vector<std::string> CallMediaPlane::CollectDialableCircuitRelayIds(
    const std::string& exclude_peer_id) const {
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
  auto hops = BuildCircuitRendezvousCandidates(exclude_peer_id);
  relay_ids.reserve(hops.size());
  for (const MeshHopCandidate& hop : hops) {
    // Dogfood: directory/contact hop MAs are often RFC1918 advertise addrs. Unconditional
    // RegisterEndpoint overwrites a seed-warmed public PreferredMultiaddr and StartBridge
    // then fails with `adp udp :send to`. Only write dialable hosts; keep existing endpoint.
    if (!hop.multiaddr.empty() && amp_links && IsAdpMultiaddr(hop.multiaddr) &&
        CircuitHopDialBookAllowsRegister(hop.multiaddr)) {
      (void)amp_links->RegisterEndpoint(hop.peer_id, hop.multiaddr);
    } else if (hop.multiaddr.empty() && amp_links) {
      if (auto ma = amp_links->PreferredMultiaddr(hop.peer_id)) {
        if (CircuitHopDialBookAllowsRegister(*ma)) {
          (void)amp_links->RegisterEndpoint(hop.peer_id, *ma);
        }
      }
    }
    const bool hop_ok = amp_hops && amp_hops->HasAny(hop.peer_id);
    bool amp_ok = false;
    if (amp_links && amp_links->GetLinkSnapshot(hop.peer_id).has_endpoint) {
      // Capability ingest can replace a public seed Preferred with /ip4/0.0.0.0 listen
      // (dogfood 084055). Skip undialable Preferred unless already Connected (peer-id-only).
      if (amp_links->IsConnected(hop.peer_id)) {
        amp_ok = true;
      } else if (auto ma = amp_links->PreferredMultiaddr(hop.peer_id)) {
        amp_ok = CircuitHopMultiaddrIsUdpDialable(*ma);
      }
    }
    if (amp_ok || hop_ok) {
      relay_ids.push_back(hop.peer_id);
    }
  }
  return relay_ids;
}

void CallMediaPlane::WarmBootstrapSeedSessions() {
  MeshHost* m = mesh();
  if (!m) {
    return;
  }
  auto chat = m->ChatDeps();
  if (!chat) {
    return;
  }
  // EnsureAssociation / RegisterEndpoint must run on Amp IO (dogfood 085210 Coordinator vs MeshPump).
  auto post_io = chat->io.post_io;
  auto task = [this]() { WarmBootstrapSeedSessionsOnIo(); };
  if (post_io) {
    post_io(std::move(task));
  } else {
    task();
  }
}

void CallMediaPlane::WarmBootstrapSeedSessionsOnIo() {
  MeshHost* m = mesh();
  if (!m) {
    return;
  }
  auto chat = m->ChatDeps();
  if (!chat) {
    return;
  }
  MeshConfig mesh_cfg = config().mesh;
  std::vector<MeshDirectoryNode> directory_nodes;
  if (deps_.list_directory_nodes) {
    directory_nodes = deps_.list_directory_nodes();
  }
  auto hops = CollectSeedHopCandidates(ResolveEffectiveBootstrapPeers(mesh_cfg, directory_nodes));
  // Register public bootstrap MAs; dial at most one cold seed (serial). Parallel EnsureAssociation
  // on both Brief hops + peer Preferred contended on ADP UDP (dogfood fd4e3de).
  for (const auto& hop : hops) {
    if (hop.peer_id.empty()) {
      continue;
    }
    if (!hop.multiaddr.empty() && IsAdpMultiaddr(hop.multiaddr) &&
        CircuitHopDialBookAllowsRegister(hop.multiaddr)) {
      (void)chat->links.RegisterEndpoint(hop.peer_id, hop.multiaddr);
    }
  }
  for (const auto& hop : hops) {
    if (hop.peer_id.empty()) {
      continue;
    }
    if (!chat->links.GetLinkSnapshot(hop.peer_id).has_endpoint) {
      log().info << "bootstrap warm skip peer=" << hop.peer_id << " reason=!endpoint";
      continue;
    }
    if (chat->links.IsConnected(hop.peer_id)) {
      log().info << "bootstrap warm already connected peer=" << hop.peer_id;
      continue; // still warm remaining seeds (dialer may pick hop2 — dogfood ae4900eb)
    }
    const std::string restore_ma = hop.multiaddr;
    const std::string peer_id = hop.peer_id;
    IChatPeerLinks* links = &chat->links;
    log().info << "bootstrap warm assoc start peer=" << peer_id << " (serial)";
    chat->links.EnsureAssociation(
        hop.peer_id, [this, links, peer_id, restore_ma](IChatPeerLinks::LinkRoe assoc) {
          if (!assoc) {
            log().warning << "bootstrap warm assoc miss peer=" << peer_id
                          << " err=" << assoc.error().message;
            return;
          }
          if (!restore_ma.empty() && CircuitHopDialBookAllowsRegister(restore_ma)) {
            (void)links->RegisterEndpoint(peer_id, restore_ma);
          }
          log().info << "bootstrap warm assoc ok peer=" << peer_id;
        });
    return; // one cold dial at a time
  }
}

void CallMediaPlane::ReserveOnBootstrapSeeds() {
  MeshHost* m = mesh();
  if (!m || !m->AmpCircuitTunnel() || !m->AmpCircuitTunnel()->IsStarted()) {
    log().warning << "circuit reserve skipped: amp circuit tunnel not started";
    return;
  }
  auto chat = m->ChatDeps();
  if (!chat) {
    log().warning << "circuit reserve skipped: no chat deps";
    return;
  }
  auto post_io = chat->io.post_io;
  // Reserve path registers + associates itself — do not also Warm in the same post (double dial).
  auto task = [this]() { ReserveOnBootstrapSeedsOnIo(); };
  if (post_io) {
    post_io(std::move(task));
  } else {
    task();
  }
}

std::vector<std::string> CallMediaPlane::EffectiveBootstrapSeedPeerIds() const {
  MeshConfig mesh_cfg = config().mesh;
  std::vector<MeshDirectoryNode> directory_nodes;
  if (deps_.list_directory_nodes) {
    directory_nodes = deps_.list_directory_nodes();
  }
  std::vector<std::string> out;
  for (const auto& hop :
       CollectSeedHopCandidates(ResolveEffectiveBootstrapPeers(mesh_cfg, directory_nodes))) {
    if (!hop.peer_id.empty()) {
      out.push_back(hop.peer_id);
    }
  }
  return out;
}

bool CallMediaPlane::AnyBootstrapSeedConnectedOnIo() const {
  MeshHost* m = mesh();
  if (!m) {
    return false;
  }
  auto chat = m->ChatDeps();
  if (!chat) {
    return false;
  }
  for (const std::string& peer_id : EffectiveBootstrapSeedPeerIds()) {
    if (chat->links.IsConnected(peer_id)) {
      return true;
    }
  }
  return false;
}

bool CallMediaPlane::AllBootstrapSeedsConnectedOnIo() const {
  MeshHost* m = mesh();
  if (!m) {
    return false;
  }
  auto chat = m->ChatDeps();
  if (!chat) {
    return false;
  }
  const auto ids = EffectiveBootstrapSeedPeerIds();
  if (ids.empty()) {
    return false;
  }
  for (const std::string& peer_id : ids) {
    if (!chat->links.IsConnected(peer_id)) {
      return false;
    }
  }
  return true;
}

void CallMediaPlane::EnsureBootstrapSeedParkedAsync(std::function<void(bool parked)> on_done,
                                                    const int timeout_ms) {
  if (!on_done) {
    return;
  }
  ReserveOnBootstrapSeeds();
  MeshHost* m = mesh();
  auto chat_opt = m ? m->ChatDeps() : std::nullopt;
  if (!chat_opt || !m->Amp()) {
    on_done(false);
    return;
  }
  const int budget = timeout_ms > 0 ? timeout_ms : 12000;
  auto settled = std::make_shared<std::atomic<bool>>(false);
  auto listener_id = std::make_shared<pp::amp::PeerLinkManager::PeerConnectedListenerId>(0);
  auto deadline_timer = std::make_shared<uint64_t>(0);
  auto* links = &m->Amp()->Runtime().Links();
  auto finish = std::make_shared<std::function<void(bool)>>();
  *finish = [settled, on_done = std::move(on_done), listener_id, deadline_timer,
             links](const bool parked) mutable {
    if (settled->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    if (*listener_id != 0) {
      links->RemovePeerConnectedListener(*listener_id);
      *listener_id = 0;
    }
    if (*deadline_timer != 0) {
      AppRuntime::CancelCoordinatorTimer(*deadline_timer);
      *deadline_timer = 0;
    }
    on_done(parked);
  };

  auto post_io = chat_opt->io.post_io;
  // Prefer Connected on *all* Brief seeds before finishing — dialer StartBridge may pick hop2
  // while answerer only parked hop1 (dogfood ae4900eb / 39412f). Deadline still accepts ≥1.
  auto try_finish_ok = [this, finish, post_io]() {
    auto go = [this, finish]() {
      if (AllBootstrapSeedsConnectedOnIo()) {
        log().info << "bootstrap seed park ok (all seeds Connected)";
        (*finish)(true);
      }
    };
    if (post_io) {
      post_io(std::move(go));
    } else {
      go();
    }
  };

  if (AllBootstrapSeedsConnectedOnIo()) {
    log().info << "bootstrap seed park ok (all seeds Connected)";
    (*finish)(true);
    return;
  }

  std::unordered_set<std::string> seed_ids;
  for (const auto& id : EffectiveBootstrapSeedPeerIds()) {
    if (!id.empty()) {
      seed_ids.insert(id);
    }
  }
  if (seed_ids.empty()) {
    log().warning << "bootstrap seed park timeout (no seed PeerIds)";
    (*finish)(false);
    return;
  }

  *listener_id = links->AddPeerConnectedListener(
      [seed_ids = std::move(seed_ids), try_finish_ok](const std::string& peer_id) {
        if (seed_ids.count(peer_id) == 0) {
          return;
        }
        try_finish_ok();
      });

  // Connected may have landed between check and AddListener.
  try_finish_ok();

  *deadline_timer = AppRuntime::ScheduleCoordinatorOneShot(
      std::chrono::milliseconds(budget), [this, finish, post_io]() {
        auto go = [this, finish]() {
          if (AllBootstrapSeedsConnectedOnIo()) {
            log().info << "bootstrap seed park ok (all seeds Connected)";
            (*finish)(true);
            return;
          }
          if (AnyBootstrapSeedConnectedOnIo()) {
            log().info << "bootstrap seed park ok (partial — deadline with ≥1 Connected)";
            (*finish)(true);
            return;
          }
          log().warning << "bootstrap seed park timeout (no Connected seed)";
          (*finish)(false);
        };
        if (post_io) {
          post_io(std::move(go));
        } else {
          go();
        }
      });
}

bool CallMediaPlane::AwaitCircuitReady(const int timeout_ms) {
  auto done = std::make_shared<std::atomic<bool>>(false);
  auto parked = std::make_shared<bool>(false);
  EnsureBootstrapSeedParkedAsync(
      [done, parked](const bool ok) {
        *parked = ok;
        done->store(true, std::memory_order_release);
      },
      timeout_ms);
  const int budget = timeout_ms > 0 ? timeout_ms : 12000;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget + 250);
  AmpParkUntil([done] { return done->load(std::memory_order_acquire); }, deadline, {});
  return *parked;
}

void CallMediaPlane::ReserveOnBootstrapSeedsOnIo() {
  MeshHost* m = mesh();
  if (!m || !m->AmpCircuitTunnel() || !m->AmpCircuitTunnel()->IsStarted()) {
    log().warning << "circuit reserve on-io skipped: tunnel not started";
    return;
  }
  auto chat = m->ChatDeps();
  if (!chat) {
    return;
  }
  // H011: same BuildCircuitHopList surface as dialer CollectDialableCircuitRelayIds (not
  // seeds-only). Dialer sticky-reorder can StartBridge any surface member — park must cover it.
  auto candidates = BuildCircuitRendezvousCandidates();
  std::string sticky_r1 = chosen_circuit_r1_;
  if (sticky_r1.empty() && circuit_hop_reach_) {
    sticky_r1 = circuit_hop_reach_->LastGoodRelayPeerKey();
  }
  if (!sticky_r1.empty()) {
    log().info << "circuit rendezvous park sticky R1=" << sticky_r1;
  }
  for (const auto& hop : candidates) {
    if (hop.peer_id.empty() || hop.multiaddr.empty()) {
      continue;
    }
    if (IsAdpMultiaddr(hop.multiaddr) && CircuitHopDialBookAllowsRegister(hop.multiaddr)) {
      (void)chat->links.RegisterEndpoint(hop.peer_id, hop.multiaddr);
    }
  }

  std::vector<std::string> surface_ids;
  surface_ids.reserve(candidates.size());
  for (const auto& hop : candidates) {
    if (!hop.peer_id.empty()) {
      surface_ids.push_back(hop.peer_id);
    }
  }
  auto ordered_ids = OrderRendezvousParkAttempts(
      std::move(surface_ids), sticky_r1,
      [&](const std::string& peer_id) { return chat->links.IsConnected(peer_id); });

  std::unordered_map<std::string, std::string> ma_by_peer;
  for (const auto& hop : candidates) {
    if (!hop.peer_id.empty() && !hop.multiaddr.empty()) {
      ma_by_peer.emplace(hop.peer_id, hop.multiaddr);
    }
  }

  std::vector<MeshHopCandidate> hops;
  hops.reserve(ordered_ids.size());
  for (const std::string& peer_id : ordered_ids) {
    MeshHopCandidate hop;
    hop.peer_id = peer_id;
    if (auto it = ma_by_peer.find(peer_id); it != ma_by_peer.end()) {
      hop.multiaddr = it->second;
    }
    hops.push_back(std::move(hop));
  }

  std::size_t connected_count = 0;
  for (const auto& hop : hops) {
    if (chat->links.IsConnected(hop.peer_id)) {
      ++connected_count;
    }
  }
  const std::size_t cold_limit = RendezvousColdDialLimit(
      hops.size(), connected_count, kCircuitRendezvousParkCoverage, /*cover_all_remaining=*/true);
  log().info << "circuit rendezvous reserve surface=" << hops.size()
             << " connected=" << connected_count << " cold_limit=" << cold_limit
             << " coverage_k=" << kCircuitRendezvousParkCoverage;

  auto start_reserve = deferred_.Bind([this, m](const std::string& relay) {
    if (!m->AmpCircuitTunnel() || !m->AmpCircuitTunnel()->IsStarted()) {
      return;
    }
    const auto id = m->AmpCircuitTunnel()->StartReserve(
        relay,
        deferred_.Bind([this, relay](Roe<CircuitTunnelBridgeResult> result) {
          if (!result || !result->ok) {
            // Live Brief may still lack op=reserve (dogfood fd4e3de "unsupported op"). Connected
            // PeerLink alone is enough for peer-id-only ServeDial — log and keep the link.
            log().warning << "circuit reserve miss peer=" << relay
                          << " err="
                          << (!result ? result.error().message
                                      : (result->error.empty() ? "rejected" : result->error));
            return;
          }
          log().info << "circuit reserve ok peer=" << relay;
        }),
        15000);
    if (!id) {
      log().warning << "circuit reserve StartReserve rejected peer=" << relay;
      return;
    }
    log().info << "circuit reserve started on rendezvous peer=" << relay;
  });

  // Reserve every Connected surface member first (H011 / dogfood 39412f).
  for (const auto& hop : hops) {
    if (hop.peer_id.empty()) {
      continue;
    }
    if (!chat->links.GetLinkSnapshot(hop.peer_id).has_endpoint) {
      log().info << "circuit reserve skip peer=" << hop.peer_id << " reason=!endpoint";
      continue;
    }
    if (chat->links.IsConnected(hop.peer_id)) {
      start_reserve(hop.peer_id);
    }
  }

  // Serial cold dial + reserve for remaining surface members (cover_all_remaining).
  auto try_at = std::make_shared<std::function<void(size_t, size_t)>>();
  *try_at = deferred_.Bind([this, chat, hops = std::move(hops), start_reserve, cold_limit, try_at](
                               size_t index, size_t cold_started) mutable {
    while (index < hops.size()) {
      if (cold_started >= cold_limit) {
        return;
      }
      const auto& hop = hops[index];
      if (hop.peer_id.empty() || !chat->links.GetLinkSnapshot(hop.peer_id).has_endpoint) {
        ++index;
        continue;
      }
      if (chat->links.IsConnected(hop.peer_id)) {
        // Already reserved in the Connected pass above.
        ++index;
        continue;
      }
      const std::string relay = hop.peer_id;
      const std::string restore_ma = hop.multiaddr;
      IChatPeerLinks* links = &chat->links;
      const size_t next = index + 1;
      const size_t next_cold = cold_started + 1;
      log().info << "circuit reserve assoc start peer=" << relay << " (serial index=" << index
                 << " cold=" << next_cold << "/" << cold_limit << ")";
      chat->links.EnsureAssociation(
          relay, deferred_.Bind([this, start_reserve, links, relay, restore_ma, try_at, next,
                                 next_cold](IChatPeerLinks::LinkRoe assoc) mutable {
            if (!assoc) {
              log().warning << "circuit reserve assoc miss peer=" << relay
                            << " err=" << assoc.error().message;
              if (try_at && *try_at) {
                (*try_at)(next, next_cold);
              }
              return;
            }
            if (!restore_ma.empty() && CircuitHopDialBookAllowsRegister(restore_ma)) {
              (void)links->RegisterEndpoint(relay, restore_ma);
            }
            start_reserve(relay);
            if (try_at && *try_at) {
              (*try_at)(next, next_cold);
            }
          }));
      return;
    }
  });
  (*try_at)(0, 0);
}

void CallMediaPlane::PreferLateReserve(const std::string& relay_peer_id) {
  if (relay_peer_id.empty()) {
    return;
  }
  chosen_circuit_r1_ = relay_peer_id;
  MeshHost* m = mesh();
  if (!m || !m->AmpCircuitTunnel() || !m->AmpCircuitTunnel()->IsStarted()) {
    log().warning << "circuit late-reserve skipped: tunnel not started peer=" << relay_peer_id;
    return;
  }
  auto chat = m->ChatDeps();
  if (!chat) {
    return;
  }
  auto post_io = chat->io.post_io;
  auto task = deferred_.Bind([this, relay_peer_id]() { PreferLateReserveOnIo(relay_peer_id); });
  if (post_io) {
    post_io(std::move(task));
  } else {
    task();
  }
}

void CallMediaPlane::PreferLateReserveOnIo(const std::string& relay_peer_id) {
  MeshHost* m = mesh();
  if (!m || !m->AmpCircuitTunnel() || !m->AmpCircuitTunnel()->IsStarted()) {
    return;
  }
  auto chat = m->ChatDeps();
  if (!chat || relay_peer_id.empty()) {
    return;
  }
  // Ensure the chosen R1 is on the surface with a dialable MA when possible.
  for (const auto& hop : BuildCircuitRendezvousCandidates()) {
    if (hop.peer_id != relay_peer_id) {
      continue;
    }
    if (!hop.multiaddr.empty() && IsAdpMultiaddr(hop.multiaddr) &&
        CircuitHopDialBookAllowsRegister(hop.multiaddr)) {
      (void)chat->links.RegisterEndpoint(hop.peer_id, hop.multiaddr);
    }
    break;
  }
  if (!chat->links.GetLinkSnapshot(relay_peer_id).has_endpoint) {
    log().warning << "circuit late-reserve skip peer=" << relay_peer_id << " reason=!endpoint";
    return;
  }
  auto start_one = deferred_.Bind([this, m, relay_peer_id]() {
    const auto id = m->AmpCircuitTunnel()->StartReserve(
        relay_peer_id,
        deferred_.Bind([this, relay_peer_id](Roe<CircuitTunnelBridgeResult> result) {
          if (!result || !result->ok) {
            log().warning << "circuit late-reserve miss peer=" << relay_peer_id
                          << " err="
                          << (!result ? result.error().message
                                      : (result->error.empty() ? "rejected" : result->error));
            return;
          }
          log().info << "circuit late-reserve ok peer=" << relay_peer_id;
        }),
        15000);
    if (!id) {
      log().warning << "circuit late-reserve StartReserve rejected peer=" << relay_peer_id;
      return;
    }
    log().info << "circuit late-reserve started peer=" << relay_peer_id;
  });
  if (chat->links.IsConnected(relay_peer_id)) {
    start_one();
    return;
  }
  log().info << "circuit late-reserve assoc start peer=" << relay_peer_id;
  chat->links.EnsureAssociation(relay_peer_id,
                                deferred_.Bind([this, start_one, relay_peer_id](IChatPeerLinks::LinkRoe assoc) {
                                  if (!assoc) {
                                    log().warning << "circuit late-reserve assoc miss peer=" << relay_peer_id
                                                  << " err=" << assoc.error().message;
                                    return;
                                  }
                                  start_one();
                                }));
}

Roe<void> CallMediaPlane::TryEnsureCircuitHopReachable(const std::string& hop_peer_id) {
  if (AppRuntime::IsShuttingDown()) {
    log().debug << "TryEnsureCircuitHopReachable rejected: shutting down";
    return Error("shutdown in progress");
  }
  if (!circuit_hop_reach_) {
    return Error("Amp circuit reach required");
  }
  return circuit_hop_reach_->TryEnsureHopReachable(hop_peer_id);
}

Roe<void> CallMediaPlane::TryEnsureCallMediaReachable(const std::string& peer_key) {
  if (AppRuntime::IsShuttingDown()) {
    log().debug << "TryEnsureCallMediaReachable rejected: shutting down";
    return Error("shutdown in progress");
  }
  if (!circuit_hop_reach_) {
    return Error("Amp circuit reach required");
  }
  if (peer_key.empty()) {
    return Error("missing call peer");
  }
  return circuit_hop_reach_->TryEnsureCallMediaReachable(peer_key);
}

void CallMediaPlane::TryEnsureCallMediaReachableAsync(const std::string& peer_key,
                                                      std::function<void(Roe<void>)> on_done) {
  if (!on_done) {
    return;
  }
  if (AppRuntime::IsShuttingDown()) {
    on_done(Error("shutdown in progress"));
    return;
  }
  if (!circuit_hop_reach_) {
    on_done(Error("Amp circuit reach required"));
    return;
  }
  if (peer_key.empty()) {
    on_done(Error("missing call peer"));
    return;
  }
  circuit_hop_reach_->TryEnsureCallMediaReachableAsync(peer_key, std::move(on_done));
}

Roe<void> CallMediaPlane::TryUpgradeCallMediaToDirect(const std::string& peer_key) {
  if (!circuit_hop_reach_) {
    return Error("amp circuit reach required");
  }
  if (peer_key.empty()) {
    return Error("missing call peer");
  }
  return circuit_hop_reach_->TryUpgradeToDirect(peer_key);
}

} // namespace pbr
