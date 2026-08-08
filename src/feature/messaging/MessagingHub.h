#pragma once

#include "base/data/Config.h"
#include "base/data/Libp2pRole.h"
#include "base/data/SessionStore.h"
#include "base/data/UserPreferences.h"
#include "base/messaging/GroupTypes.h"
#include "base/people/ContactsStore.h"
#include "base/people/IdentityStore.h"
#include "base/people/ProfileIdentityView.h"
#include "common/Module.h"
#include "feature/messaging/ContactActionDispatcher.h"
#include "feature/messaging/DirectoryShadowCache.h"
#include "feature/messaging/InboxController.h"
#include "feature/messaging/PeerDisplayResolver.h"
#include "base/messaging/PeerKemKeyStore.h"
#include "base/messaging/GroupRosterStore.h"
#include "base/messaging/PeerSigningKeyStore.h"
#include "feature/messaging/GroupInviteGate.h"
#include "feature/messaging/GroupMembershipService.h"
#include "base/messaging/SqliteThreadStore.h"
#include "feature/messaging/CallStack.h"
#include "feature/messaging/MessageRouter.h"
#include "feature/messaging/P2pMessagingService.h"
#include "base/net/ServiceClientsImpl.h"
#include "base/net/IPushDeviceClient.h"
#include "libp2p/integration/host/Libp2pHost.h"
#include "libp2p/integration/host/DialBackService.h"
#include "libp2p/integration/host/CircuitRelayService.h"
#include "libp2p/integration/host/LanMdnsDiscovery.h"
#include "libp2p/integration/host/MediaRelayService.h"
#include "libp2p/integration/host/Reachability.h"
#include "libp2p/integration/host/ReachabilityService.h"
#include "libp2p/integration/host/MeshHost.h"
#include "libp2p/integration/host/NodeRuntime.h"
#include "libp2p/integration/host/PeerSessionManager.h"
#include "base/people/MeshHopPolicy.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>

namespace pbr {

class AgentSession;
class ProfileSecretsService;
class RelayDirectoryKemKeyResolver;
class RelayDirectorySigningKeyResolver;
class SqlitePskSessionStore;

/**
 * App-only messaging composition root (also known as MessagingCore).
 *
 * Ownership planes:
 * - **MessagingCore (this class):** stores, HTTP Brief clients, inbox/P2P/groups/router,
 *   LAN mDNS, policy timers, N025 ephemeral-listen *execution* glue.
 * - **MeshHost (`mesh_`):** shared with headless `pp-node` — NodeRuntime + dial-back +
 *   circuit/media relay + reachability (`libp2p/integration/host/MeshHost`).
 * - **CallStack (`call_stack_`):** call media, CSM, lifecycle, libp2p media bridge,
 *   CallMediaDirect, dial/hop helpers.
 *
 * UI/tools talk through MessagingFacade / CallUiBackend / ports — not Hub accessors.
 * Profile secrets + identity DEK registration remain injected (`BindSecrets`).
 */
class MessagingHub : public Module {
public:
  /** Hot-reloadable network / mesh slice projected from AppConfig. */
  struct NetworkConfig {
    ServiceEndpointConfig relay;
    ServiceEndpointConfig directory;
    ServiceEndpointConfig registration;
    bool node_enabled = true;
    bool circuit_relay = false;
    bool media_relay = true;
    bool prefer_contacts_for_routing = true;

    bool operator==(const NetworkConfig& other) const {
      return relay.base_url == other.relay.base_url && directory.base_url == other.directory.base_url &&
             registration.base_url == other.registration.base_url && node_enabled == other.node_enabled &&
             circuit_relay == other.circuit_relay && media_relay == other.media_relay &&
             prefer_contacts_for_routing == other.prefer_contacts_for_routing;
    }
    bool operator!=(const NetworkConfig& other) const { return !(*this == other); }
  };

  /** Inbound group-invite policy projected from ProfilePreferences. */
  struct PolicyPrefs {
    GroupInvitePolicy group_invite_policy = GroupInvitePolicy::ContactsOnly;

    bool operator==(const PolicyPrefs& other) const {
      return group_invite_policy == other.group_invite_policy;
    }
    bool operator!=(const PolicyPrefs& other) const { return !(*this == other); }
  };

  /** Push / OS notification preference projected from ProfilePreferences. */
  struct NotificationPrefs {
    bool show_notifications = true;

    bool operator==(const NotificationPrefs& other) const {
      return show_notifications == other.show_notifications;
    }
    bool operator!=(const NotificationPrefs& other) const { return !(*this == other); }
  };

  static NetworkConfig ProjectNetwork(const AppConfig& config);
  static PolicyPrefs ProjectPolicy(const ProfilePreferences& prefs);
  static NotificationPrefs ProjectNotifications(const ProfilePreferences& prefs);

  MessagingHub();
  ~MessagingHub();

