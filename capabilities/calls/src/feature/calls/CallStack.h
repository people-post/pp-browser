#pragma once

#include "lib/people/MeshNodeReputation.h"
#include "mesh/types/AddressDisclosure.h"
#include "common/MeshConfig.h"
#include "lib/media/MediaEngine.h"
#include "lib/calls/ICallSessionStore.h"
#include "lib/calls/CallStores.h"
#include "common/Error.h"
#include "common/Module.h"
#include "common/thread/IThreadCatalog.h"
#include "common/thread/IThreadTranscript.h"
#include "lib/people/IContactBook.h"
#include "common/directory/ILocalAccount.h"
#include "feature/calls/CallDeliveryPorts.h"
#include "lib/calls/CallPathPolicy.h"
#include "feature/calls/CallDirectPath.h"
#include "feature/calls/CallMediaPlane.h"
#include "lib/calls/CallMediaSeat.h"
#include "lib/calls/CallPathMobility.h"
#include "lib/calls/ICallMediaKeyStore.h"
#include "feature/calls/CallReads.h"
#include "feature/calls/CallSessionManager.h"
#include "common/Lifetime.h"
#include "feature/calls/CallUiState.h"
#include "feature/calls/CallsLoop.h"
#include "common/runtime/RuntimeOwnerExecutor.h"
#include "common/media/MediaSessionsOwner.h"
#include "feature/calls/SharedPorts.h"
#include "feature/calls/CallHopRelayDeps.h"
#include "common/chat/SystemControl.h"
#include "domain/call_media/ICallMediaTransport.h"
#include "mesh/host/MeshHost.h"

#include <unordered_map>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/**
 * The calls capability assembled (Wave 3 / V040): the owner thread and its event loop, the call stores, the media
 * engine, the device (CallDevice: the live calls, the seat, each call's media and the other parts), the session manager over it (the UI's intents, inbound call control) and `CallMediaPlane`
 * (the call_media transport) over the hub's borrowed `MeshConnectivity` / `MeshMediaRelay` (L015). The app's
 * `CallsParticipant` owns it and maps the hub's lifecycle onto it; CallStackDeps brings the mesh / config glue.
 * It publishes the calls' UI snapshot (`UiState`).
 *
 * CallUiBackend binds a CallStack& directly (not the hub) for call APIs.
 */
struct CallStackDeps {
  /** The chats a call starts from (a call's thread). */
  const IThreadCatalog* threads = nullptr;
  /** Where a call's summary lands when it cannot be sent. */
  IThreadTranscript* transcript = nullptr;
  IContactBook* contacts = nullptr;
  const ILocalAccount* account = nullptr;
  /** P001 initiation pricing between peers (the hub's; may be null). */
  IInitiationBillingBook* initiation_billing = nullptr;
  /** Delivery + dial registration; filled from MeshDeliveryOrchestrator without typing it here. */
  CallDeliveryPorts delivery;
  /** Where call control is bound as a system-control family (after the session manager is built; unbound with it). */
  SystemControlRegistry* system_controls = nullptr;

  /** Current MeshHost (null before StartMesh, reset on stop). */
  std::function<mesh::MeshHost*()> mesh;
  /** The live mesh config (role, caps, bootstrap, listen multiaddrs), from the app. */
  /** The hub's mesh config as last published (immutable snapshot — read from any owner). */
  std::function<std::shared_ptr<const MeshConfig>()> mesh_config;

  /** Cached mesh_node rows from Brief directory (n-dir). */
  std::function<std::vector<mesh::MeshDirectoryNode>()> list_directory_nodes;
  /** N032: this profile's verdicts on mesh nodes (the media hop ranking demotes what it distrusts). */
  std::function<std::shared_ptr<MeshNodeReputation>()> node_reputation;
  /** DHT peer_routing cache (n2-caps); directory-shaped nodes. */
  std::function<std::vector<mesh::MeshDirectoryNode>()> list_dht_nodes;
  /** Reachability seed probe — when false, hop policy skips org seed candidates. */
  std::function<bool()> seed_dial_ok;

