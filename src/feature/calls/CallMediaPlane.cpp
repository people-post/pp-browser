#include "feature/calls/CallMediaPlane.h"
#include "feature/calls/CallsThread.h"

#include "foundation/data/MeshRole.h"

#include <functional>
#include <memory>
#include "common/PbrCompat.h"

namespace pbr {

CallMediaPlane::CallMediaPlane() {
  redirectLogger("CallMediaPlane");
}

CallMediaPlane::~CallMediaPlane() {
  Clear();
}

std::shared_ptr<const MeshConfig> CallMediaPlane::mesh_config() const {
  auto cfg = deps_.mesh_config ? deps_.mesh_config() : nullptr;
  return cfg ? cfg : std::make_shared<const MeshConfig>();
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
    return;
  }
  auto pump = [m]() { m->Tick(); };
  call_media_amp_ = std::make_unique<CallMediaAmpTransport>(m->Amp()->Runtime(), std::move(pump));
  call_media_amp_->Start();
  log().info << "call-media transport=amp";
}

ICallMediaTransport* CallMediaPlane::Transport() {
  if (test_media_transport_) {
    return test_media_transport_;
  }
  return call_media_amp_.get();
}

void CallMediaPlane::BindTestMediaPath(ICallMediaTransport* transport) {
  test_media_transport_ = transport;
}

CallTopologyController::MediaRelayDeps CallMediaPlane::BuildMediaRelayDeps() const {
  CallTopologyController::MediaRelayDeps deps;
  if (!mesh_media_) {
    return deps;
  }
  MeshHost* m = mesh();
  const bool use_amp_relay = mesh_media_->AmpRelayAvailable();
  const MediaRelayAttachPorts ports = mesh_media_->RelayAttachPorts();
  deps.relay = ports.relay;
  deps.dial = ports.dial;
  deps.circuit_reach = ports.service_reach;
  deps.objects_alive = ports.objects_alive;
  deps.objects_snap = ports.objects_snap;
  const auto snapshot = mesh_config();
  MeshConfig mesh_cfg = *snapshot;
  NormalizeMeshConfig(mesh_cfg);
  deps.bootstrap_peers = mesh_cfg.bootstrap_peers;
  deps.prefer_contacts = mesh_cfg.prefer_contacts_for_routing;
  deps.list_directory_nodes = deps_.list_directory_nodes;
  deps.list_dht_nodes = deps_.list_dht_nodes;
  deps.seed_dial_ok = deps_.seed_dial_ok;
  deps.prefer_local_as_hop =
      ResolveMeshRole(*snapshot) == MeshRole::Node && mesh_cfg.capabilities.media_relay && use_amp_relay;
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
  MeshMediaPlane* mesh_media = mesh_media_;
  deps.resolve_remote_listen_by_peer = [mesh_media]() { return *mesh_media->PeerListenBook(); };
  deps.peer_lan_confirmed = [mesh_media](const std::string& peer_id) { return mesh_media->PeerLanConfirmed(peer_id); };
  if (deps.local_listen_multiaddr.find("/ip4/0.0.0.0/") != std::string::npos ||
      deps.local_listen_multiaddr.find("/ip6/::/") != std::string::npos) {
    deps.local_listen_multiaddr.clear();
  }
  return deps;
}

void CallMediaPlane::BindBridge(const CallMediaBridgeBindArgs& args) {
  ICallMediaTransport* transport = Transport();
  IDialRegistry* dial = mesh_media_ ? mesh_media_->Dial() : nullptr;
  ICircuitHopReach* reach = mesh_media_ ? mesh_media_->CircuitReach() : nullptr;
  if (!transport || !dial || !args.host || !args.session_store || !args.media_keys || !args.media_engine) {
    call_media_bridge_.reset();
    media_bridge_bound_sessions_key_ = nullptr;
    return;
  }
  const bool sessions_changed = (media_bridge_bound_sessions_key_ != args.sessions_key);
  if (!call_media_bridge_ || sessions_changed) {
    call_media_bridge_ = std::make_unique<CallMediaBridge>(*args.host, *args.session_store, *args.media_keys,
                                                           *args.media_engine, *transport, dial, reach);
    media_bridge_bound_sessions_key_ = args.sessions_key;
    log().info << "CallMediaBridge bound (sessions_changed=" << (sessions_changed ? 1 : 0)
               << " transport=" << (test_media_transport_ ? "test" : "amp") << ")";
  } else {
    call_media_bridge_->SetReachDeps(dial, reach);
  }
  CircuitRendezvousCoordinator* rendezvous = &mesh_media_->Rendezvous();
  call_media_bridge_->SetSeedWarm([rendezvous]() { rendezvous->WarmBootstrapSeedSessions(); });
  call_media_bridge_->SetSeedReserve([rendezvous]() { rendezvous->ReserveOnBootstrapSeeds(); });
  call_media_bridge_->SetSeedParkAwait([rendezvous](std::function<void(bool)> done, int timeout_ms) {
    rendezvous->EnsureBootstrapSeedParkedAsync(std::move(done), timeout_ms);
  });
}

void CallMediaPlane::DetachFromMeshMedia() {
  if (call_media_bridge_) {
    call_media_bridge_->SetReachDeps(nullptr, nullptr);
  }
}

void CallMediaPlane::PrepareForMeshStop(const std::function<void()>& abort_inflight_circuit) {
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
}

void CallMediaPlane::FinishMeshStop() {
  call_media_bridge_.reset();
  media_bridge_bound_sessions_key_ = nullptr;
  call_media_amp_.reset();
}

void CallMediaPlane::DetachRelayClient() {
  if (IMediaRelayClient* relay = mesh_media_ ? mesh_media_->RelayClient() : nullptr) {
    relay->Detach();
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

void CallMediaPlane::Clear() {
  call_media_bridge_.reset();
  media_bridge_bound_sessions_key_ = nullptr;
  call_media_amp_.reset();
  test_media_transport_ = nullptr;
}

void CallMediaPlane::StopMeshMedia(const std::string& call_id) {
  if (call_media_bridge_) {
    call_media_bridge_->StopMeshMedia(call_id);
  }
}

void CallMediaPlane::RegisterCallPeerListenMultiaddrs(const std::string& identity,
                                                     const std::vector<std::string>& multiaddrs) {
  if (!mesh_media_) {
    return;
  }
  if (identity.rfind("account:", 0) != 0 || !deps_.note_mesh_peer_id_for_relay) {
    mesh_media_->RegisterPeerListenMultiaddrs(identity, multiaddrs);
    return;
  }
  // Registered on the connectivity owner; the account → PeerId note is call state (calls owner).
  mesh_media_->RegisterPeerListenMultiaddrs(
      identity, multiaddrs, [identity, note = deps_.note_mesh_peer_id_for_relay](const std::string& peer_id) {
        if (!peer_id.empty()) {
          CallsThread::Post([identity, note, peer_id]() { note(identity, peer_id); });
        }
      });
}

} // namespace pbr
