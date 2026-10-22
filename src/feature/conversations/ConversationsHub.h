#pragma once

#include "common/chat/SystemControl.h"

#include "foundation/data/Config.h"
#include "foundation/data/MeshRole.h"
#include "foundation/data/SessionStore.h"
#include "foundation/data/UserPreferences.h"
#include "domain/messaging/GroupTypes.h"
#include "domain/people/ContactsStore.h"
#include "domain/people/IdentityStore.h"
#include "domain/people/ProfileIdentityView.h"
#include "common/Module.h"
#include "feature/conversations/ContactActionDispatcher.h"
#include "feature/conversations/DirectoryShadowCache.h"
#include "feature/conversations/InboxController.h"
#include "feature/conversations/MeshConnectivityWiring.h"
#include "feature/conversations/PeerDisplaySources.h"
#include "domain/messaging/PeerKemKeyStore.h"
#include "domain/messaging/GroupRosterStore.h"
#include "domain/messaging/PeerSigningKeyStore.h"
#include "domain/messaging/GroupInviteGate.h"
#include "feature/conversations/GroupMembershipWorkflow.h"
#include "feature/conversations/HubParticipant.h"
#include "feature/conversations/HubPolicyPrefs.h"
#include "domain/messaging/SqliteThreadStore.h"
#include "domain/messaging/ThreadLocalPrefsStore.h"
#include "domain/pricing/PaymentPromiseInbound.h"
#include "domain/pricing/InitiationBillingStore.h"
#include "common/Lifetime.h"
#include "mesh/connectivity/MeshConnectivity.h"
#include "mesh/media_plane/MeshMediaRelay.h"
#include "foundation/platform/NetworkMonitor.h"
#include "common/chat/AttachmentDownloadPolicy.h"
#include "mesh/types/AddressDisclosure.h"
#include "domain/messaging/AttachmentSuppressionStore.h"
#include "feature/conversations/AgentInboundPorts.h"
#include "feature/conversations/MessageRouter.h"
#include "feature/conversations/MeshDeliveryOrchestrator.h"
#include "domain/net/BlobClient.h"
#include "domain/net/BlobQuota.h"
#include "domain/net/HttpBlobClient.h"
#include "domain/net/OrgBackendClientsImpl.h"
#include "domain/net/IPushDeviceClient.h"
#include "mesh/l4/circuit/CircuitRelayTypes.h"
#include "mesh/reachability/LanMdnsDiscovery.h"
#include "mesh/reachability/Reachability.h"
#include "mesh/reachability/ReachabilityEngine.h"
#include "mesh/discovery/MeshDirectoryCache.h"
#include "mesh/discovery/NameDirectory.h"
#include "mesh/dht/DhtTypes.h"
#include "mesh/host/MeshHost.h"
#include "domain/people/MeshHopPolicy.h"
#include "domain/pricing/PaymentPromiseStore.h"
#include "domain/pricing/PaymentPromiseLifecycle.h"
#include "domain/pricing/PaymentPromiseWireCodec.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

class IPskSessionStore;
class ProfileSecretsEngine;
class RelayDirectoryKemKeyResolver;
class RelayDirectorySigningKeyResolver;
class SqlitePskSessionStore;

/**
 * App-only messaging composition root (also known as ConversationsCore).
 *
 * Ownership planes:
 * - **ConversationsCore (this class):** stores, HTTP Brief clients, inbox/P2P/groups/router,
 *   LAN mDNS, policy timers, N025 ephemeral-listen *execution* glue.
 * - **MeshHost (`mesh_`):** shared with headless `pp-node` — Amp stack + L4 + reachability.
 * - **Participants (`HubParticipant`):** features riding messaging and the mesh (calls, live
 *   broadcast), owned and wired at the feature level (feature/product/ProductStack). The hub drives
 *   their lifecycle and lends them its stores, messaging and neutral mesh objects; it never names
 *   them.
 *
 * UI/tools talk through ConversationsFacade / ports — not Hub accessors.
 * Profile secrets + identity DEK registration remain injected (`BindSecrets`).
 */
class ConversationsHub : public Module {
public:
  /** Hot-reloadable network / mesh slice projected from AppConfig. */
  struct NetworkConfig {
    ServiceEndpointConfig relay;
    DirectoryConfig directory;
    ServiceEndpointConfig registration;
    bool node_enabled = true;
    bool circuit_relay = false;
    bool media_relay = true;
    bool dht = false;
    bool prefer_contacts_for_routing = true;
    mesh::DirectAudience direct_connections = mesh::DirectAudience::Contacts;
    bool trusted_relays_only = false;

