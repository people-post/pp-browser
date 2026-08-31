#pragma once

#include "base/data/Config.h"
#include "base/media/CallMediaEngine.h"
#include "base/messaging/CallSessionStore.h"
#include "common/Error.h"
#include "common/Module.h"
#include "feature/messaging/CallLibp2pMediaBridge.h"
#include "feature/messaging/CallLifecycle.h"
#include "feature/messaging/CallMediaKeyStore.h"
#include "feature/messaging/CallSessionManager.h"
#include "feature/messaging/CallTopologyRelayDeps.h"
#include "base/p2p/CallMediaAmpTransport.h"
#include "base/p2p/CallMediaDirectService.h"
#include "base/p2p/CircuitRelayService.h"
#include "base/p2p/ICallMediaTransport.h"
#include "base/p2p/Libp2pHost.h"
#include "base/p2p/Libp2pWorker.h"
#include "base/p2p/MediaRelayService.h"
#include "base/p2p/MeshHost.h"
#include "base/p2p/NodeRuntime.h"
#include "base/p2p/PeerSessionManager.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class ContactsStore;
class IdentityStore;
class IThreadStore;
class P2pMessagingService;
class SqlitePskSessionStore;

/**
 * Wave 3: call media / session / lifecycle stack extracted from MessagingHub.
 *
 * Owns the call-media unique_ptrs (CSM + media engine/keys/store + libp2p media bridge +
 * CallMediaDirect + media_relay client + dial registry + circuit hop reach + lifecycle) and the
 * call-scoped reachability helpers. The Hub owns `unique_ptr<CallStack>`, forwards
 * `Calls()`/`Lifecycle()`, and injects mesh/config/mDNS glue through CallStackDeps so this stack
 * stays free of NodeRuntime / reachability / admission-policy ownership.
 *
 * CallUiBackend binds a CallStack& directly (not the Hub) for call APIs.
 */
struct CallStackDeps {
  IThreadStore* store = nullptr;
  ContactsStore* contacts = nullptr;
  IdentityStore* identity = nullptr;
  SqlitePskSessionStore* psk = nullptr;
  /** Recreated on stack rebuild (BuildMessagingStack); passed fresh each BuildSessions. */
  P2pMessagingService* p2p = nullptr;

  /** Current MeshHost (null before StartLibp2p, reset on stop). */
  std::function<MeshHost*()> mesh;
  /** Live AppConfig (libp2p role / caps / bootstrap / listen multiaddr). */
  std::function<const AppConfig&()> config;

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
  /** Phase A: build CSM against current p2p, wire providers, bind lifecycle + relay deps. */
  void BuildSessions(const CallStackDeps& deps);
  /** Phase B (mesh up): create/start call-media transport (Amp or libp2p) + WireMediaRelayDeps. */
  void OnMeshServicesStarted();
  /** Teardown before mesh Stop: clear bindings, PrepareForTeardown; abort circuit via callback. */
  void PrepareForMeshStop(const std::function<void()>& abort_inflight_circuit);
  /** Teardown after mesh Stop: reset libp2p media bridge / call-media transport / dial registry. */
  void FinishMeshStop();
  /** Reset call session manager + lifecycle (Hub teardown ordering before p2p reset). */
  void ResetSessions();
  /** Final teardown: reset media engine / key store / session store. */
  void Shutdown();

  CallSessionManager* Calls();
  CallLifecycle* Lifecycle();
  CallMediaKeyStore* MediaKeys() { return call_media_keys_.get(); }
  CallMediaEngine* MediaEngine() { return call_media_engine_.get(); }

  /** Abort in-flight call-media Connect before joining the worker pool (app shutdown). */
  void AbortCallMediaForShutdown();
  void WireMediaRelayDeps();
  void EnsureCallLifecycleBound();
  void SetEphemeralListenDesire(bool want);
  /** N025 desire: lifecycle wants listen OR an explicit desire is set. */
  bool WantEphemeralListen() const;
  bool HasActiveLocalCall();
  /** Force relay client + dial registry rebuild on capability change (RefreshMeshCapabilities). */
  void ResetRelayClients();

  std::vector<std::string> LocalCallListenMultiaddrs() const;
  void RegisterCallPeerListenMultiaddrs(const std::string& identity,
                                        const std::vector<std::string>& multiaddrs);
  Roe<void> TryEnsureCircuitHopReachable(const std::string& hop_peer_id);
  Roe<void> TryEnsureCallMediaReachable(const std::string& peer_key);

private:
  std::vector<std::string> CollectDialableCircuitRelayIds(const std::string& exclude_peer_id) const;

  MeshHost* mesh() const { return deps_.mesh ? deps_.mesh() : nullptr; }
  NodeRuntime* Runtime() const;
  PeerSessionManager* Sessions() const;
  MediaRelayService* MediaRelay() const;
  CircuitRelayService* CircuitRelay() const;
  const AppConfig& config() const;

  CallStackDeps deps_;
  std::unique_ptr<CallSessionStore> call_session_store_;
  std::unique_ptr<CallMediaKeyStore> call_media_keys_;
  std::unique_ptr<CallMediaEngine> call_media_engine_;
  std::unique_ptr<CallSessionManager> call_sessions_;
  std::unique_ptr<CallLibp2pMediaBridge> call_libp2p_bridge_;
  std::unique_ptr<CallLifecycle> call_lifecycle_;
  /** CallSessionManager the bridge was last built against (detect stack rebuild). */
  CallSessionManager* libp2p_bridge_bound_sessions_ = nullptr;
  std::unique_ptr<MediaRelayServiceClient> media_relay_client_;
  std::unique_ptr<PeerSessionDialRegistry> dial_registry_;
  std::unique_ptr<CircuitHopReachClient> circuit_hop_reach_;
  /** Exactly one of these is set when mesh is up ([A020] single call-media entry). */
  std::unique_ptr<CallMediaDirectService> call_media_direct_;
  std::unique_ptr<CallMediaAmpTransport> call_media_amp_;
  /** Lifecycle-driven N025 desire (mirrors old MessagingHub::ephemeral_listen_desired_). */
  bool ephemeral_listen_desired_ = false;

  ICallMediaTransport* CallMediaTransport();
};

} // namespace pbr
