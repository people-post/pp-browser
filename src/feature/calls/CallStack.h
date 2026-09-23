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
#include "feature/calls/CallTopologyRelayDeps.h"
#include "feature/calls/CallControlInboundPorts.h"
#include "domain/mesh/l4/call_media/ICallMediaTransport.h"
#include "domain/mesh/host/MeshHost.h"

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
 * (Amp transport + dial/relay/hop + bridge). Hub owns `unique_ptr<CallStack>`, forwards
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
  std::function<const AppConfig&()> config;

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
};

class CallStack : public Module {
public:
  CallStack();
  ~CallStack() override;

  /** Phase A (profile init, no mesh): create call session store, media key store, media engine. */
  Roe<void> InitializeStores(const std::string& profile_db_path, const std::string& profile_id);
  /** Phase A: build CSM against current p2p, wire providers, bind lifecycle + media plane. */
  void BuildSessions(const CallStackDeps& deps);
  /** Phase B (mesh up): start Amp call-media transport + Wire media plane. */
  void OnMeshServicesStarted();
  /**
   * Test-only: bind CallMediaBridge without Amp mesh.
   * `transport` / `dial` are non-owning; call after BuildSessions. Re-runs Wire.
   */
  void BindTestMediaPath(ICallMediaTransport* transport, IDialRegistry* dial);
  void BindTestMediaPath(ICallMediaTransport* transport, IDialRegistry* dial,
                         ICircuitHopReach* circuit_reach);
  /** Teardown before mesh Stop: clear bindings, PrepareForTeardown; abort circuit via callback. */
  void PrepareForMeshStop(const std::function<void()>& abort_inflight_circuit);
  /** Teardown after mesh Stop: reset media plane mesh objects. */
  void FinishMeshStop();
  /** Reset call session manager + lifecycle (Hub teardown ordering before p2p reset). */
  void ResetSessions();
  /** Final teardown: reset media engine / key store / session store. */
  void Shutdown();

  CallSessionManager* Calls();
  CallLifecycle* Lifecycle();
  CallMediaKeyStore* MediaKeys() { return call_media_keys_.get(); }
  CallMediaEngine* MediaEngine() { return call_media_engine_.get(); }
  CallMediaSeat* MediaSeat() { return call_media_seat_.get(); }

  /** Abort in-flight call-media Connect before joining the worker pool (app shutdown). */
  void AbortCallMediaForShutdown();
  /** True while CallMediaBridge Connect sequence is in flight (cheap for shutdown marks). */
  bool IsConnectWorkerInflight() const;
  void WireMediaRelayDeps();
  void EnsureCallLifecycleBound();
  void SetEphemeralListenDesire(bool want);
  /** N025 desire: CallLifecycle::WantEphemeralListen only. */
  bool WantEphemeralListen() const;
  bool HasActiveLocalCall();
  /** Force relay client + dial registry rebuild on capability change (RefreshMeshCapabilities). */
  void ResetRelayClients();

  std::vector<std::string> LocalCallListenMultiaddrs() const;
  void RegisterCallPeerListenMultiaddrs(const std::string& identity,
                                        const std::vector<std::string>& multiaddrs);
  Roe<void> TryEnsureCircuitHopReachable(const std::string& hop_peer_id);
  Roe<void> TryEnsureCallMediaReachable(const std::string& peer_key);
  void TryEnsureCallMediaReachableAsync(const std::string& peer_key,
                                        std::function<void(Roe<void>)> on_done);
  /** L3.25c: upgrade call-media / hop from circuit R1 to direct via ACP. */
  Roe<void> TryUpgradeCallMediaToDirect(const std::string& peer_key);
  /**
   * Register bootstrap ADP endpoints and EnsureAssociation (async, fire-and-forget).
   * Call before punch/circuit so org seed holds Sessions for double-NAT splice.
   */
  void WarmBootstrapSeedSessions();
  /** Answerer/offerer: StartReserve on dialable bootstrap seeds after warm. */
  void ReserveOnBootstrapSeeds();

private:
  MeshHost* mesh() const { return deps_.mesh ? deps_.mesh() : nullptr; }
  const AppConfig& config() const;
  void SyncMediaPlaneDeps();
  /** After plane Wire: BindBridge + CSM SetMediaRelayDeps / SetDirectMediaPorts. */
  void BindMediaProducts();
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
};

} // namespace pbr
