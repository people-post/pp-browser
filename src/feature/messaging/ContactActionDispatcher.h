#pragma once

#include "common/Module.h"
#include "contacts/ContactsStore.h"
#include "identity/IdentityStore.h"
#include "messaging/InboxController.h"
#include "net/ServiceClients.h"

#include <functional>
#include <optional>
#include <string>

namespace pbr {

class ContactActionDispatcher : public Module {
public:
  ContactActionDispatcher(InboxController& inbox, ContactsStore& contacts, IdentityStore& identity,
                          IRegistrationClient& registration);

  Roe<std::optional<std::string>> Dispatch(const std::string& payload_json);
  void SetOnActionMessage(std::function<void(const std::string& message)> callback);

private:
  InboxController& inbox_;
  ContactsStore& contacts_;
  IdentityStore& identity_;
  IRegistrationClient& registration_;
  std::function<void(const std::string&)> on_action_message_;
};

} // namespace pbr
