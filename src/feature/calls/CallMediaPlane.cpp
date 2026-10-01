#include "feature/calls/CallMediaPlane.h"

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
  if (!connectivity_ || !media_relay_) {
    return deps;
  }
  MeshHost* m = mesh();
  const bool use_amp_relay = media_relay_->AmpRelayAvailable();
  const MediaRelayAttachPorts ports = media_relay_->RelayAttachPorts();
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
  deps.trusted_relays_only = mesh_cfg.trusted_relays_only;
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
  MeshConnectivity* connectivity = connectivity_;
  deps.resolve_remote_listen_by_peer = [connectivity]() { return *connectivity->PeerListenBook(); };
  deps.peer_lan_confirmed = [connectivity](const std::string& peer_id) {
    return connectivity->PeerLanConfirmed(peer_id);
  };
  if (deps.local_listen_multiaddr.find("/ip4/0.0.0.0/") != std::string::npos ||
      deps.local_listen_multiaddr.find("/ip6/::/") != std::string::npos) {
    deps.local_listen_multiaddr.clear();
  }
  return deps;
}

CallDirectPathDeps CallMediaPlane::DirectPathDeps() {
  CallDirectPathDeps deps;
  deps.transport = Transport();
  if (!connectivity_) {
    return deps;
  }
  deps.dial = connectivity_->Dial();
  deps.circuit_reach = connectivity_->CircuitReach();
  CircuitRendezvousCoordinator* rendezvous = &connectivity_->Rendezvous();
  deps.seed_warm = [rendezvous]() { rendezvous->WarmBootstrapSeedSessions(); };
  deps.seed_reserve = [rendezvous]() { rendezvous->ReserveOnBootstrapSeeds(); };
  deps.seed_park_await = [rendezvous](std::function<void(bool)> done, int timeout_ms) {
    rendezvous->EnsureBootstrapSeedParkedAsync(std::move(done), timeout_ms);
  };
  return deps;
}

void CallMediaPlane::StopTransport() {
  if (ICallMediaTransport* transport = Transport()) {
    transport->ClearInboundHandler();
    transport->Stop();
  }
}

void CallMediaPlane::FinishMeshStop() {
  call_media_amp_.reset();
}

void CallMediaPlane::DetachRelayClient() {
  if (IMediaRelayClient* relay = media_relay_ ? media_relay_->RelayClient() : nullptr) {
    relay->Detach();
  }
}

void CallMediaPlane::DetachTransport() {
  if (ICallMediaTransport* transport = Transport()) {
    transport->Detach();
  }
}

void CallMediaPlane::Clear() {
  call_media_amp_.reset();
  test_media_transport_ = nullptr;
}

void CallMediaPlane::RegisterCallPeerListenMultiaddrs(const std::string& identity,
                                                     const std::vector<std::string>& multiaddrs) {
  if (!connectivity_) {
    return;
  }
  if (identity.rfind("account:", 0) != 0 || !deps_.note_mesh_peer_id_for_relay) {
    connectivity_->RegisterPeerListenMultiaddrs(identity, multiaddrs);
    return;
  }
  // Registered on the connectivity owner; the note takes the account → PeerId to the calls owner.
  connectivity_->RegisterPeerListenMultiaddrs(
      identity, multiaddrs, [identity, note = deps_.note_mesh_peer_id_for_relay](const std::string& peer_id) {
        if (!peer_id.empty()) {
          note(identity, peer_id);
        }
      });
}

} // namespace pbr
