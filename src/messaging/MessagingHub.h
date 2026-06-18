#pragma once

#include "app/Config.h"
#include "contacts/ContactsStore.h"
#include "identity/IdentityStore.h"
#include "messaging/ContactActionDispatcher.h"
#include "messaging/InboxController.h"
#include "messaging/JsonThreadStore.h"
#include "messaging/MessageRouter.h"
#include "messaging/P2pMessagingService.h"
#include "net/ServiceClientsImpl.h"

#include <memory>
#include <string>

namespace pbr {

class AgentSession;

class MessagingHub {
public:
  static MessagingHub& Instance();

  Roe<void> Initialize(const AppConfig& config);
  void Shutdown();

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
