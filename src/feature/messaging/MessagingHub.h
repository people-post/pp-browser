#pragma once

#include "base/data/Config.h"
#include "base/people/ContactsStore.h"
#include "base/people/IdentityStore.h"
#include "feature/messaging/ContactActionDispatcher.h"
#include "feature/messaging/InboxController.h"
#include "base/messaging/PeerSigningKeyStore.h"
#include "base/messaging/SqliteThreadStore.h"
#include "feature/messaging/MessageRouter.h"
#include "feature/messaging/P2pMessagingService.h"
#include "base/net/McpDirectoryClient.h"
#include "base/net/McpRegistrationClient.h"
#include "base/net/McpRelayClient.h"
#include "base/net/ServiceClientsImpl.h"

#include <memory>
#include <string>

namespace pbr {

class AgentSession;
class McpClient;

class MessagingHub {
public:
  static MessagingHub& Instance();

  Roe<void> Initialize(const AppConfig& config, const std::string& profile_data_dir,
                       McpClient* promoted_mcp = nullptr);
  Roe<void> Reinitialize(const AppConfig& config, const std::string& profile_data_dir, McpClient* promoted_mcp = nullptr);
  void Shutdown();
  bool IsInitialized() const { return initialized_; }

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

  void BindAgent(AgentSession& agent);

private:
  MessagingHub() = default;

  void InstallServiceClients(const AppConfig& config, McpClient* promoted_mcp);
  void UpdateServiceClients(const AppConfig& config, McpClient* promoted_mcp);

  std::string data_dir_;
  AppConfig config_;
  AgentSession* agent_ = nullptr;
  std::unique_ptr<SqliteThreadStore> store_;
  std::unique_ptr<ContactsStore> contacts_;
  std::unique_ptr<IdentityStore> identity_;
  PeerSigningKeyStore signing_key_store_;
  std::unique_ptr<InboxController> inbox_;
  std::unique_ptr<MockRelayClient> mock_relay_;
  std::unique_ptr<MockDirectoryClient> mock_directory_;
  std::unique_ptr<MockRegistrationClient> mock_registration_;
  std::unique_ptr<McpRelayClient> mcp_relay_;
  std::unique_ptr<McpDirectoryClient> mcp_directory_;
  std::unique_ptr<McpRegistrationClient> mcp_registration_;
  std::string http_relay_url_;
  std::string http_directory_url_;
  std::string http_registration_url_;
  std::unique_ptr<HttpRelayClient> http_relay_;
  std::unique_ptr<HttpDirectoryClient> http_directory_;
  std::unique_ptr<HttpRegistrationClient> http_registration_;
  IRelayClient* relay_ = nullptr;
  IDirectoryClient* directory_ = nullptr;
  IRegistrationClient* registration_ = nullptr;
  std::unique_ptr<P2pMessagingService> p2p_;
  std::unique_ptr<ContactActionDispatcher> actions_;
  std::unique_ptr<MessageRouter> router_;
  bool initialized_ = false;
};

} // namespace pbr
