#pragma once

#include "common/Module.h"
#include "common/thread/IThreadStore.h"
#include "lib/people/ContactsStore.h"
#include "lib/people/IdentityStore.h"
#include "feature/conversations/InboxController.h"
#include "feature/conversations/MeshDeliveryOrchestrator.h"
#include "lib/net/OrgBackendClients.h"
#include "mesh/types/IDirectoryClient.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

class GroupMembershipWorkflow;

/**
 * The account an action must look up in the directory before it runs: `add_contact`, and
 * `start_conversation` without a `contact_id`, name the person by `account_id` only (a people list stores
 * no keys); the keys come from a fresh lookup. nullopt for every other payload, or when no account is named.
 */
std::optional<std::string> DirectoryAccountToLookUp(const std::string& payload_json);

class ContactActionDispatcher : public Module {
public:
  using DispatchResult = Roe<std::optional<std::string>>;

  /** `directory` (may be null: every lookup then fails) serves the account lookups; it outlives this. */
  ContactActionDispatcher(InboxController& inbox, ContactsStore& contacts, IdentityStore& identity,
                          IThreadStore& store, GroupMembershipWorkflow* groups,
                          IRegistrationClient* registration, MeshDeliveryOrchestrator* mesh_messaging = nullptr,
                          mesh::IDirectoryClient* directory = nullptr);

  /** Runs the action now. One that needs a directory lookup blocks on HTTP: the UI thread uses DispatchAsync. */
  DispatchResult Dispatch(const std::string& payload_json);
  /**
   * For the UI thread: an action that needs a directory lookup (DirectoryAccountToLookUp) looks up on a
   * worker and finishes back on the UI thread; any other finishes before this returns. `done` gets the
   * result, unless this dispatcher is gone by then.
   */
  void DispatchAsync(const std::string& payload_json, std::function<void(DispatchResult)> done);
  void SetRegistrationClient(IRegistrationClient* registration);
  void SetGroupMembership(GroupMembershipWorkflow* groups);
  void SetOnActionMessage(std::function<void(const std::string& message)> callback);

private:
  /** `looked_up`: the directory's answer for DirectoryAccountToLookUp(payload), when that names one. */
  DispatchResult Run(const Object& payload, const std::optional<mesh::DirectoryHit>& looked_up);

  InboxController& inbox_;
  ContactsStore& contacts_;
  IdentityStore& identity_;
  IThreadStore& store_;
  GroupMembershipWorkflow* groups_ = nullptr;
  IRegistrationClient* registration_ = nullptr;
  MeshDeliveryOrchestrator* mesh_messaging_ = nullptr;
  mesh::IDirectoryClient* directory_ = nullptr;
  std::function<void(const std::string&)> on_action_message_;
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

} // namespace pbr
