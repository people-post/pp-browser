#pragma once

#include "base/data/Config.h"
#include "base/people/ContactsStore.h"
#include "base/people/IdentityStore.h"
#include "feature/messaging/ContactActionDispatcher.h"
#include "feature/messaging/InboxController.h"
#include "base/messaging/PeerKemKeyStore.h"
#include "base/messaging/PeerSigningKeyStore.h"
#include "base/messaging/SqliteThreadStore.h"
#include "feature/messaging/MessageRouter.h"
#include "feature/messaging/P2pMessagingService.h"
#include "base/net/ServiceClientsImpl.h"
#include "libp2p/integration/host/Libp2pHost.h"
#include "libp2p/integration/host/PeerSessionManager.h"

#include <functional>
#include <memory>
#include <string>

namespace pbr {

class AgentSession;
class RelayDirectoryKemKeyResolver;
class RelayDirectorySigningKeyResolver;
class SqlitePskSessionStore;

class MessagingHub {
public:
  static MessagingHub& Instance();

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
  MessageRouter& Router();
  ContactActionDispatcher& Actions();
  bool HasRouter() const { return router_ != nullptr; }
  IThreadStore& Store();
  ContactsStore& Contacts();
  IdentityStore& Identity();
  IDirectoryClient& Directory();
  IRegistrationClient& Registration();
  Libp2pHost* Libp2p();
  PeerSessionManager* Sessions();

  void BindAgent(AgentSession& agent);
  PeerSigningKeyStore& SigningKeys();

  /** Idle sweep / session policy tick (call from UI loop). */
  void TickLibp2p();
  /** Drop cold peer connections (Android background). */
  void SuspendLibp2pColdPeers();

  void SetOnMessagingReady(std::function<void()> callback);

private:
  MessagingHub() = default;

  void InstallServiceClients(const AppConfig& config);
  void UpdateServiceClients(const AppConfig& config);
  void WireRelayAuthSigner();
  Roe<void> StartLibp2p(const AppConfig& config);
  void StopLibp2p();
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
  std::unique_ptr<RelayDirectorySigningKeyResolver> signing_resolver_;
  std::unique_ptr<RelayDirectoryKemKeyResolver> kem_resolver_;
  std::unique_ptr<InboxController> inbox_;
  std::unique_ptr<MockRelayClient> mock_relay_;
  std::unique_ptr<MockDirectoryClient> mock_directory_;
  std::unique_ptr<MockRegistrationClient> mock_registration_;
  std::string http_relay_url_;
  std::string http_directory_url_;
  std::string http_registration_url_;
  std::unique_ptr<HttpRelayClient> http_relay_;
  std::unique_ptr<HttpDirectoryClient> http_directory_;
  std::unique_ptr<HttpRegistrationClient> http_registration_;
  IRelayClient* relay_ = nullptr;
  IDirectoryClient* directory_ = nullptr;
  IRegistrationClient* registration_ = nullptr;
  std::unique_ptr<Libp2pHost> libp2p_host_;
  std::unique_ptr<PeerSessionManager> peer_sessions_;
  std::unique_ptr<P2pMessagingService> p2p_;
  std::unique_ptr<ContactActionDispatcher> actions_;
  std::unique_ptr<MessageRouter> router_;
  std::function<void()> on_messaging_ready_;
  bool initialized_ = false;
  bool messaging_ready_ = false;
};

} // namespace pbr