    bool operator==(const NetworkConfig& other) const {
      return relay.base_url == other.relay.base_url && directory == other.directory &&
             registration.base_url == other.registration.base_url && node_enabled == other.node_enabled &&
             circuit_relay == other.circuit_relay && media_relay == other.media_relay && dht == other.dht &&
             prefer_contacts_for_routing == other.prefer_contacts_for_routing &&
             direct_connections == other.direct_connections && trusted_relays_only == other.trusted_relays_only;
    }
    bool operator!=(const NetworkConfig& other) const { return !(*this == other); }
  };

  using PolicyPrefs = HubPolicyPrefs;


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

  ConversationsHub();
  ~ConversationsHub();

  /** Core store/inbox/AI — profile vault unlock is separate (ProfileSecretsEngine). */
  Roe<void> Initialize(const AppConfig& config, const std::string& profile_data_dir);
  Roe<void> Reinitialize(const AppConfig& config, const std::string& profile_data_dir);
  void Shutdown();
  /**
   * Quit when the runtime teardown quiesce timed out: persist stores but free nothing
   * (running tasks may still touch them — THREADING.md § Teardown quiesce).
   */
  void FlushForExit();
  bool IsInitialized() const { return initialized_; }

  /**
   * Abort in-flight call-media Connect before joining the worker pool / destroying the hub.
   * Safe to call multiple times; no-op if media is not up.
   */
  /** Participants unblock their waits (shutdown requested). Any thread. */
  void AbortParticipantsForShutdown();

  /**
   * Mark app shutdown so an in-flight EnsureMessagingReady / StartMesh must not finish
   * bring-up (and must discard a partial stack). Call before AppRuntime::Shutdown joins
   * the unlock worker — otherwise mesh comes up during join and StopMesh runs with no pool.
   */
  void RequestShutdown();
  /** Undo RequestShutdown when the teardown was abandoned (profile reset quiesce timed out). */
  void CancelShutdownRequest();

  void BindSessionStore(SessionStore& store);

  /** App-owned profile vault/DEK service. Bind before EnsureMessagingReady. */
  void BindSecrets(ProfileSecretsEngine& secrets);

  /**
   * Local messaging stack ready (vault unlocked, identity loaded, relay-capable
   * orchestrator). Does **not** imply Amp mesh is up — see IsMeshReady().
   */
  bool IsMessagingReady() const { return messaging_ready_; }

  /** Amp mesh underlay running and attached after async bring-up. */
  bool IsMeshReady() const { return mesh_ready_; }

  /**
   * Requires ProfileSecretsEngine unlocked; loads identity and builds the **local**
   * messaging stack, then schedules Amp mesh start off the critical path.
   * Returns once messaging_ready_ is set (cover / unlock_in_progress may clear).
   */
  Roe<void> EnsureMessagingReady();

  InboxController& Inbox();
  /** Per-thread local settings (pin / mute / archive); device-only, not part of the chat protocol. */
  ThreadLocalPrefsStore& ThreadPrefs();
  MeshDeliveryOrchestrator& MeshMessaging();
  GroupMembershipWorkflow& Groups();
  /**
   * k5: the device's network changed (any thread; the OS monitor calls it, tests may too). The mesh
   * re-validates its links and addresses; calls re-anchor / retry their upgrade.
   */
  void OnLocalNetworkChanged(const NetworkChange& change);

  /**
   * Register a participant (non-owning) before Initialize; it must outlive the hub's Shutdown.
   * The owner clears them after Shutdown, before they go.
   */
  void AddParticipant(HubParticipant& participant);
  void ClearParticipants();

