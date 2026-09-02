#pragma once

#include "common/Module.h"
#include "common/thread/IThreadStore.h"
#include "domain/people/ContactsStore.h"
#include "domain/people/IdentityStore.h"
#include "feature/messaging/InboxController.h"
#include "feature/messaging/MeshMessagingService.h"
#include "base/net/ServiceClients.h"

#include <functional>
#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

class GroupMembershipService;

class ContactActionDispatcher : public Module {
public:
  ContactActionDispatcher(InboxController& inbox, ContactsStore& contacts, IdentityStore& identity,
                          IThreadStore& store, GroupMembershipService* groups,
                          IRegistrationClient* registration, MeshMessagingService* mesh_messaging = nullptr);

  Roe<std::optional<std::string>> Dispatch(const std::string& payload_json);
  void SetRegistrationClient(IRegistrationClient* registration);
  void SetGroupMembership(GroupMembershipService* groups);
  void SetOnActionMessage(std::function<void(const std::string& message)> callback);

private:
  InboxController& inbox_;
  ContactsStore& contacts_;
  IdentityStore& identity_;
  IThreadStore& store_;
  GroupMembershipService* groups_ = nullptr;
  IRegistrationClient* registration_ = nullptr;
  MeshMessagingService* mesh_messaging_ = nullptr;
  std::function<void(const std::string&)> on_action_message_;
};

} // namespace pbr
