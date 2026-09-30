#pragma once

#include "common/privacy/AddressDisclosure.h"
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
#include "domain/messaging/CallPathPolicy.h"
#include "feature/calls/CallMediaBridge.h"
#include "feature/calls/CallMediaPlane.h"
#include "feature/calls/CallMediaSeat.h"
#include "feature/calls/CallPathMobility.h"
#include "domain/messaging/CallMediaKeyStore.h"
#include "feature/calls/CallSessionManager.h"
#include "foundation/runtime/DeferredSelf.h"
#include "feature/calls/CallUiState.h"
#include "feature/calls/CallsExecutor.h"
#include "feature/calls/CallsLoop.h"
#include "feature/calls/CallsThread.h"
#include "feature/calls/SharedPorts.h"
#include "feature/calls/CallTopologyRelayDeps.h"
#include "feature/calls/CallControlInboundPorts.h"
#include "domain/mesh/l4/call_media/ICallMediaTransport.h"
#include "domain/mesh/host/MeshHost.h"

#include <unordered_map>
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
 * (call_media transport) over the hub's borrowed `MeshConnectivity` / `MeshMediaRelay` (L015). Hub owns `unique_ptr<CallStack>`, forwards
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
   * Neutral mesh objects — owned by the product hub, outlive the stack (L015): connectivity (dial,
   * reach, rendezvous parking, punch) and the media_relay client built on it. The owner calls
   * `DetachMeshMedia` before replacing their objects and `RebindMeshMedia` after.
   */
  MeshConnectivity* connectivity = nullptr;
  MeshMediaRelay* media_relay = nullptr;
  /** Who may learn our address (projects/privacy T1; hub-owned, outlives the stack). Null = anyone. */
  const AddressDisclosureGate* address_disclosure = nullptr;
};

class CallStack : public Module {
public:
  CallStack();
  ~CallStack() override;

  /** Phase A (profile init, no mesh): create call session store, media key store, media engine. */
  Roe<void> InitializeStores(const std::string& profile_db_path, const std::string& profile_id);
  /** Phase A: build CSM against current p2p, wire providers, bind call state + media plane. */
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
  /** Test-only, group calls: also stand in for the hub's media_relay client (SoftMigrate / hop attach). */
  void BindTestMediaPath(ICallMediaTransport* transport, IDialRegistry* dial, ICircuitHopReach* circuit_reach,
                         IMediaRelayClient* relay);
  /** Teardown before mesh Stop: clear bindings, PrepareForTeardown; abort circuit via callback. */
  void PrepareForMeshStop(const std::function<void()>& abort_inflight_circuit);
  /** Teardown after mesh Stop: drop the bridge and the call-media transport. */
  void FinishMeshStop();
  /**
   * k5 / k6 (any thread): the device's attachment. Feeds the mobility class (`changed` = a network
   * change, counted as churn); `moved` = the mesh moved (re-validate paths: re-anchor / re-punch).
   */
  void OnLocalNetwork(const MobilityAttachment& attachment, bool changed, bool moved);
  /** k6 (any thread): our observed public address moved without a local change (NAT rebind). */
  void OnObservedAddressChanged();
  /** k6: re-read the mobility override (config `mesh.mobility` / `--mobility=`) — any thread. */
  void ReloadMobilityOverride();
  /** k6: this endpoint's mobility class as advertised in caps (any thread). */
  MobilityClass LocalMobility() const { return mobility_.LocalClass(); }
  /** k6: the path policy of a call (calls owner). */
  /** projects/privacy T3: who may call us (applied on the calls owner). */
  void SetInboundCallAudience(InboundAudience audience);
  /** Mobility policy, restricted to the relay when the call's peer may not learn our address. */
  CallPathPolicy PathPolicyFor(const std::string& call_id) const;
  /** Before the owner replaces / drops mesh media objects: topology + bridge let go of them. */
  void DetachMeshMedia();
  /** After the owner rewired mesh media: rebind bridge + topology to the new objects. */
  void RebindMeshMedia();
  /** Reset the call session manager (Hub teardown ordering before p2p reset). */
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
  CallMediaKeyStore* MediaKeys() { return call_media_keys_.get(); }
  CallMediaEngine* MediaEngine() { return call_media_engine_.get(); }