  /** Core store/inbox/AI — profile vault unlock is separate (ProfileSecretsService). */
  Roe<void> Initialize(const AppConfig& config, const std::string& profile_data_dir);
  Roe<void> Reinitialize(const AppConfig& config, const std::string& profile_data_dir);
  void Shutdown();
  bool IsInitialized() const { return initialized_; }

  /**
   * Abort in-flight call-media Connect before joining the worker pool / destroying the hub.
   * Safe to call multiple times; no-op if libp2p media is not up.
   */
  void AbortCallMediaForShutdown();

  void BindSessionStore(SessionStore& store);

  /** App-owned profile vault/DEK service. Bind before EnsureMessagingReady. */
  void BindSecrets(ProfileSecretsService& secrets);

  /** libp2p / P2P stack ready after profile unlock + identity load. */
  bool IsMessagingReady() const { return messaging_ready_; }

  /** Requires ProfileSecretsService unlocked; loads identity and starts libp2p. */
  Roe<void> EnsureMessagingReady();

  InboxController& Inbox();
  P2pMessagingService& P2p();
  GroupMembershipService& Groups();
  /** Call media / session / lifecycle stack (Wave 3). Always non-null after construction. */
  CallStack& CallStackRef() { return *call_stack_; }
  const CallStack& CallStackRef() const { return *call_stack_; }
  CallSessionManager* Calls();
  CallLifecycle* Lifecycle();
  MessageRouter& Router();
  ContactActionDispatcher& Actions();
  bool HasRouter() const { return router_ != nullptr; }
  IThreadStore& Store();
  ContactsStore& Contacts();
  IdentityStore& Identity();
  IDirectoryClient& Directory();
  DirectoryShadowCache& DirectoryShadows();
  PeerDisplayResolver& PeerLabels();
  IRegistrationClient& Registration();
  IPushDeviceClient* PushDevices();
  IClientCompatClient* ClientCompat();
  /** Profile data directory used for stores and client-compat cache. */
  const std::string& ProfileDataDir() const { return data_dir_; }
  // Thin forward for internal call wiring; mesh UX should use Mesh()->Host().
  Libp2pHost* Libp2p();
  PeerSessionManager* Sessions() const;
  /** Last libp2p start failure (empty if ok). For Network settings UX. */
  const std::string& LastLibp2pError() const { return libp2p_last_error_; }

  /** Desktop Node "Help the network" posture (node_enabled → Libp2pRole::Node). */
  bool IsHelpNetworkEnabled() const;

  /** Me → Profile projection (no LocalIdentity leak to settings UI). */
  ProfileIdentityView LoadProfileIdentityView();
  Roe<void> SaveProfileNickname(const std::string& nickname);
  Roe<void> RegisterIdentity(const std::string& nickname);
  Roe<void> RotateBriefLlmKey();

  ReachabilitySnapshot Reachability() const;
  void RunReachabilityProbe(bool try_upnp);
  void TryUpnpPortMapping();
  void TickReachabilityUx();
  void RefreshMeshCapabilities();
  void Apply(const NetworkConfig& config);
  void Apply(const PolicyPrefs& prefs);
  void Apply(const NotificationPrefs& prefs);

  /**
   * Shared libp2p mesh composition root (NodeRuntime + dial-back + relays +
   * reachability). Mesh UX (status chrome, reachability, relay load) should read
   * through here rather than the thin hub forwards below. Null before the stack
   * is up; callers must degrade gracefully.
   */
  MeshHost* Mesh() { return mesh_.get(); }
  const MeshHost* Mesh() const { return mesh_.get(); }

  // Thin mesh forwards kept for internal call/lifecycle wiring; prefer Mesh()
  // for mesh UX so the public hub surface stays narrow.
  DialBackService* DialBack() { return mesh_ ? mesh_->DialBack() : nullptr; }
  CircuitRelayService* CircuitRelay() { return mesh_ ? mesh_->CircuitRelay() : nullptr; }
  MediaRelayService* MediaRelay() { return mesh_ ? mesh_->MediaRelay() : nullptr; }

  /**
   * nf: try circuit bridge via preferred hops (contacts then seed when prefer_contacts).
   * Registers hop endpoints as needed; returns first successful bridge.
   */
  Roe<CircuitRelayBridgeResult> RequestCircuitBridgePreferred(const std::string& target_peer_id,
                                                              const std::string& target_multiaddr,
                                                              int timeout_ms = 8000);

  void SetOnReachabilityUpdated(std::function<void()> callback);

  void BindAgent(AgentSession& agent);
  PeerSigningKeyStore& SigningKeys();

  /** Idle sweep / session policy tick (call from UI loop). */
  void TickLibp2p();
  /** Drop cold peer connections (Android background). */
  void SuspendLibp2pColdPeers();

  void SetOnMessagingReady(std::function<void()> callback);
  /** FCM/opaque call_wake — hop to UI (CallController::OnCallWake). Set from Application. */
  void SetOnCallWake(std::function<void()> callback);