  // --- Lent to participants -------------------------------------------------
  /** Null until a messaging stack is built (and after it is dropped). */
  MeshDeliveryOrchestrator* MeshMessagingOrNull() { return mesh_messaging_.get(); }
  SystemControlRegistry& SystemControls() { return system_controls_; }
  SqlitePskSessionStore* PskSessions() { return psk_store_.get(); }
  /** The group rosters (from stores open to hub teardown; null outside that). */
  GroupRosterStore* GroupRosterOrNull() { return group_roster_.get(); }
  std::string ProfileDbPath() const { return store_ ? store_->ProfileDbPath() : std::string(); }
  const std::string& ProfileId() const { return profile_id_; }
  const mesh::AddressDisclosureGate& AddressDisclosure() const { return address_disclosure_; }
  /** The neutral mesh objects (L015): outlive every participant; rewired between detach and rebind. */
  mesh::MeshConnectivity& Connectivity() { return *mesh_connectivity_; }
  mesh::MeshMediaRelay& MediaRelay() { return *mesh_media_relay_; }
  /** What the mesh's reach reads (the mesh, directory / DHT nodes, seed probe, LAN notes). */
  MeshConnectivityWiringInputs MeshInputs();
  /** The mesh config as last published (immutable snapshot — read from any thread). */
  std::shared_ptr<const MeshConfig> MeshConfigSnapshot() const;
  void PrefetchPeerReachability(const std::string& identity);
  /** Re-evaluate the mobile ephemeral listen (a participant's want changed). UI thread. */
  void SyncMobileEphemeralListen();
  MessageRouter& Router();
  ContactActionDispatcher& Actions();
  bool HasRouter() const { return router_ != nullptr; }
  IThreadStore& Store();
  ContactsStore& Contacts();
  IdentityStore& Identity();
  IPskSessionStore* PskStore();
  ProfileSecretsEngine* Secrets();
  mesh::IDirectoryClient& Directory();
  DirectoryShadowCache& DirectoryShadows();
  PeerDisplayResolver& PeerLabels();
  IRegistrationClient& Registration();
  IBlobClient& Blob();
  IPushDeviceClient* PushDevices();
  IClientCompatClient* ClientCompat();
  /** Profile data directory used for stores and client-compat cache. */
  const std::string& ProfileDataDir() const { return data_dir_; }
  /** Last mesh start failure (empty if ok). For Network settings UX. */
  const std::string& LastMeshError() const { return mesh_last_error_; }

  /** Desktop Node "Help the network" posture (node_enabled → MeshRole::Node). */
  bool IsHelpNetworkEnabled() const;

  /** Me → Profile projection (no LocalIdentity leak to settings UI). */
  ProfileIdentityView LoadProfileIdentityView();
  Roe<void> SaveProfileNickname(const std::string& nickname);
  /** Refresh the local account backup on a worker (registration, nickname, contacts). Best effort. */
  void WriteLocalBackupAsync();
  Roe<void> RegisterIdentity(const std::string& nickname);
  Roe<void> UploadProfileIconFromPath(const std::string& path);
  Roe<void> ClearProfileIcon();
  Roe<BlobQuotaRecoveryPlan> PlanRelayQuotaRecovery();
  Roe<void> FreeOldestRelayBlobSlot();
  void RequestAttachmentDownload(const std::string& thread_id, const std::string& message_id);
  void DrainPendingAttachmentMedia();
  Roe<void> ClearDownloadedAttachments();
  /**
   * "Delete for everyone": only our own message in a chat or group. Sends the retract, then deletes the
   * message here like "delete for me", then (best effort) the uploaded copy on the relay. Blocking: call
   * off the UI thread.
   */
  Roe<void> DeleteMessageForEveryone(const std::string& thread_id, const std::string& message_id);
  Roe<ThreadMessage> SendAttachmentFromPath(const std::string& thread_id, const std::string& path);
  void SendAttachmentFromPathAsync(const std::string& thread_id, const std::string& path,
                                   std::function<void(Roe<ThreadMessage>)> on_done);
  AttachmentFetchWorkflow& Attachments();
  /** Our uploaded relay copy of an attachment message, if any; best effort. */
  void DeleteUploadedCopy(const ThreadMessage& message);
  std::string ContactIconLocalPath(const Contact& contact);
  std::string IdentityIconLocalPath(const std::string& identity);
  void EnsureDirectoryHitIconCached(const mesh::DirectoryHit& hit);
  void EnsureContactIconCached(const Contact& contact);
  void SetOnPeerIconsChanged(std::function<void()> callback);
  Roe<void> RotateBriefLlmKey();

  /** P001: send `charge_required` and re-lock peer initiation billing. */
  Roe<void> SendChargeRequired(const std::string& peer_identity,
                               std::optional<int64_t> floor_minor = std::nullopt);
  InitiationBillingStore* InitiationBilling() const { return initiation_billing_.get(); }
  PaymentPromiseStore* PaymentPromises() const { return payment_promises_.get(); }

