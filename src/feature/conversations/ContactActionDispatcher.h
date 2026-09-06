#pragma once

#include "common/Module.h"
#include "common/thread/IThreadStore.h"
#include "domain/people/ContactsStore.h"
#include "domain/people/IdentityStore.h"
#include "feature/conversations/InboxController.h"
#include "feature/conversations/MeshDeliveryOrchestrator.h"
#include "domain/net/OrgBackendClients.h"

#include <functional>
#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

class GroupMembershipWorkflow;

class ContactActionDispatcher : public Module {
public:
  ContactActionDispatcher(InboxController& inbox, ContactsStore& contacts, IdentityStore& identity,
                          IThreadStore& store, GroupMembershipWorkflow* groups,
                          IRegistrationClient* registration, MeshDeliveryOrchestrator* mesh_messaging = nullptr);

  Roe<std::optional<std::string>> Dispatch(const std::string& payload_json);
  void SetRegistrationClient(IRegistrationClient* registration);
  void SetGroupMembership(GroupMembershipWorkflow* groups);
  void SetOnActionMessage(std::function<void(const std::string& message)> callback);

private:
  InboxController& inbox_;
  ContactsStore& contacts_;
  IdentityStore& identity_;
  IThreadStore& store_;
  GroupMembershipWorkflow* groups_ = nullptr;
  IRegistrationClient* registration_ = nullptr;
  MeshDeliveryOrchestrator* mesh_messaging_ = nullptr;
  std::function<void(const std::string&)> on_action_message_;
};

} // namespace pbr
