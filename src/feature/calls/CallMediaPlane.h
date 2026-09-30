#pragma once

#include "foundation/data/Config.h"
#include "domain/media/CallMediaEngine.h"
#include "domain/messaging/CallSessionStore.h"
#include "domain/messaging/CallMediaKeyStore.h"
#include "common/Error.h"
#include "common/Module.h"
#include "domain/mesh/connectivity/MeshConnectivity.h"
#include "domain/mesh/media_plane/MeshMediaRelay.h"
#include "feature/calls/CallDirectPathDeps.h"
#include "feature/calls/CallMediaSeat.h"
#include "feature/calls/CallMediaHost.h"
#include "feature/calls/CallTopologyController.h"
#include "feature/calls/CallTopologyRelayDeps.h"
#include "domain/mesh/l4/call_media/CallMediaAmpTransport.h"
#include "domain/mesh/l4/call_media/ICallMediaTransport.h"
#include "domain/mesh/host/MeshHost.h"
#include "common/directory/MeshHopTypes.h"


#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Call media plane under CallStack (V040): the call_media Amp transport, the 1:1 path's deps (the
 * session manager owns the path itself) and the topology's relay deps. The neutral mesh media objects (media_relay client, dial registry,
 * circuit reach, rendezvous parking) are borrowed from the product hub's `MeshConnectivity` and
 * `MeshMediaRelay` (L015).
 *
 * Does **not** hold standing pointers to CallStack siblings (CSM / stores); the stack fills deps
 * callbacks.
 */
struct CallMediaPlaneDeps {
  std::function<MeshHost*()> mesh;
  std::function<std::shared_ptr<const MeshConfig>()> mesh_config;
  std::function<std::vector<MeshDirectoryNode>()> list_directory_nodes;
  std::function<std::vector<MeshDirectoryNode>()> list_dht_nodes;
  std::function<bool()> seed_dial_ok;
  /** Stack computes advertise set (role + ephemeral desire + MeshHost). */
  std::function<std::vector<std::string>()> local_listen_multiaddrs;
  /** SoftMigrate relay-cap queries — filled from CallSessionManager by CallStack. */
  std::function<bool(const std::string& peer_id)> peer_has_media_relay;
  std::function<std::vector<std::string>()> list_media_relay_peers;
  /** Dial-book account: ↔ PeerId learning — any thread (the stack enqueues it for the calls owner). */
  std::function<void(const std::string& account_identity, const std::string& peer_id)>
      note_mesh_peer_id_for_relay;
};

class CallMediaPlane : public Module {
public:
  CallMediaPlane();
  ~CallMediaPlane() override;

  void SetDeps(CallMediaPlaneDeps deps);
  /** Borrowed neutral mesh objects (outlive the plane; null = none). */
  void SetMesh(MeshConnectivity* connectivity, MeshMediaRelay* media_relay) {
    connectivity_ = connectivity;
    media_relay_ = media_relay;
  }

  /** Mesh up: create / start the Amp call-media transport. */
  void OnMeshStarted();
  /** Test / harness: use this transport instead of Amp call-media (not owned). */
  void BindTestMediaPath(ICallMediaTransport* transport);
  /** What the 1:1 path runs on (transport, dial, reach, seed hooks); not usable without a transport. */
  CallDirectPathDeps DirectPathDeps();
  CallTopologyController::MediaRelayDeps BuildMediaRelayDeps() const;
  /** Mesh stop: the transport stops taking and carrying streams (the 1:1 path let go before). */
  void StopTransport();
  void FinishMeshStop();
  /** Group SFU: close media_relay before LeaveCall joins capture. */
  void DetachRelayClient();
  /** Transport Detach (after LeaveCall; the 1:1 path let go before). */
  void DetachTransport();
  /** Reset all plane-owned objects (CallStack::Shutdown). */
  void Clear();


  /** Register a call peer's listen addrs in the mesh listen book; learns account ↔ PeerId. */
  void RegisterCallPeerListenMultiaddrs(const std::string& identity,
                                        const std::vector<std::string>& multiaddrs);

private:
  MeshHost* mesh() const { return deps_.mesh ? deps_.mesh() : nullptr; }
  /** Mesh config snapshot (defaults when none is wired). */
  std::shared_ptr<const MeshConfig> mesh_config() const;
  ICallMediaTransport* Transport();

  CallMediaPlaneDeps deps_;
  MeshConnectivity* connectivity_ = nullptr;
  MeshMediaRelay* media_relay_ = nullptr;
  std::unique_ptr<CallMediaAmpTransport> call_media_amp_;
  ICallMediaTransport* test_media_transport_ = nullptr;
};

} // namespace pbr