  /** Hub-owned mesh glue the call stack cannot own. */
  std::function<void(const std::string& identity)> prefetch_peer_reachability;
  std::function<void()> sync_mobile_ephemeral_listen;
  /** A call's peer named its device: ask that device for the account's card (AccountCardService). */
  std::function<void(const std::string& account, const std::string& peer_id)> note_peer_device;

  /**
   * Neutral mesh objects — owned by the product hub, outlive the stack (L015): connectivity (dial,
   * reach, rendezvous parking, punch) and the media_relay client built on it. The owner calls
   * `DetachMeshMedia` before replacing their objects and `RebindMeshMedia` after.
   */
  mesh::MeshConnectivity* connectivity = nullptr;
  mesh::MeshMediaRelay* media_relay = nullptr;
  /**
   * Where calls hear which relay circuit reach chose (H011: announced to the call peer). Connectivity has one
   * such slot; when its owner keeps it (the app's mesh also remembers the relay, call-path-resilience K015) it
   * forwards each choice, on connectivity's lane, to the hook set here — empty stops it. None: calls take
   * connectivity's slot themselves (a bench, the call probe).
   */
  std::function<void(std::function<void(const std::string& relay_peer_id)> hook)> set_on_relay_chosen;
  /** Who may learn our address (projects/privacy T1; hub-owned, outlives the stack). Null = anyone. */
  const mesh::AddressDisclosureGate* address_disclosure = nullptr;
};

class CallStack : public Module {
public:
  CallStack();
  ~CallStack() override;

  /** Phase A (profile init, no mesh): take the call stores (the app's storage opens them), create the media engine. */
  Roe<void> InitializeStores(CallStores stores);
  /**
   * Phase A: build the device and the session manager against `deps`, wire providers, bind call state + media plane. Once:
   * while sessions exist (until ResetSessions) a further call keeps them — rebuilding would end
   * every live call. `deps` must therefore reach late-bound objects (delivery through the owner).
   */
  void BuildSessions(const CallStackDeps& deps);
  /** Phase B (mesh up, mesh media wired by the owner): start Amp call-media transport + bind. */
  void OnMeshServicesStarted();
  /**
   * Test-only: bind CallDirectPath without Amp mesh.
   * `transport` / `dial` / `circuit_reach` are non-owning; call after BuildSessions.
   */
  void BindTestMediaPath(ICallMediaTransport* transport, mesh::IDialRegistry* dial);
  void BindTestMediaPath(ICallMediaTransport* transport, mesh::IDialRegistry* dial,
                         mesh::ICircuitHopReach* circuit_reach);
  /** Test-only, group calls: also stand in for the hub's media_relay client (SoftMigrate / hop attach). */
  void BindTestMediaPath(ICallMediaTransport* transport, mesh::IDialRegistry* dial,
                         mesh::ICircuitHopReach* circuit_reach, mesh::IMediaRelayClient* relay);
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
  /** V058 3 (any thread): `identity` is no longer in the group (`nullopt`: the group ended); its call drops them. */
  void OnGroupMembersGone(const std::string& group_id, std::optional<std::string> identity);
  /** k6: re-read the mobility override (config `mesh.mobility` / `--mobility=`) — any thread. */
  void ReloadMobilityOverride();
  /** k6: this endpoint's mobility class as advertised in caps (any thread). */
  MobilityClass LocalMobility() const { return mobility_.LocalClass(); }
  /** k6: the path policy of a call (calls owner). */
  /** projects/privacy T3: who may call us (applied on the calls owner). */
  void SetInboundCallAudience(mesh::InboundAudience audience);
  /** CallSessionManager::SetOnInviteBlocked, kept here so a rebuilt session manager has it too. */
  void SetOnInviteBlocked(std::function<void(const BlockedCallInvite&)> callback);
  /** Mobility policy, restricted to the relay when the call's peer may not learn our address. */
  CallPathPolicy PathPolicyFor(const std::string& call_id) const;
  /** Before the owner replaces / drops mesh media objects: topology + bridge let go of them. */
  void DetachMeshMedia();
  /** After the owner rewired mesh media: rebind bridge + topology to the new objects. */
  void RebindMeshMedia();
  /** Reset the session manager and the device (Hub teardown ordering before p2p reset). */
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
  /** The calls' parts (the session manager drives them). Owner objects, like Calls(). */
  CallDevice* Device();
  /**
   * The calls' durable state, read-only (any thread; null before BuildSessions): what the screen and
   * the hub read — never the session manager.
   */
  const CallReads* Reads() const { return reads_.get(); }
  ICallMediaKeyStore* MediaKeys() { return call_media_keys_.get(); }
  /** The call sessions (null before InitializeStores). */
  ICallSessionStore* Sessions() { return call_session_store_.get(); }
  /** The call stores encrypted at rest (the profile's data key goes to them). */
  const std::vector<IDekConsumer*>& AtRestStores() const { return at_rest_; }
  MediaEngine* Engine() { return call_media_engine_.get(); }

