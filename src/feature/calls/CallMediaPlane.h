#pragma once

#include "foundation/data/Config.h"
#include "domain/media/CallMediaEngine.h"
#include "domain/messaging/CallSessionStore.h"
#include "domain/messaging/CallMediaKeyStore.h"
#include "common/Error.h"
#include "common/Module.h"
#include "domain/people/ContactsStore.h"
#include "feature/calls/AmpCircuitHopReach.h"
#include "feature/calls/AmpMediaRelayClient.h"
#include "feature/calls/CallMediaBridge.h"
#include "feature/calls/CallMediaSeat.h"
#include "feature/calls/CallMediaHost.h"
#include "feature/calls/CallTopologyController.h"
#include "feature/calls/CallTopologyRelayDeps.h"
#include "domain/mesh/l4/call_media/CallMediaAmpTransport.h"
#include "domain/mesh/l4/call_media/ICallMediaTransport.h"
#include "domain/mesh/host/MeshHost.h"
#include "common/directory/MeshHopTypes.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** Peer listen multiaddrs + LAN-confirmed PeerIds for PreferLocal / SoftMigrate (V035). */
struct CallDialBook {
  std::unordered_map<std::string, std::vector<std::string>> peer_listen_mas;
  std::unordered_set<std::string> lan_confirmed_peers;
};

/**
 * Mesh-media plane under CallStack (V040): Amp transport, dial registry, media_relay client,
 * circuit hop reach, CallMediaBridge ownership, and dial book.
 *
 * Does **not** hold standing pointers to CallStack siblings (CSM / stores / seat / lifecycle).
 * Stack passes those only into `BindBridge` and fills deps callbacks. Keep `Wire` shallow
 * (AGENTS.md function-complexity convention).
 */
struct CallMediaPlaneDeps {
  ContactsStore* contacts = nullptr;
  std::function<MeshHost*()> mesh;
  std::function<const AppConfig&()> config;
  std::function<std::vector<MeshDirectoryNode>()> list_directory_nodes;
  std::function<std::vector<MeshDirectoryNode>()> list_dht_nodes;
  std::function<bool()> seed_dial_ok;
  std::function<void(const std::string& peer_id)> note_lan_mdns_peer_id;
  std::function<void(const std::string& identity, const std::string& multiaddr)>
      register_peer_direct_endpoint;
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

  /** Phase B: create/start Amp call-media transport + Wire. */
  void OnMeshStarted();
  /** Mesh clients only (relay / dial / hop). Stack follows with BindBridge + CSM install. */
  void Wire();
  void BindTestMediaPath(ICallMediaTransport* transport, IDialRegistry* dial);
  /**
   * Test / hard-lab product-stack: Amp transport + dial + optional circuit reach without MeshHost.
   * When `circuit_reach` is non-null it is used for Bridge Ensure (not owned; must outlive Wire).
   */
  void BindTestMediaPath(ICallMediaTransport* transport, IDialRegistry* dial,
                         ICircuitHopReach* circuit_reach);
  /**
   * Construct or refresh CallMediaBridge from stack-owned ingredients.
   * Rebuilds when `sessions_key` changes; otherwise updates reach deps + seat/lifecycle.
   */
  void BindBridge(const CallMediaBridgeBindArgs& args);
  CallTopologyController::MediaRelayDeps BuildMediaRelayDeps() const;

  /** Media half of mesh stop (bridge PrepareForTeardown + transport stop + relay reset). */
  void PrepareForMeshStop(const std::function<void()>& abort_inflight_circuit);
  void FinishMeshStop();
  void ResetRelayClients();
  /** Group SFU: close media_relay before LeaveCall joins capture. */
  void DetachRelayClient();
  /** Bridge PrepareForTeardown(0) + transport Detach (after LeaveCall). */
  void AbortBridgeAndTransport();
  bool IsConnectWorkerInflight() const;
  /** Reset all plane-owned objects (CallStack::Shutdown). */
  void Clear();

  CallMediaBridge* Bridge() { return call_media_bridge_.get(); }
  void StopMeshMedia(const std::string& call_id);

