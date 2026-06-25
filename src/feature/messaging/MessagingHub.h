#pragma once

#include "base/data/Config.h"
#include "base/people/ContactsStore.h"
#include "base/people/IdentityStore.h"
#include "feature/messaging/ContactActionDispatcher.h"
#include "feature/messaging/InboxController.h"
#include "base/messaging/JsonThreadStore.h"
#include "feature/messaging/MessageRouter.h"
#include "feature/messaging/P2pMessagingService.h"
#include "base/net/ServiceClientsImpl.h"

#include <memory>
#include <string>

namespace pbr {

class AgentSession;

class MessagingHub {
public:
  static MessagingHub& Instance();

  Roe<void> Initialize(const AppConfig& config, const std::string& profile_data_dir);
  Roe<void> Reinitialize(const AppConfig& config, const std::string& profile_data_dir);
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

  std::string data_dir_;
  std::unique_ptr<JsonThreadStore> store_;
  std::unique_ptr<ContactsStore> contacts_;
  std::unique_ptr<IdentityStore> identity_;
  std::unique_ptr<InboxController> inbox_;
  std::unique_ptr<MockRelayClient> mock_relay_;
  std::unique_ptr<HttpRelayClient> http_relay_;
  IRelayClient* relay_ = nullptr;
  std::unique_ptr<MockDirectoryClient> mock_directory_;
  std::unique_ptr<HttpDirectoryClient> http_directory_;
  IDirectoryClient* directory_ = nullptr;
  std::unique_ptr<MockRegistrationClient> mock_registration_;
  std::unique_ptr<HttpRegistrationClient> http_registration_;
  IRegistrationClient* registration_ = nullptr;
  std::unique_ptr<P2pMessagingService> p2p_;
  std::unique_ptr<ContactActionDispatcher> actions_;
  std::unique_ptr<MessageRouter> router_;
  bool initialized_ = false;
};

} // namespace pbr
