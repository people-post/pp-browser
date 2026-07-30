#pragma once

#include "base/data/Config.h"
#include "base/data/Libp2pRole.h"
#include "base/data/UserPreferences.h"
#include "base/people/ContactsStore.h"
#include "base/people/IdentityStore.h"
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
#include "base/media/CallMediaEngine.h"
#include "base/messaging/SqliteThreadStore.h"
#include "base/messaging/CallSessionStore.h"
#include "feature/messaging/CallMediaKeyStore.h"
#include "feature/messaging/CallSessionManager.h"
#include "feature/messaging/MessageRouter.h"
#include "feature/messaging/P2pMessagingService.h"
#include "base/net/ServiceClientsImpl.h"
#include "base/net/IPushDeviceClient.h"
#include "libp2p/integration/host/Libp2pHost.h"
#include "libp2p/integration/host/DialBackService.h"
#include "libp2p/integration/host/CircuitRelayService.h"
#include "libp2p/integration/host/MediaRelayService.h"
#include "libp2p/integration/host/Reachability.h"
#include "libp2p/integration/host/ReachabilityService.h"
#include "libp2p/integration/host/NodeRuntime.h"
#include "libp2p/integration/host/PeerSessionManager.h"
#include "base/people/MeshHopPolicy.h"

#include <functional>
#include <memory>
#include <string>

namespace pbr {

class AgentSession;
class RelayDirectoryKemKeyResolver;
class RelayDirectorySigningKeyResolver;
class SqlitePskSessionStore;

class MessagingHub : public Module {
public:
  MessagingHub();
  ~MessagingHub();

  /** Core store/inbox/AI — profile vault unlock is separate (ProfileSecretsService). */
  Roe<void> Initialize(const AppConfig& config, const std::string& profile_data_dir);
  Roe<void> Reinitialize(const AppConfig& config, const std::string& profile_data_dir);
  void Shutdown();
  bool IsInitialized() const { return initialized_; }

  /** libp2p / P2P stack ready after profile unlock + identity load. */
  bool IsMessagingReady() const { return messaging_ready_; }

  /** Requires ProfileSecretsService unlocked; loads identity and starts libp2p. */
  Roe<void> EnsureMessagingReady();

  InboxController& Inbox();
  P2pMessagingService& P2p();
  GroupMembershipService& Groups();
  CallSessionManager* Calls();
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
  Libp2pHost* Libp2p();
  PeerSessionManager* Sessions();
  /** Last libp2p start failure (empty if ok). For Network settings UX. */
  const std::string& LastLibp2pError() const { return libp2p_last_error_; }

  ReachabilitySnapshot Reachability() const;
  void RunReachabilityProbe(bool try_upnp);
  void TryUpnpPortMapping();
  void TickReachabilityUx();
  void RefreshMeshCapabilities();
  /**
   * Hot-apply persisted AppConfig owned by messaging (HTTP clients + mesh caps).
   * No-ops unchanged slices so LLM/integrations saves do not rebuild relays.
   */
  void ApplyRuntimeConfig(const AppConfig& config);
  /** Hot-apply profile prefs messaging owns (group invite policy, push preference). */
  void ApplyProfilePrefs(const ProfilePreferences& prefs);
  DialBackService* DialBack() { return dial_back_.get(); }
  CircuitRelayService* CircuitRelay() { return circuit_relay_.get(); }
  MediaRelayService* MediaRelay() { return media_relay_.get(); }

  /**
   * nf: try circuit bridge via preferred hops (contacts then seed when prefer_contacts).
   * Registers hop endpoints as needed; returns first successful bridge.
   */
  Roe<CircuitRelayBridgeResult> RequestCircuitBridgePreferred(const std::string& target_multiaddr,
                                                              int timeout_ms = 8000);

  void SetOnReachabilityUpdated(std::function<void()> callback);

  void BindAgent(AgentSession& agent);
  PeerSigningKeyStore& SigningKeys();

  /** Idle sweep / session policy tick (call from UI loop). */
  void TickLibp2p();
  /** Drop cold peer connections (Android background). */
  void SuspendLibp2pColdPeers();

  void SetOnMessagingReady(std::function<void()> callback);

private:
  void InstallServiceClients(const AppConfig& config);
  void UpdateServiceClients(const AppConfig& config);
  void WireRelayAuthSigner();
  Roe<void> StartLibp2p(const AppConfig& config);
  void StopLibp2p();
  void StartMeshServices(Libp2pRole role);
  void ApplyMeshAdmissionPolicies();
  void WireCallMediaRelayDeps();
  void RegisterContactEndpoints();
  Roe<void> BuildMessagingStack();
  void NotifyMessagingReady();

  std::string data_dir_;
  std::string profile_id_;
  AppConfig config_;
  AgentSession* agent_ = nullptr;
  std::unique_ptr<SqliteThreadStore> store_;
  std::unique_ptr<ContactsStore> contacts_;
  std::unique_ptr<IdentityStore> identity_;
  PeerSigningKeyStore signing_key_store_;
  PeerKemKeyStore kem_key_store_;
  std::unique_ptr<SqlitePskSessionStore> psk_store_;
  std::unique_ptr<GroupRosterStore> group_roster_;
  std::unique_ptr<CallSessionStore> call_session_store_;
  std::unique_ptr<CallMediaKeyStore> call_media_keys_;
  std::unique_ptr<CallMediaEngine> call_media_engine_;
  std::unique_ptr<CallSessionManager> call_sessions_;
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
  std::unique_ptr<NodeRuntime> node_runtime_;
  std::unique_ptr<DialBackService> dial_back_;
  std::unique_ptr<CircuitRelayService> circuit_relay_;
  std::unique_ptr<MediaRelayService> media_relay_;
  ReachabilityService reachability_;
  std::string libp2p_last_error_;
  bool upnp_auto_tried_ = false;
  bool reachability_banner_shown_ = false;
  uint64_t reachability_outbound_since_ms_ = 0;
  std::function<void()> on_reachability_updated_;
  std::unique_ptr<P2pMessagingService> p2p_;
  std::unique_ptr<ContactActionDispatcher> actions_;
  std::unique_ptr<MessageRouter> router_;
  std::function<void()> on_messaging_ready_;
  bool initialized_ = false;
  bool messaging_ready_ = false;
  bool applied_show_notifications_known_ = false;
  bool applied_show_notifications_ = true;
};

} // namespace pbr