  /** L4: PeerId-only OK when stack address book has a dial path (or relay / pasted ma). */
  bool IsContactReachable(const Contact& contact) const;

private:
  void InstallServiceClients(const AppConfig& config);
  void UpdateServiceClients(const AppConfig& config);
  void WireRelayAuthSigner();
  Roe<void> StartLibp2p(const AppConfig& config);
  void StopLibp2p();
  /** App-only mesh glue (CallMediaDirect / LAN mDNS / policies) after MeshHost start. */
  void StartMeshServices();
  /** Shared libp2p mesh host (NodeRuntime + dial-back + relays + reachability). */
  NodeRuntime* Runtime() const { return mesh_ ? mesh_->Runtime() : nullptr; }
  void ApplyMeshAdmissionPolicies();
  void PublishNodeAdvertisedAddrs();
  /** CallStackDeps for building the call stack against the current p2p / mesh / config. */
  CallStackDeps MakeCallStackDeps();
  void RegisterContactEndpoints();
  Roe<void> BuildMessagingStack();
  void NotifyMessagingReady();

  void SyncMobileEphemeralListen();
  void SyncLanMdnsAdvertisement();
  void OnLanMdnsPeerDiscovered(const LanMdnsDiscoveredPeer& peer);
  void PublishMobileCallScopedAddrs();
  void PrefetchPeerReachability(const std::string& identity);
  void StartCoordinatorTimers();
  void StopCoordinatorTimers();

  std::string data_dir_;
  std::string profile_id_;
  AppConfig config_;
  AgentSession* agent_ = nullptr;
  SessionStore* session_store_ = nullptr;
  ProfileSecretsService* secrets_ = nullptr;

  // --- MessagingCore stores / HTTP / inbox ---------------------------------
  std::unique_ptr<SqliteThreadStore> store_;
  std::unique_ptr<ContactsStore> contacts_;
  std::unique_ptr<IdentityStore> identity_;
  PeerSigningKeyStore signing_key_store_;
  PeerKemKeyStore kem_key_store_;
  std::unique_ptr<SqlitePskSessionStore> psk_store_;
  std::unique_ptr<GroupRosterStore> group_roster_;
  std::unique_ptr<GroupInviteGate> group_invite_gate_;
  std::unique_ptr<DirectoryShadowCache> directory_shadows_;
  std::unique_ptr<PeerDisplayResolver> peer_labels_;
  std::unique_ptr<GroupMembershipService> group_membership_;
  std::unique_ptr<RelayDirectorySigningKeyResolver> signing_resolver_;
  std::unique_ptr<RelayDirectoryKemKeyResolver> kem_resolver_;
  std::unique_ptr<InboxController> inbox_;
  std::string http_relay_url_;
  std::string http_directory_url_;
  std::string http_registration_url_;
  std::unique_ptr<HttpRelayClient> http_relay_;
  std::unique_ptr<HttpPushDeviceClient> http_push_devices_;
  std::unique_ptr<HttpDirectoryClient> http_directory_;
  std::unique_ptr<HttpRegistrationClient> http_registration_;
  std::unique_ptr<HttpClientCompatClient> http_client_compat_;
  IRelayClient* relay_ = nullptr;
  IPushDeviceClient* push_devices_ = nullptr;
  IDirectoryClient* directory_ = nullptr;
  IRegistrationClient* registration_ = nullptr;
  IClientCompatClient* client_compat_ = nullptr;
  std::unique_ptr<P2pMessagingService> p2p_;
  std::unique_ptr<ContactActionDispatcher> actions_;
  std::unique_ptr<MessageRouter> router_;

  // --- CallStack (app-only) ------------------------------------------------
  std::unique_ptr<CallStack> call_stack_;

  // --- MeshHost (shared with pp-node) + app mesh glue ----------------------
  std::unique_ptr<MeshHost> mesh_;
  std::unique_ptr<LanMdnsDiscovery> lan_mdns_;
  std::string libp2p_last_error_;
  bool upnp_auto_tried_ = false;
  bool reachability_banner_shown_ = false;
  uint64_t reachability_outbound_since_ms_ = 0;
  std::function<void()> on_reachability_updated_;
  std::function<void()> on_messaging_ready_;
  std::function<void()> on_call_wake_;
  bool initialized_ = false;
  bool messaging_ready_ = false;
  uint64_t hub_policy_timer_id_ = 0;
  /** True while StartEphemeralListenAsync is in flight (avoid duplicate starts from UI tick). */
  bool mobile_ephemeral_start_inflight_ = false;
  int64_t mobile_ephemeral_start_inflight_at_ms_ = 0;
  /** True while StopEphemeralListen runs on IO (ListenOn/StopListening PostAndWait). */
  bool mobile_ephemeral_stop_inflight_ = false;
  std::unordered_set<std::string> lan_mdns_contact_peer_ids_;
  std::string mobile_ephemeral_last_start_error_;
};

/** Preferred name for the app messaging assembler (holds MeshHost + CallStack). */
using MessagingCore = MessagingHub;

} // namespace pbr