  /** P002/P003: local signed payment-promise lifecycle (no settlement rails). */
  Roe<PaymentPromise> CreatePaymentPromiseOffer(const PaymentPromiseLifecycle::OfferParams& params);
  /** Peer-chat offer: forces `service_ref=thread:<id>` and payer-ack release (P003). */
  Roe<PaymentPromise> CreatePaymentPromiseOfferForThread(const std::string& thread_id,
                                                         PaymentPromiseLifecycle::OfferParams params);
  Roe<PaymentPromise> AcceptPaymentPromise(const std::string& promise_id);
  Roe<PaymentPromise> MarkPaymentPromiseDelivering(const std::string& promise_id);
  Roe<PaymentPromise> RecordPaymentPromiseOutcome(const std::string& promise_id, PaymentPromiseState outcome,
                                                  const std::string& note = {});
  Roe<void> AvoidPaymentPromiseCounterparty(const std::string& promise_id);
  Roe<std::vector<PaymentPromise>> ListPaymentPromises() const;
  Roe<std::optional<PaymentPromise>> GetPaymentPromise(const std::string& promise_id) const;
  Roe<std::vector<PaymentPromise>> ListPendingInboundPaymentPromises() const;
  Roe<std::optional<PaymentPromise>> GetPendingInboundPaymentPromise(const std::string& promise_id) const;
  /** Commit a staged inbound receipt into the local store (P003). */
  Roe<PaymentPromise> AcceptInboundPaymentPromise(const std::string& promise_id);
  /** Drop a staged inbound receipt without committing (P003). */
  Roe<bool> IgnoreInboundPaymentPromise(const std::string& promise_id);
  bool ShouldAvoidPaymentCounterparty(const std::string& other_account_id);
  Roe<ThreadMessage> BuildPaymentPromiseControlMessage(const std::string& thread_id,
                                                       PaymentPromiseControlType type,
                                                       const PaymentPromise& promise,
                                                       const std::string& body_text);

  mesh::ReachabilitySnapshot Reachability() const;
  void RunReachabilityProbe(bool try_upnp);
  void TryUpnpPortMapping();
  void TickReachabilityUx();
  void RefreshMeshCapabilities();
  void WireAttachmentDownloads();
  void Apply(const NetworkConfig& config);
  void Apply(const PolicyPrefs& prefs);
  void Apply(const NotificationPrefs& prefs);

  /**
   * Shared Amp mesh composition root. Mesh UX (status chrome, reachability, relay load) should
   * read through here. Null before the stack is up; callers must degrade gracefully.
   */
  mesh::MeshHost* Mesh() { return mesh_.get(); }
  const mesh::MeshHost* Mesh() const { return mesh_.get(); }

  void SetOnReachabilityUpdated(std::function<void()> callback);

  void BindAgentInbound(AgentInboundPorts ports);
  PeerSigningKeyStore& SigningKeys();

  /** Idle sweep / session policy tick (coordinator ~1s). Amp UDP is MeshHost MeshPump. */
  void TickMesh();
  /** Drop cold peer connections (Android background). */
  void SuspendMeshColdPeers();

  void SetOnMessagingReady(std::function<void()> callback);
  /** Fired on UI thread when async mesh bring-up finishes (success or soft-fail). */
  void SetOnMeshReady(std::function<void()> callback);
  /** FCM/opaque call_wake — hop to UI (CallController::OnCallWake). Set from Application. */
  void SetOnCallWake(std::function<void()> callback);