  /**
   * Run `op` on the calls owner and wait (hub wiring that must touch the device's parts, e.g.
   * billing store, media callbacks). No-op without sessions.
   */
  void RunOnOwner(const std::function<void(CallDevice&)>& op);
  /** The UI edge's commands: queued behind the owner's events; `calls` is null without sessions. */
  void PostToSessions(std::function<void(CallSessionManager* calls)> run);

  /** Abort in-flight call-media Connect before joining the worker pool (app shutdown). */
  void AbortCallMediaForShutdown();
  /** True while CallDirectPath Connect sequence is in flight (cheap for shutdown marks). */
  bool IsConnectWorkerInflight() const;
  /**
   * Bind the device's call-state hook (chrome refresh, N025 listen). Calls owner only, at the bind
   * points (BuildSessions / BindMediaProducts) — never per ring change: the hook runs on the owner, so
   * re-binding elsewhere would swap a std::function while it runs (B49).
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
  std::shared_ptr<const mesh::MeshLocalView> LocalMeshView() const;
  void RegisterCallPeerListenMultiaddrs(const std::string& identity, const std::vector<std::string>& multiaddrs);

private:
  // Bodies of the hub-facing edges above; the public methods run them on the calls owner.
  Roe<void> InitializeStoresOnOwner(CallStores stores);
  void BuildSessionsOnOwner(const CallStackDeps& deps);
  void BindSessionOutbox();
  void BindCallControlInbound();
  /** What the sessions ask of this node's mesh and tell it (built once per device). */
  CallMeshGlue MakeMeshGlue();
  CallPeerCaps LocalPeerCaps() const;
  void BindSessionMeshReach();
  void OnMeshServicesStartedOnOwner();
  void BindTestMediaPathOnOwner(ICallMediaTransport* transport, mesh::IDialRegistry* dial,
                                mesh::ICircuitHopReach* circuit_reach, mesh::IMediaRelayClient* relay);
  void PrepareForMeshStopOnOwner(const std::function<void()>& abort_inflight_circuit);
  void FinishMeshStopOnOwner();
  void DetachMeshMediaOnOwner();
  void RebindMeshMediaOnOwner();
  void ResetSessionsOnOwner();
  void AbortCallMediaForShutdownOnOwner();
  void RegisterCallPeerListenMultiaddrsOnOwner(const std::string& identity, const std::vector<std::string>& multiaddrs);
  void ReleaseOnOwner();
  /** Wake the hub's N025 listen sync (on UI) after publishing the listen desire. */
  void SyncHubEphemeralListen();

