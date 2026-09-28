#pragma once

#include "foundation/data/Config.h"
#include "domain/media/CallMediaEngine.h"
#include "domain/messaging/CallSessionStore.h"
#include "domain/messaging/SqlitePskSessionStore.h"
#include "common/Error.h"
#include "common/Module.h"
#include "common/thread/IThreadStore.h"
#include "domain/people/ContactsStore.h"
#include "domain/people/IdentityStore.h"
#include "feature/calls/CallDeliveryPorts.h"
#include "feature/calls/CallMediaBridge.h"
#include "feature/calls/CallMediaPlane.h"
#include "feature/calls/CallMediaSeat.h"
#include "feature/calls/CallLifecycle.h"
#include "domain/messaging/CallMediaKeyStore.h"
#include "feature/calls/CallSessionManager.h"
#include "feature/calls/CallUiState.h"
#include "feature/calls/CallsThread.h"
#include "feature/calls/SharedPorts.h"
#include "feature/calls/CallTopologyRelayDeps.h"
#include "feature/calls/CallControlInboundPorts.h"
#include "domain/mesh/l4/call_media/ICallMediaTransport.h"
#include "domain/mesh/host/MeshHost.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Call media / session / lifecycle stack (Wave 3 / V040).
 *
 * Phase assembler: profile stores, CSM, Lifecycle, MediaSeat, and `CallMediaPlane`
 * (call_media transport + bridge) over the hub's borrowed `MeshMediaPlane` (L015). Hub owns `unique_ptr<CallStack>`, forwards
 * `Calls()`/`Lifecycle()`, injects mesh/config/mDNS glue through CallStackDeps.
 *
 * CallUiBackend binds a CallStack& directly (not the Hub) for call APIs.
 */
struct CallStackDeps {
  IThreadStore* store = nullptr;
  ContactsStore* contacts = nullptr;
  IdentityStore* identity = nullptr;
  SqlitePskSessionStore* psk = nullptr;
  /** Delivery + dial registration; filled from MeshDeliveryOrchestrator without typing it here. */
  CallDeliveryPorts delivery;
  /** Wire inbound call-control into conversations receive path after CSM is built. */
  std::function<void(CallControlInboundPorts ports)> bind_call_control;

  /** Current MeshHost (null before StartMesh, reset on stop). */
  std::function<MeshHost*()> mesh;
  /** Live AppConfig (libp2p role / caps / bootstrap / listen multiaddr). */
  /** The hub's mesh config as last published (immutable snapshot — read from any owner). */
  std::function<std::shared_ptr<const MeshConfig>()> mesh_config;

  /** Cached mesh_node rows from Brief directory (n-dir). */
  std::function<std::vector<MeshDirectoryNode>()> list_directory_nodes;
  /** DHT peer_routing cache (n2-caps); directory-shaped nodes. */
  std::function<std::vector<MeshDirectoryNode>()> list_dht_nodes;
  /** Reachability seed probe — when false, hop policy skips org seed candidates. */
  std::function<bool()> seed_dial_ok;

  /** Hub-owned mesh glue the call stack cannot own. */
  std::function<void(const std::string& identity)> prefetch_peer_reachability;
  std::function<void()> sync_mobile_ephemeral_listen;
  std::function<void(const std::string& peer_id)> note_lan_mdns_peer_id;

  /**
   * Neutral mesh media (relay client, dial, reach, parking) — owned by the product hub, outlives
   * the stack (L015). The owner calls `DetachMeshMedia` before replacing its objects and
   * `RebindMeshMedia` after.
   */
  MeshMediaPlane* mesh_media = nullptr;
};

class CallStack : public Module {
public:
  CallStack();
  ~CallStack() override;

  /** Phase A (profile init, no mesh): create call session store, media key store, media engine. */
  Roe<void> InitializeStores(const std::string& profile_db_path, const std::string& profile_id);
  /** Phase A: build CSM against current p2p, wire providers, bind lifecycle + media plane. */
  void BuildSessions(const CallStackDeps& deps);
  /** Phase B (mesh up, mesh media wired by the owner): start Amp call-media transport + bind. */
  void OnMeshServicesStarted();
  /**
   * Test-only: bind CallMediaBridge without Amp mesh.
   * `transport` / `dial` / `circuit_reach` are non-owning; call after BuildSessions.
   */
  void BindTestMediaPath(ICallMediaTransport* transport, IDialRegistry* dial);
  void BindTestMediaPath(ICallMediaTransport* transport, IDialRegistry* dial,
                         ICircuitHopReach* circuit_reach);
  /** Teardown before mesh Stop: clear bindings, PrepareForTeardown; abort circuit via callback. */
  void PrepareForMeshStop(const std::function<void()>& abort_inflight_circuit);
  /** Teardown after mesh Stop: drop the bridge and the call-media transport. */
  void FinishMeshStop();
  /** k5: the device's network changed (any thread) — calls react on their owner. */
  void OnLocalNetworkChanged();
  /** Before the owner replaces / drops mesh media objects: topology + bridge let go of them. */
  void DetachMeshMedia();
  /** After the owner rewired mesh media: rebind bridge + topology to the new objects. */
  void RebindMeshMedia();
  /** Reset call session manager + lifecycle (Hub teardown ordering before p2p reset). */
  void ResetSessions();
  /** Final teardown: reset media engine / key store / session store. */
  void Shutdown();