  /** L4: PeerId-only OK when Amp address book has a dial path (or relay / pasted ma). */
  bool IsContactReachable(const Contact& contact) const;

private:
  /** Bind the billing and payment-promise families (after the signing-key resolver exists). */
  void BindPricingFamilies();
  /** Account signing keys through the directory resolver (promise signatures). */
  AccountSigningKeyLookup AccountSigningKeys() const;
  void InstallOrgBackendClients(const AppConfig& config);
  void UpdateOrgBackendClients(const AppConfig& config);
  void WireRelayAuthSigner();
  Roe<void> StartMesh(const AppConfig& config);
  void StopMesh();
  void PublishMeshConfig();
  /** App-only mesh glue (LAN mDNS / policies) after MeshHost start. */
  void StartMeshServices();
  /** Undo BuildMessagingStack / StartMesh without a full hub Shutdown (shutdown race). */
  void DiscardMessagingBringUp();
  void ApplyMeshAdmissionPolicies();
  /** Republish who may learn our address (setting + contacts; projects/privacy T1). UI thread. */
  void PublishAddressDisclosure();
  /** The audience in force: the setting, or everyone in the Node role (P004). */
  mesh::DirectAudience EffectiveDirectAudience() const;
  void PublishNodeAdvertisedAddrs();
  /** Point mesh connectivity and the hop-policy feed at the current messaging / mesh. */
  void WireMeshConnectivity();
  void NoteLanMdnsPeerId(const std::string& peer_id);
  std::vector<HubParticipant*> Participants() const;
  /** Participants in reverse registration order (the way down). */
  std::vector<HubParticipant*> ParticipantsDown() const;
  bool ParticipantsWantEphemeralListen() const;
  void NotifyMessagingBuilt();
  void NotifyPolicyPrefs(const PolicyPrefs& prefs);
  void RegisterContactEndpoints();
  void RegisterMeshDirectoryEndpoints();
  void RegisterDhtBootstrapEndpoints();
  void ConfigureAmpDhtProtocol();
  void ConfigureAmpDirectoryProtocol();
  void ApplyDhtFindPeerResult(const std::string& peer_id, const mesh::PeerRoutingRecord& record);
  /** The messaging objects (orchestrator, groups, actions) over the open stores; participants told. */
  void BuildMessagingObjects();
  /** Unlock: the relay-capable stack ready to use (built at Initialize; rebuilt after a discard). */
  Roe<void> BuildLocalMessagingStack();
  /** Attach Amp links from a running mesh_ to the orchestrator. */
  Roe<void> AttachAmpMessagingStack();
  /** @deprecated Prefer BuildLocalMessagingStack + ScheduleMeshBringUp. Sync local+mesh. */
  Roe<void> BuildMessagingStack();
  void ScheduleMeshBringUp();
  void NotifyMessagingReady();
  void NotifyMeshReady();

  void ScheduleDirectoryHitIconFetch(const mesh::DirectoryHit& hit);
  void ScheduleContactIconFetch(const Contact& contact);
  void NotifyPeerIconsChanged();

  void SyncLanMdnsAdvertisement();
  void OnLanMdnsPeerDiscovered(const mesh::LanMdnsDiscoveredPeer& peer);
  /** k5: OS network monitor lives with the mesh services (started / stopped with them). */
  void StartNetworkMonitor();
  void StopNetworkMonitor();
  /** k6: a reachability probe finished — a moved observed address is mobility churn. UI thread. */
  void NoteObservedAddress();
  void PublishMobileCallScopedAddrs();
  void StartCoordinatorTimers();
  void StopCoordinatorTimers();

  std::string data_dir_;
  std::string profile_id_;
  AppConfig config_;  // UI (the hub's thread)
  /** `config_.mesh` as owners read it (call stack, mesh media policy): republished on every write. */
  mutable std::mutex mesh_config_mu_;
  std::shared_ptr<const MeshConfig> mesh_config_snapshot_ = std::make_shared<const MeshConfig>();
  /** Who may learn our address; read by mesh and call paths on any thread (outlives them all). */
  mesh::AddressDisclosureGate address_disclosure_;
  /** System-control families features bind (call control): read by receive and delivery threads,
   *  bound by the call stack — declared early, so it outlives them all. */
  SystemControlRegistry system_controls_;
  AgentInboundPorts agent_inbound_;
  SessionStore* session_store_ = nullptr;
  ProfileSecretsEngine* secrets_ = nullptr;