  mesh::MeshHost* mesh() const { return deps_.mesh ? deps_.mesh() : nullptr; }
  /** Mesh config snapshot (defaults when none is wired). */
  std::shared_ptr<const MeshConfig> mesh_config() const;
  void SyncMediaPlaneDeps();
  /** The device's 1:1 deps (on the plane's transport) + its topology relay deps. */
  void BindMediaProducts();
  /** Hear (or, empty, stop hearing) which relay circuit reach chose: through the owner when it keeps the slot. */
  void SetRelayChosenHook(std::function<void(const std::string& relay_peer_id)> hook);
  /** Calls' hooks on the shared connectivity (announce chosen R1, signaling punch). */
  void BindMeshMediaHooks();
  mesh::MeshConnectivity* connectivity() const { return deps_.connectivity; }
  mesh::MeshMediaRelay* media_relay() const { return deps_.media_relay; }
  /** What the calls show may have changed: refresh chrome, wake N025 listen when its desire flips. */
  void OnCallStateChangedOnOwner();

  /** The calls owner's executor: every call object gets it from here (THREADING.md § Owner runners). */
  /** The calls owner: the media-sessions owner thread (UI without one); hooks publish the UI state. */
  RuntimeOwnerExecutor executor_{kMediaSessionsOwner};
  /** The calls owner's event queue: every input and delayed event goes through Dispatch. */
  CallsLoop loop_{executor_, [this](CallStackEvent& event) { Dispatch(event); }};
  CallStackDeps deps_;
  /** Written by the owner of the settings (UI), read when the calls owner rebuilds its session manager. */
  std::atomic<mesh::InboundAudience> inbound_call_audience_{mesh::InboundAudience::Everyone};
  /** The GUI's listener for dropped invites (owner thread). */
  std::function<void(const BlockedCallInvite&)> on_invite_blocked_;
  std::unique_ptr<ICallSessionStore> call_session_store_;
  std::unique_ptr<ICallMediaKeyStore> call_media_keys_;
  std::vector<IDekConsumer*> at_rest_;
  std::unique_ptr<MediaEngine> call_media_engine_;
  /** Before call_sessions_, which reads through it. */
  std::unique_ptr<CallReads> reads_;
  /** The parts (before the session manager, which drives them). */
  std::unique_ptr<CallDevice> call_device_;
  std::unique_ptr<CallSessionManager> call_sessions_;
  std::unique_ptr<CallMediaPlane> media_plane_;
  /** k6: this device's and each call peer's mobility → the call's path policy. */
  CallPathMobility mobility_;
  /** The device's last reported attachment (owner), for the connect lines of the Diag channel. */
  MobilityAttachment last_attachment_;
  bool attachment_known_ = false;
  /** H013: the relays the answerer of `call_id` advertised in its CallAccept (calls owner). */
  struct PeerParkedRelays {
    std::string call_id;
    std::vector<std::string> relays;
  };
  PeerParkedRelays peer_parked_;
  CallsWakeSlot mobility_wake_{loop_, calls_event::MobilityWake{}};
  /** Names the current device: its parts' events carry it (a rebuilt one drops the old ones). */
  uint64_t sessions_generation_ = 0;
  void ApplyMobilityOverrideOnOwner();
  /** Route one event to the child it is for (owner). */
  void Dispatch(CallStackEvent& event);
  /** After a mobility event: re-arm its wake; a flipped class re-plans the active call. */
  void AfterMobilityEvent(bool local_class_changed);
  void NotifyPathPolicyChangedOnOwner(const std::string& call_id);
  SharedPorts<CallUiState> ui_state_;
  RuntimeOwnerExecutor::HookId publish_hook_ = 0;
  std::atomic<int> call_state_binds_{0};
  std::function<void()> on_chrome_refresh_;
  bool want_ephemeral_listen_ = false;
  /** Chrome refreshes posted to UI drop once the stack is gone. */
  pp::Lifetime chrome_self_;
  /** BindTestMediaPath's stand-in for the mesh's io lane (fake dial registries); one per stack. */
  std::shared_ptr<pp::SerialExecutor> test_lane_;
};

} // namespace pbr