  /**
   * Run `op` on the calls owner and wait (hub wiring that must touch the session manager, e.g.
   * billing store, media callbacks). No-op without sessions.
   */
  void RunOnOwner(const std::function<void(CallSessionManager&)>& op);
  /** The UI edge's commands: queued behind the owner's events; `calls` is null without sessions. */
  void PostToSessions(std::function<void(CallSessionManager* calls)> run);

  /** Abort in-flight call-media Connect before joining the worker pool (app shutdown). */
  void AbortCallMediaForShutdown();
  /** True while CallMediaBridge Connect sequence is in flight (cheap for shutdown marks). */
  bool IsConnectWorkerInflight() const;
  /**
   * Bind the call-state port sets (CSM state-change hook, bridge arming / seat). Calls owner only, at
   * the bind points (BuildSessions / BindMediaProducts) — never per ring change: the targets call
   * these ports on the owner, so re-binding elsewhere would swap a std::function while it runs (B49).
   */
  void BindCallState();
  /** Test-only: times BindCallState bound the port sets. */
  int CallStateBindsForTest() const { return call_state_binds_.load(std::memory_order_relaxed); }
  /** The GUI's chrome refresh: runs on UI after anything that can change what the calls show. */
  void SetOnChromeRefresh(std::function<void()> fn);
  /** N025 desire: a call is shown (ringing, calling or in a call). */
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
  void BindSessionOutbox();
  void BindCallControlInbound();
  void BindSessionProviders();
  CallPeerCaps LocalPeerCaps() const;
  void BindSessionMeshReach();
  void OnMeshServicesStartedOnOwner();
  void BindTestMediaPathOnOwner(ICallMediaTransport* transport, IDialRegistry* dial,
                                ICircuitHopReach* circuit_reach, IMediaRelayClient* relay);
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
  /** The session manager's 1:1 path (on the plane's transport) + its topology relay deps. */
  void BindMediaProducts();
  /** Calls' hooks on the shared connectivity (announce chosen R1, signaling punch). */
  void BindMeshMediaHooks();
  MeshConnectivity* connectivity() const { return deps_.connectivity; }
  MeshMediaRelay* media_relay() const { return deps_.media_relay; }
  /** What the calls show may have changed: refresh chrome, wake N025 listen when its desire flips. */
  void OnCallStateChangedOnOwner();

  /** The calls owner's executor: every call object gets it from here (THREADING.md § Owner runners). */
  OwnerExecutor& executor_ = CallsOwnerExecutor();
  /** The calls owner's event queue: every input and delayed event goes through Dispatch. */
  CallsLoop loop_{executor_, [this](CallStackEvent& event) { Dispatch(event); }};
  CallStackDeps deps_;
  InboundAudience inbound_call_audience_ = InboundAudience::Everyone;
  std::unique_ptr<CallSessionStore> call_session_store_;
  std::unique_ptr<CallMediaKeyStore> call_media_keys_;
  std::unique_ptr<CallMediaEngine> call_media_engine_;
  std::unique_ptr<CallSessionManager> call_sessions_;
  std::unique_ptr<CallMediaPlane> media_plane_;
  /** k6: this device's and each call peer's mobility → the call's path policy. */
  CallPathMobility mobility_;
  CallsWakeSlot mobility_wake_{loop_, calls_event::MobilityWake{}};
  /** Names the current session manager: its events carry it (a rebuilt one drops the old ones). */
  uint64_t sessions_generation_ = 0;
  void ApplyMobilityOverrideOnOwner();
  /** Route one event to the child it is for (owner). */
  void Dispatch(CallStackEvent& event);
  /** After a mobility event: re-arm its wake; a flipped class re-plans the active call. */
  void AfterMobilityEvent(bool local_class_changed);
  void NotifyPathPolicyChangedOnOwner(const std::string& call_id);
  SharedPorts<CallUiState> ui_state_;
  CallsThread::HookId publish_hook_ = 0;
  std::atomic<int> call_state_binds_{0};
  std::function<void()> on_chrome_refresh_;
  bool want_ephemeral_listen_ = false;
  /** Chrome refreshes posted to UI drop once the stack is gone. */
  DeferredSelf chrome_self_;
};

} // namespace pbr