  // --- ConversationsCore stores / HTTP / inbox ---------------------------------
  std::unique_ptr<SqliteThreadStore> store_;
  std::unique_ptr<ContactsStore> contacts_;
  std::unique_ptr<IdentityStore> identity_;
  std::unique_ptr<InitiationBillingStore> initiation_billing_;
  std::unique_ptr<PaymentPromiseStore> payment_promises_;
  PeerSigningKeyStore signing_key_store_;
  PeerKemKeyStore kem_key_store_;
  std::unique_ptr<SqlitePskSessionStore> psk_store_;
  std::unique_ptr<GroupRosterStore> group_roster_;
  std::unique_ptr<ThreadLocalPrefsStore> thread_prefs_;
  std::unique_ptr<GroupInviteGate> group_invite_gate_;
  std::unique_ptr<DirectoryShadowCache> directory_shadows_;
  std::unique_ptr<mesh::MeshDirectoryCache> mesh_directory_cache_;
  std::unique_ptr<PeerDisplayResolver> peer_labels_;
  std::unique_ptr<GroupMembershipWorkflow> group_membership_;
  std::unique_ptr<RelayDirectorySigningKeyResolver> signing_resolver_;
  std::unique_ptr<RelayDirectoryKemKeyResolver> kem_resolver_;
  std::unique_ptr<InboxController> inbox_;
  std::unique_ptr<AttachmentSuppressionStore> attachment_suppressions_;
  std::unique_ptr<AttachmentFetchWorkflow> attachment_downloads_;
  std::string http_relay_url_;
  std::string http_directory_url_;
  std::string http_registration_url_;
  std::unique_ptr<HttpRelayClient> http_relay_;
  std::unique_ptr<HttpPushDeviceClient> http_push_devices_;
  /** Owns HTTP or FailoverDirectoryClient backends (person + HTTP mesh list). */
  std::unique_ptr<mesh::IDirectoryClient> directory_owned_;
  std::unique_ptr<HttpRegistrationClient> http_registration_;
  std::unique_ptr<HttpBlobClient> http_blob_;
  std::unique_ptr<HttpClientCompatClient> http_client_compat_;
  IRelayClient* relay_ = nullptr;
  IPushDeviceClient* push_devices_ = nullptr;
  mesh::IDirectoryClient* directory_ = nullptr;
  IRegistrationClient* registration_ = nullptr;
  IBlobClient* blob_ = nullptr;
  IClientCompatClient* client_compat_ = nullptr;
  std::unique_ptr<MeshDeliveryOrchestrator> mesh_messaging_;
  std::unique_ptr<ContactActionDispatcher> actions_;
  std::unique_ptr<MessageRouter> router_;

  // --- Neutral mesh objects (L015) — lent to participants; outlive them ---
  /** Reaching peers (dial registry, listen book, circuit reach, rendezvous, punch walk). */
  std::unique_ptr<mesh::MeshConnectivity> mesh_connectivity_;
  /** The media_relay client, built on connectivity (declared after it: goes first). */
  std::unique_ptr<mesh::MeshMediaRelay> mesh_media_relay_;
  /** Pushes the candidate policy into connectivity while the mesh runs (declared after it: goes first). */
  std::unique_ptr<MeshHopPolicyFeed> hop_policy_feed_;

  /** Registered participants (calls, broadcast); read from any thread. */
  mutable std::mutex participants_mu_;
  std::vector<HubParticipant*> participants_;

  // --- MeshHost (shared with pp-node) + app mesh glue ----------------------
  std::unique_ptr<mesh::MeshHost> mesh_;
  std::unique_ptr<mesh::LanMdnsDiscovery> lan_mdns_;
  std::unique_ptr<NetworkMonitor> network_monitor_;
  /** Last seed-observed public address (k6 churn). UI thread. */
  std::string last_observed_addr_;
  std::string mesh_last_error_;
  bool upnp_auto_tried_ = false;
  bool reachability_banner_shown_ = false;
  uint64_t reachability_outbound_since_ms_ = 0;
  std::function<void()> on_reachability_updated_;
  std::function<void()> on_peer_icons_changed_;
  std::function<void()> on_messaging_ready_;
  std::function<void()> on_mesh_ready_;
  std::function<void()> on_call_wake_;
  bool initialized_ = false;
  bool messaging_ready_ = false;
  bool mesh_ready_ = false;
  std::atomic<bool> mesh_bringup_scheduled_{false};
  std::atomic<bool> shutdown_requested_{false};
  uint64_t hub_policy_timer_id_ = 0;
  /** True while StartEphemeralListenAsync is in flight (avoid duplicate starts from UI tick). */
  bool mobile_ephemeral_start_inflight_ = false;
  int64_t mobile_ephemeral_start_inflight_at_ms_ = 0;
  /** True while StopEphemeralListen runs on IO (ListenOn/StopListening PostAndWait). */
  bool mobile_ephemeral_stop_inflight_ = false;
  std::unordered_set<std::string> lan_mdns_contact_peer_ids_;
  std::string mobile_ephemeral_last_start_error_;
};

/** Preferred name for the app messaging assembler (holds MeshHost; calls and broadcast participate). */
using ConversationsCore = ConversationsHub;

} // namespace pbr
