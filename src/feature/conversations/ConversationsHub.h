#pragma once

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
#include "feature/conversations/PeerDisplayResolver.h"
#include "domain/messaging/PeerKemKeyStore.h"
#include "domain/messaging/GroupRosterStore.h"
#include "domain/messaging/PeerSigningKeyStore.h"
#include "feature/conversations/GroupInviteGate.h"
#include "feature/conversations/GroupMembershipWorkflow.h"
#include "domain/messaging/SqliteThreadStore.h"
#include "domain/messaging/InitiationBillingStore.h"
#include "feature/calls/CallStack.h"
#include "domain/messaging/AttachmentDownloadPolicy.h"
#include "domain/messaging/AttachmentSuppressionStore.h"
#include "feature/conversations/AgentInboundPorts.h"
#include "feature/conversations/MessageRouter.h"
#include "feature/conversations/MeshDeliveryOrchestrator.h"
#include "domain/net/BlobClient.h"
#include "domain/net/BlobQuotaUtil.h"
#include "domain/net/HttpBlobClient.h"
#include "domain/net/OrgBackendClientsImpl.h"
#include "domain/net/IPushDeviceClient.h"
#include "domain/mesh/l4/circuit/CircuitRelayTypes.h"
#include "domain/mesh/reachability/LanMdnsDiscovery.h"
#include "domain/mesh/reachability/Reachability.h"
#include "domain/mesh/reachability/ReachabilityEngine.h"
#include "domain/mesh/discovery/MeshDirectoryCache.h"
#include "domain/mesh/discovery/NameDirectory.h"
#include "domain/mesh/dht/DhtTypes.h"
#include "domain/mesh/host/MeshHost.h"
#include "domain/people/MeshHopPolicy.h"
#include "domain/messaging/PaymentPromiseStore.h"
#include "domain/messaging/PaymentPromiseLifecycle.h"
#include "domain/messaging/PaymentPromiseWireCodec.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include "common/PbrCompat.h"

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
 * - **CallStack (`call_stack_`):** call media, CSM, lifecycle, Amp media bridge,
 *   dial/hop helpers.
 *
 * UI/tools talk through ConversationsFacade / CallUiBackend / ports — not Hub accessors.
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

    bool operator==(const NetworkConfig& other) const {
      return relay.base_url == other.relay.base_url && directory == other.directory &&
             registration.base_url == other.registration.base_url && node_enabled == other.node_enabled &&
             circuit_relay == other.circuit_relay && media_relay == other.media_relay && dht == other.dht &&
             prefer_contacts_for_routing == other.prefer_contacts_for_routing;
    }
    bool operator!=(const NetworkConfig& other) const { return !(*this == other); }
  };

  /** Inbound group-invite + attachment download policy projected from ProfilePreferences. */
  struct PolicyPrefs {
    GroupInvitePolicy group_invite_policy = GroupInvitePolicy::ContactsOnly;
    AttachmentDownloadPolicy attachment_download_policy = AttachmentDownloadPolicy::Smart;

    bool operator==(const PolicyPrefs& other) const {
      return group_invite_policy == other.group_invite_policy &&
             attachment_download_policy == other.attachment_download_policy;
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

  ConversationsHub();
  ~ConversationsHub();

  /** Core store/inbox/AI — profile vault unlock is separate (ProfileSecretsEngine). */
  Roe<void> Initialize(const AppConfig& config, const std::string& profile_data_dir);
  Roe<void> Reinitialize(const AppConfig& config, const std::string& profile_data_dir);
  void Shutdown();
  bool IsInitialized() const { return initialized_; }

  /**
   * Abort in-flight call-media Connect before joining the worker pool / destroying the hub.
   * Safe to call multiple times; no-op if media is not up.
   */
  void AbortCallMediaForShutdown();

  void BindSessionStore(SessionStore& store);

  /** App-owned profile vault/DEK service. Bind before EnsureMessagingReady. */
  void BindSecrets(ProfileSecretsEngine& secrets);

  /** Amp / P2P stack ready after profile unlock + identity load. */
  bool IsMessagingReady() const { return messaging_ready_; }

  /** Requires ProfileSecretsEngine unlocked; loads identity and starts Amp mesh. */
  Roe<void> EnsureMessagingReady();

  InboxController& Inbox();
  MeshDeliveryOrchestrator& MeshMessaging();
  GroupMembershipWorkflow& Groups();
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
  IPskSessionStore* PskStore();
  ProfileSecretsEngine* Secrets();
  IDirectoryClient& Directory();
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
  Roe<void> RegisterIdentity(const std::string& nickname);
  Roe<void> UploadProfileIconFromPath(const std::string& path);
  Roe<void> ClearProfileIcon();
  Roe<BlobQuotaRecoveryPlan> PlanRelayQuotaRecovery();
  Roe<void> FreeOldestRelayBlobSlot();
  void RequestAttachmentDownload(const std::string& thread_id, const std::string& message_id);
  void DrainPendingAttachmentMedia();
  Roe<void> ClearDownloadedAttachments();
  Roe<ThreadMessage> SendAttachmentFromPath(const std::string& thread_id, const std::string& path);
  AttachmentFetchWorkflow& Attachments();
  std::string ContactIconLocalPath(const Contact& contact);
  std::string IdentityIconLocalPath(const std::string& identity);
  void EnsureDirectoryHitIconCached(const DirectoryHit& hit);
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
  /** Stage a remote signed receipt from an inbound control message (does not commit; P003). */
  Roe<PaymentPromise> StagePaymentPromiseControlMessage(const ThreadMessage& message);

  ReachabilitySnapshot Reachability() const;
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
  MeshHost* Mesh() { return mesh_.get(); }
  const MeshHost* Mesh() const { return mesh_.get(); }

  /**
   * nf: try circuit bridge via preferred hops (contacts then seed when prefer_contacts).
   * Registers hop endpoints as needed; returns first successful Amp bridge.
   */
  Roe<CircuitRelayBridgeResult> RequestCircuitBridgePreferred(const std::string& target_peer_id,
                                                              const std::string& target_multiaddr,
                                                              int timeout_ms = 8000);

  void SetOnReachabilityUpdated(std::function<void()> callback);

  void BindAgentInbound(AgentInboundPorts ports);
  PeerSigningKeyStore& SigningKeys();

  /** Idle sweep / session policy tick (coordinator ~1s). Amp UDP is TickAmpMesh. */
  void TickMesh();
  /** Drop cold peer connections (Android background). */
  void SuspendMeshColdPeers();

  void SetOnMessagingReady(std::function<void()> callback);
  /** FCM/opaque call_wake — hop to UI (CallController::OnCallWake). Set from Application. */
  void SetOnCallWake(std::function<void()> callback);

  /** L4: PeerId-only OK when Amp address book has a dial path (or relay / pasted ma). */
  bool IsContactReachable(const Contact& contact) const;

private:
  void InstallOrgBackendClients(const AppConfig& config);
  void UpdateOrgBackendClients(const AppConfig& config);
  void WireRelayAuthSigner();
  Roe<void> StartMesh(const AppConfig& config);
  void StopMesh();
  /** App-only mesh glue (LAN mDNS / policies) after MeshHost start. */
  void StartMeshServices();
  void ApplyMeshAdmissionPolicies();
  void PublishNodeAdvertisedAddrs();
  /** CallStackDeps for building the call stack against the current p2p / mesh / config. */
  CallStackDeps MakeCallStackDeps();
  void RegisterContactEndpoints();
  void RegisterMeshDirectoryEndpoints();
  void RegisterDhtBootstrapEndpoints();
  void ConfigureAmpDhtProtocol();
  void ConfigureAmpDirectoryProtocol();
  void ApplyDhtFindPeerResult(const std::string& peer_id, const PeerRoutingRecord& record);
  Roe<void> BuildMessagingStack();
  void NotifyMessagingReady();

  void ScheduleDirectoryHitIconFetch(const DirectoryHit& hit);
  void ScheduleContactIconFetch(const Contact& contact);
  void NotifyPeerIconsChanged();

  void SyncMobileEphemeralListen();
  void SyncLanMdnsAdvertisement();
  void OnLanMdnsPeerDiscovered(const LanMdnsDiscoveredPeer& peer);
  void PublishMobileCallScopedAddrs();
  void PrefetchPeerReachability(const std::string& identity);
  void StartCoordinatorTimers();
  void StopCoordinatorTimers();
  /** Amp UDP drain — MeshHost::Tick / MeshRuntime::Drive (no libp2p io_context). */
  void TickAmpMesh();

  std::string data_dir_;
  std::string profile_id_;
  AppConfig config_;
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
  std::unique_ptr<GroupInviteGate> group_invite_gate_;
  std::unique_ptr<DirectoryShadowCache> directory_shadows_;
  std::unique_ptr<MeshDirectoryCache> mesh_directory_cache_;
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
  std::unique_ptr<IDirectoryClient> directory_owned_;
  std::unique_ptr<HttpRegistrationClient> http_registration_;
  std::unique_ptr<HttpBlobClient> http_blob_;
  std::unique_ptr<HttpClientCompatClient> http_client_compat_;
  IRelayClient* relay_ = nullptr;
  IPushDeviceClient* push_devices_ = nullptr;
  IDirectoryClient* directory_ = nullptr;
  IRegistrationClient* registration_ = nullptr;
  IBlobClient* blob_ = nullptr;
  IClientCompatClient* client_compat_ = nullptr;
  std::unique_ptr<MeshDeliveryOrchestrator> mesh_messaging_;
  std::unique_ptr<ContactActionDispatcher> actions_;
  std::unique_ptr<MessageRouter> router_;

  // --- CallStack (app-only) ------------------------------------------------
  std::unique_ptr<CallStack> call_stack_;

  // --- MeshHost (shared with pp-node) + app mesh glue ----------------------
  std::unique_ptr<MeshHost> mesh_;
  std::unique_ptr<LanMdnsDiscovery> lan_mdns_;
  std::string mesh_last_error_;
  bool upnp_auto_tried_ = false;
  bool reachability_banner_shown_ = false;
  uint64_t reachability_outbound_since_ms_ = 0;
  std::function<void()> on_reachability_updated_;
  std::function<void()> on_peer_icons_changed_;
  std::function<void()> on_messaging_ready_;
  std::function<void()> on_call_wake_;
  bool initialized_ = false;
  bool messaging_ready_ = false;
  uint64_t hub_policy_timer_id_ = 0;
  uint64_t amp_mesh_pump_timer_id_ = 0;
  /** True while StartEphemeralListenAsync is in flight (avoid duplicate starts from UI tick). */
  bool mobile_ephemeral_start_inflight_ = false;
  int64_t mobile_ephemeral_start_inflight_at_ms_ = 0;
  /** True while StopEphemeralListen runs on IO (ListenOn/StopListening PostAndWait). */
  bool mobile_ephemeral_stop_inflight_ = false;
  std::unordered_set<std::string> lan_mdns_contact_peer_ids_;
  std::string mobile_ephemeral_last_start_error_;
};

/** Preferred name for the app messaging assembler (holds MeshHost + CallStack). */
using ConversationsCore = ConversationsHub;

} // namespace pbr
