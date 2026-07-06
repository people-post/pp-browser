#pragma once

#include "common/Module.h"
#include "base/people/ContactsStore.h"
#include "base/people/IdentityStore.h"
#include "feature/messaging/InboxController.h"
#include "feature/messaging/P2pMessagingService.h"
#include "base/net/ServiceClients.h"

#include <functional>
#include <optional>
#include <string>

namespace pbr {

class ContactActionDispatcher : public Module {
public:
  ContactActionDispatcher(InboxController& inbox, ContactsStore& contacts, IdentityStore& identity,
                          IRegistrationClient* registration, P2pMessagingService* p2p = nullptr);

  Roe<std::optional<std::string>> Dispatch(const std::string& payload_json);
  void SetRegistrationClient(IRegistrationClient* registration);
  void SetOnActionMessage(std::function<void(const std::string& message)> callback);

private:
  InboxController& inbox_;
  ContactsStore& contacts_;
  IdentityStore& identity_;
  IRegistrationClient* registration_ = nullptr;
  P2pMessagingService* p2p_ = nullptr;
  std::function<void(const std::string&)> on_action_message_;
};

} // namespace pbr