  void RegisterCallPeerListenMultiaddrs(const std::string& identity,
                                        const std::vector<std::string>& multiaddrs);
  Roe<void> TryEnsureCircuitHopReachable(const std::string& hop_peer_id);
  Roe<void> TryEnsureCallMediaReachable(const std::string& peer_key);
  Roe<void> TryUpgradeCallMediaToDirect(const std::string& peer_key);
  void WarmBootstrapSeedSessions();
  void ReserveOnBootstrapSeeds();
  /** Connectivity: kick warm/reserve so this peer is ServeDial-reachable via org hops. */
  void EnsureCircuitReady() { ReserveOnBootstrapSeeds(); }
  /**
   * H011 L3.1c: StartReserve a specific R1 PeerId (late park after dialer chose / announce).
   * No-op when peer empty or circuit tunnel not started.
   */
  void PreferLateReserve(const std::string& relay_peer_id);
  /**
   * Kick warm+reserve and invoke on_done(true) once any bootstrap/directory seed is Connected,
   * or on_done(false) at timeout (H010: answerer must be parkable before offerer StartBridge).
   */
  void EnsureBootstrapSeedParkedAsync(std::function<void(bool parked)> on_done, int timeout_ms = 12000);
  /**
   * Block until circuit-ready (any bootstrap seed Connected) or timeout.
   * Safe on AcceptInvite worker — MeshPump / coordinator drive progress.
   */
  bool AwaitCircuitReady(int timeout_ms = 12000);

private:
  using IoPump = std::function<void()>;
  using IoPost = std::function<void(std::function<void()>)>;

  MeshHost* mesh() const { return deps_.mesh ? deps_.mesh() : nullptr; }
  const AppConfig& config() const;
  ICallMediaTransport* Transport();
  IDialRegistry* ActiveDial() const;
  ICircuitHopReach* ActiveCircuitReach() const;

  /** True when Amp media_relay coordinator is started. */
  bool WireMediaRelayClient(MeshHost* m, const IoPump& io_pump, const IoPost& post_io);
  void WireDialRegistry(MeshHost* m, bool use_amp_relay, const IoPost& post_io);
  void WireCircuitHopReach(MeshHost* m, bool use_amp_relay, const IoPump& io_pump,
                           const IoPost& post_io);

  void TryColdPunchAsync(MeshHost* m, IChatPeerLinks* punch_links, const std::string& target_peer_id,
                         std::function<void(Roe<void>)> on_done);
  void TryUpgradePunchAsync(MeshHost* m, const std::string& introducer_peer_key,
                            const std::string& target_peer_id,
                            std::function<void(Roe<void>)> on_done);

  void MergeDialBookListenAddrs(const std::string& identity, const std::vector<std::string>& ranked);
  void RegisterOneListenMultiaddr(const std::string& identity, const std::string& ma);
  static std::string PeerIdFromListenMultiaddr(const std::string& ma);

  std::vector<std::string> CollectDialableCircuitRelayIds(const std::string& exclude_peer_id) const;
  /**
   * H011 shared rendezvous surface: same BuildCircuitHopList as dialer StartBridge collect.
   * `exclude_peer_id` drops the call target. Does not filter dialability (reserve registers MAs).
   */
  std::vector<MeshHopCandidate> BuildCircuitRendezvousCandidates(
      const std::string& exclude_peer_id = {}) const;
  bool PeerLanConfirmed(const std::string& peer_id) const;

  void WarmBootstrapSeedSessionsOnIo();
  /** H011: StartReserve over shared rendezvous surface (not seeds-only). */
  void ReserveOnBootstrapSeedsOnIo();
  void PreferLateReserveOnIo(const std::string& relay_peer_id);
  bool AnyBootstrapSeedConnectedOnIo() const;
  std::vector<std::string> EffectiveBootstrapSeedPeerIds() const;

  CallMediaPlaneDeps deps_;
  CallDialBook dial_book_;

  std::unique_ptr<CallMediaBridge> call_media_bridge_;
  const void* media_bridge_bound_sessions_key_ = nullptr;
  std::unique_ptr<IMediaRelayClient> media_relay_client_;
  std::unique_ptr<PeerSessionDialRegistry> dial_registry_;
  std::unique_ptr<ICircuitHopReach> circuit_hop_reach_;
  /** H011 L3.1b/c: last chosen / announced R1 PeerId for park sticky + late-reserve. */
  std::string chosen_circuit_r1_;
  std::unique_ptr<CallMediaAmpTransport> call_media_amp_;
  ICallMediaTransport* test_media_transport_ = nullptr;
  IDialRegistry* test_dial_ = nullptr;
  ICircuitHopReach* test_circuit_reach_ = nullptr;
};

} // namespace pbr