  /** GUI view of the call stack, published by the calls owner after each step (t2b). */
  std::shared_ptr<const CallUiState> UiState() const { return ui_state_.Get(); }
  /** Build and publish the UI snapshot (on the calls owner: after-task hook, bind points). */
  void PublishUiState();

  /**
   * Owner objects. Other threads may hold these pointers only between the owner's bind edges (the
   * hub's UI thread: those edges wait on the owner) and only for durable-store reads.
   */
  CallSessionManager* Calls();
  CallLifecycle* Lifecycle();
  CallMediaKeyStore* MediaKeys() { return call_media_keys_.get(); }
  CallMediaEngine* MediaEngine() { return call_media_engine_.get(); }
  CallMediaSeat* MediaSeat() { return call_media_seat_.get(); }

  /**
   * Run `op` on the calls owner and wait (hub wiring that must touch the session manager, e.g.
   * billing store, media callbacks). No-op without sessions.
   */
  void RunOnOwner(const std::function<void(CallSessionManager&)>& op);

  /** Abort in-flight call-media Connect before joining the worker pool (app shutdown). */
  void AbortCallMediaForShutdown();
  /** True while CallMediaBridge Connect sequence is in flight (cheap for shutdown marks). */
  bool IsConnectWorkerInflight() const;
  /**
   * Bind lifecycle-derived port sets (lifecycle signaling, CSM hop/lifecycle, bridge arming/seat).
   * Calls owner only, at the bind points (BuildSessions / BindMediaProducts) — never per ring change
   * or per Lifecycle() query: the targets call these ports on the owner, so re-binding elsewhere
   * would swap a std::function while it runs (B49).
   */
  void EnsureCallLifecycleBound();
  /** Test-only: times EnsureCallLifecycleBound bound the port sets. */
  int LifecyclePortBindsForTest() const { return lifecycle_port_binds_.load(std::memory_order_relaxed); }
  void SetEphemeralListenDesire(bool want);
  /** N025 desire: CallLifecycle::WantEphemeralListen only. */
  bool WantEphemeralListen() const;
  bool HasActiveLocalCall();

  std::vector<std::string> LocalCallListenMultiaddrs() const;
  /** This node's mesh as the connectivity owner last published it (empty without mesh media). */
  std::shared_ptr<const MeshLocalView> LocalMeshView() const;
  void RegisterCallPeerListenMultiaddrs(const std::string& identity,
                                        const std::vector<std::string>& multiaddrs);

private:
  // Bodies of the hub-facing edges above; the public methods run them on the calls owner.
  Roe<void> InitializeStoresOnOwner(const std::string& profile_db_path, const std::string& profile_id);
  void BuildSessionsOnOwner(const CallStackDeps& deps);
  void OnMeshServicesStartedOnOwner();
  void BindTestMediaPathOnOwner(ICallMediaTransport* transport, IDialRegistry* dial,
                                ICircuitHopReach* circuit_reach);
  void PrepareForMeshStopOnOwner(const std::function<void()>& abort_inflight_circuit);
  void FinishMeshStopOnOwner();
  void DetachMeshMediaOnOwner();
  void RebindMeshMediaOnOwner();
  void ResetSessionsOnOwner();
  void AbortCallMediaForShutdownOnOwner();
  void RegisterCallPeerListenMultiaddrsOnOwner(const std::string& identity,
                                               const std::vector<std::string>& multiaddrs);
  void ReleaseOnOwner();
  /** Wake the hub's N025 listen sync (on UI) after publishing the listen desire. */
  void SyncHubEphemeralListen();

  MeshHost* mesh() const { return deps_.mesh ? deps_.mesh() : nullptr; }
  /** Mesh config snapshot (defaults when none is wired). */
  std::shared_ptr<const MeshConfig> mesh_config() const;
  void SyncMediaPlaneDeps();
  /** BindBridge + CSM SetMediaRelayDeps / SetDirectMediaPorts. */
  void BindMediaProducts();
  /** Calls' hooks on the shared mesh media (announce chosen R1, signaling punch). */
  void BindMeshMediaHooks();
  MeshMediaPlane* mesh_media() const { return deps_.mesh_media; }
  void BindSeatTeardown();
  CallLifecycleSignalingPorts MakeLifecycleSignalingPorts();
  CallHopArmingPorts MakeHopArmingPorts() const;
  CallDirectArmingPorts MakeDirectArmingPorts() const;
  CallSessionLifecyclePorts MakeSessionLifecyclePorts() const;
  CallDirectMediaPorts MakeDirectMediaPorts() const;
  CallDirectSeatPorts MakeDirectSeatPorts() const;
  CallTopologySeatPorts MakeTopologySeatPorts() const;

  CallStackDeps deps_;
  std::unique_ptr<CallSessionStore> call_session_store_;
  std::unique_ptr<CallMediaKeyStore> call_media_keys_;
  std::unique_ptr<CallMediaEngine> call_media_engine_;
  std::unique_ptr<CallMediaSeat> call_media_seat_;
  std::unique_ptr<CallSessionManager> call_sessions_;
  std::unique_ptr<CallLifecycle> call_lifecycle_;
  std::unique_ptr<CallMediaPlane> media_plane_;
  SharedPorts<CallUiState> ui_state_;
  CallsThread::HookId publish_hook_ = 0;
  std::atomic<int> lifecycle_port_binds_{0};
};

} // namespace pbr
