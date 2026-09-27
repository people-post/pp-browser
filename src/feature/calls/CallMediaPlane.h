#pragma once

#include "foundation/data/Config.h"
#include "domain/media/CallMediaEngine.h"
#include "domain/messaging/CallSessionStore.h"
#include "domain/messaging/CallMediaKeyStore.h"
#include "common/Error.h"
#include "common/Module.h"
#include "domain/mesh/media_plane/MeshMediaPlane.h"
#include "feature/calls/CallMediaBridge.h"
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
 * Call media plane under CallStack (V040): the call_media Amp transport, CallMediaBridge ownership
 * and the topology's relay deps. The neutral mesh media objects (media_relay client, dial registry,
 * circuit reach, rendezvous parking) are borrowed from the product hub's `MeshMediaPlane` (L015).
 *
 * Does **not** hold standing pointers to CallStack siblings (CSM / stores / seat / lifecycle).
 * Stack passes those only into `BindBridge` and fills deps callbacks.
 */
struct CallMediaPlaneDeps {
  std::function<MeshHost*()> mesh;
  std::function<const AppConfig&()> config;
  std::function<std::vector<MeshDirectoryNode>()> list_directory_nodes;
  std::function<std::vector<MeshDirectoryNode>()> list_dht_nodes;
  std::function<bool()> seed_dial_ok;
  /** Stack computes advertise set (role + ephemeral desire + MeshHost). */
  std::function<std::vector<std::string>()> local_listen_multiaddrs;
  /** SoftMigrate relay-cap queries — filled from CallSessionManager by CallStack. */
  std::function<bool(const std::string& peer_id)> peer_has_media_relay;
  std::function<std::vector<std::string>()> list_media_relay_peers;
  /** Dial-book account: ↔ PeerId learning (CallSessionManager::NoteMeshPeerIdForRelay). */
  std::function<void(const std::string& account_identity, const std::string& peer_id)>
      note_mesh_peer_id_for_relay;
};

/** Args for one bridge bind; not retained on the plane after BindBridge returns. */
struct CallMediaBridgeBindArgs {
  CallMediaHost* host = nullptr;
  CallSessionStore* session_store = nullptr;
  CallMediaKeyStore* media_keys = nullptr;
  CallMediaEngine* media_engine = nullptr;
  /** Rebuild key (typically CallSessionManager*); compare only, do not dereference as CSM. */
  const void* sessions_key = nullptr;
};

class CallMediaPlane : public Module {
public:
  CallMediaPlane();
  ~CallMediaPlane() override;

  void SetDeps(CallMediaPlaneDeps deps);
  /** Borrowed neutral mesh media (outlives the plane; null = none). */
  void SetMeshMedia(MeshMediaPlane* mesh_media) { mesh_media_ = mesh_media; }

  /** Mesh up: create / start the Amp call-media transport. */
  void OnMeshStarted();
  /** Test / harness: use this transport instead of Amp call-media (not owned). */
  void BindTestMediaPath(ICallMediaTransport* transport);
  /**
   * Construct or refresh CallMediaBridge from stack-owned ingredients.
   * Rebuilds when `sessions_key` changes; otherwise updates reach deps + seat/lifecycle.
   */
  void BindBridge(const CallMediaBridgeBindArgs& args);
  CallTopologyController::MediaRelayDeps BuildMediaRelayDeps() const;
  /** Before the mesh media objects are replaced / dropped: the bridge lets go of dial + reach. */
  void DetachFromMeshMedia();

  /** Media half of mesh stop (bridge PrepareForTeardown + transport stop), bracketed by aborts. */
  void PrepareForMeshStop(const std::function<void()>& abort_inflight_circuit);
  void FinishMeshStop();
  /** Group SFU: close media_relay before LeaveCall joins capture. */
  void DetachRelayClient();
  /** Bridge PrepareForTeardown(0) + transport Detach (after LeaveCall). */
  void AbortBridgeAndTransport();
  bool IsConnectWorkerInflight() const;
  /** Reset all plane-owned objects (CallStack::Shutdown). */
  void Clear();

  CallMediaBridge* Bridge() { return call_media_bridge_.get(); }
  void StopMeshMedia(const std::string& call_id);

  /** Register a call peer's listen addrs in the mesh listen book; learns account ↔ PeerId. */
  void RegisterCallPeerListenMultiaddrs(const std::string& identity,
                                        const std::vector<std::string>& multiaddrs);

private:
  MeshHost* mesh() const { return deps_.mesh ? deps_.mesh() : nullptr; }
  const AppConfig& config() const;
  ICallMediaTransport* Transport();

  CallMediaPlaneDeps deps_;
  MeshMediaPlane* mesh_media_ = nullptr;
  std::unique_ptr<CallMediaBridge> call_media_bridge_;
  const void* media_bridge_bound_sessions_key_ = nullptr;
  std::unique_ptr<CallMediaAmpTransport> call_media_amp_;
  ICallMediaTransport* test_media_transport_ = nullptr;
};

} // namespace pbr
