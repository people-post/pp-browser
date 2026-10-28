#include "feature/conversations/ContactActionDispatcher.h"

#include "lib/messaging/GroupTypes.h"
#include "common/chat/MessagingJson.h"
#include "domain/registration/RegistrationClient.h"
#include "lib/people/ContactIdentity.h"
#include "lib/people/ContactJson.h"
#include "lib/people/ContactTypes.h"
#include "feature/conversations/GroupMembershipWorkflow.h"
#include "feature/conversations/MeshDeliveryOrchestrator.h"
#include "foundation/runtime/AppRuntime.h"

#include "common/ValueJson.h"
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

ContactActionDispatcher::ContactActionDispatcher(InboxController& inbox, ContactsStore& contacts,
                                                 IdentityStore& identity, IThreadStore& store,
                                                 GroupMembershipWorkflow* groups, IRegistrationClient* registration,
                                                 MeshDeliveryOrchestrator* mesh_messaging,
                                                 mesh::IDirectoryClient* directory)
    : inbox_(inbox), contacts_(contacts), identity_(identity), store_(store), groups_(groups),
      registration_(registration), mesh_messaging_(mesh_messaging), directory_(directory) {
  redirectLogger("ContactActionDispatcher");
}

void ContactActionDispatcher::SetRegistrationClient(IRegistrationClient* registration) {
  registration_ = registration;
}

void ContactActionDispatcher::SetGroupMembership(GroupMembershipWorkflow* groups) {
  groups_ = groups;
}

void ContactActionDispatcher::SetOnActionMessage(std::function<void(const std::string& message)> callback) {
  on_action_message_ = std::move(callback);
}

namespace {

void RegisterKeysFromHit(MeshDeliveryOrchestrator* mesh_messaging, const mesh::DirectoryHit& hit) {
  if (!mesh_messaging) {
    return;
  }
  mesh_messaging->RegisterPeerKeysFromDirectory(hit);
}

/** Blocking: HTTP. */
Roe<mesh::DirectoryHit> LookUpAccount(mesh::IDirectoryClient* directory, const std::string& account_id) {
  if (!directory) {
    return Error("Directory not configured");
  }
  return directory->LookupByAccount(account_id);
}

std::optional<std::string> AccountToLookUp(const Object& payload) {
  const std::string type = payload.getString("type").value_or("");
  if (type != "add_contact" && !(type == "start_conversation" && !payload.getString("contact_id"))) {
    return std::nullopt;
  }
  auto account_id = payload.getString("account_id");
  if (!account_id || account_id->empty()) {
    return std::nullopt;
  }
  return account_id;
}

} // namespace

std::optional<std::string> DirectoryAccountToLookUp(const std::string& payload_json) {
  const auto payload = TryParseObject(payload_json);
  return payload ? AccountToLookUp(*payload) : std::nullopt;
}

ContactActionDispatcher::DispatchResult ContactActionDispatcher::Dispatch(const std::string& payload_json) {
  const auto payload = TryParseObject(payload_json);
  if (!payload) {
    return DispatchResult(std::optional<std::string>{});
  }
  std::optional<mesh::DirectoryHit> looked_up;
  if (const auto account_id = AccountToLookUp(*payload)) {
    auto hit = LookUpAccount(directory_, *account_id);
    if (!hit) {
      return hit.error();
    }
    looked_up = std::move(*hit);
  }
  return Run(*payload, looked_up);
}

void ContactActionDispatcher::DispatchAsync(const std::string& payload_json,
                                            std::function<void(DispatchResult)> done) {
  const auto payload = TryParseObject(payload_json);
  const auto account_id = payload ? AccountToLookUp(*payload) : std::nullopt;
  if (!account_id) {
    done(payload ? Run(*payload, std::nullopt) : DispatchResult(std::optional<std::string>{}));
    return;
  }
  // The directory is an HTTP call: off the UI thread. The contact and the thread are made back on it.
  AppRuntime::PostWorkerAndReplyOnUI<Roe<mesh::DirectoryHit>>(
      WorkerLane::Normal,
      [directory = directory_, account_id = *account_id]() { return LookUpAccount(directory, account_id); },
      [this, alive = std::weak_ptr<int>(alive_), action = *payload,
       done = std::move(done)](Roe<mesh::DirectoryHit> hit) {
        if (!alive.lock()) {
          return;
        }
        if (!hit) {
          done(hit.error());
          return;
        }
        done(Run(action, std::optional<mesh::DirectoryHit>(std::move(*hit))));
      });
}

ContactActionDispatcher::DispatchResult ContactActionDispatcher::Run(
    const Object& payload, const std::optional<mesh::DirectoryHit>& looked_up) {
  auto type = payload.getString("type");
  if (!type) {
    return Roe<std::optional<std::string>>(std::optional<std::string>{});
  }

  if (*type == "start_conversation") {
    std::string contact_id;
    ThreadChannel channel = ThreadChannel::E2ePublic;
    if (auto cid = payload.getString("contact_id")) {
      contact_id = *cid;
    } else if (looked_up) {
      RegisterKeysFromHit(mesh_messaging_, *looked_up);
      auto contact = contacts_.AddFromDirectoryHit(*looked_up);
      if (!contact) {
        return contact.error();
      }
      if (mesh_messaging_) {
        mesh_messaging_->RegisterContactDirectEndpoints(*contact);
      }
      contact_id = contact->id;
    } else {
      return Error("Missing contact_id or account_id");
    }
    auto thread = inbox_.FindOrCreateDirectThread(contact_id, channel);
    if (!thread) {
      return thread.error();
    }
    if (mesh_messaging_) {
      auto contact = contacts_.Get(contact_id);
      if (contact && *contact) {
        mesh_messaging_->RegisterContactDirectEndpoints(**contact);
      }
      mesh_messaging_->WarmPeerForThread(thread->id);
    }
    if (on_action_message_) {
      on_action_message_("Opened conversation with " + thread->title);
    }
    return Roe<std::optional<std::string>>(std::optional<std::string>{});
  }

  if (*type == "secure_message") {
    auto contact_id = payload.getString("contact_id");
    if (!contact_id) {
      return Error("Missing contact_id");
    }
    auto thread = inbox_.FindOrCreateDirectThread(*contact_id, ThreadChannel::E2e);
    if (!thread) {
      return thread.error();
    }
    if (mesh_messaging_) {
      auto contact = contacts_.Get(*contact_id);
      if (contact && *contact) {
        mesh_messaging_->RegisterContactDirectEndpoints(**contact);
      }
      (void)mesh_messaging_->EnsurePskGenerated(thread->id);
      mesh_messaging_->WarmPeerForThread(thread->id);
    }
    if (on_action_message_) {
      on_action_message_("Opened secure conversation with " + thread->title);
    }
    return Roe<std::optional<std::string>>(std::optional<std::string>{});
  }

  if (*type == "add_contact") {
    if (!looked_up) {
      return Error("Missing account_id");
    }
    RegisterKeysFromHit(mesh_messaging_, *looked_up);
    auto contact = contacts_.AddFromDirectoryHit(*looked_up);
    if (!contact) {
      return contact.error();
    }
    if (mesh_messaging_) {
      mesh_messaging_->RegisterContactDirectEndpoints(*contact);
    }
    if (on_action_message_) {
      on_action_message_("Added " + contact->display_name + " to contacts");
    }
    return Roe<std::optional<std::string>>(std::optional<std::string>{});
  }

  if (*type == "show_contact") {
    auto contact_id = payload.getString("contact_id");
    if (!contact_id) {
      return Error("Missing contact_id");
    }
    auto contact = contacts_.Get(*contact_id);
    if (!contact || !*contact) {
      return Error("Contact not found");
    }
    return Roe<std::optional<std::string>>(DumpJson(ContactToJson(**contact)));
  }

  if (*type == "open_conversation") {
    auto thread_id = payload.getString("thread_id");
    if (!thread_id) {
      return Error("Missing thread_id");
    }
    auto thread = inbox_.OpenThread(*thread_id);
    if (!thread) {
      return thread.error();
    }
    if (on_action_message_) {
      on_action_message_("Opened " + thread->title);
    }
    return Roe<std::optional<std::string>>(std::optional<std::string>{});
  }

  if (*type == "register_user") {
    auto identity = identity_.Get();
    if (!identity) {
      return identity.error();
    }
    if (!registration_) {
      return Error("Registration client not configured");
    }

    auto result = FinishAndPersistRegistration(*registration_, identity_, identity->nickname);
    if (!result) {
      return result.error();
    }
    if (on_action_message_) {
      on_action_message_(result->registered ? "Registered on network" : "Registration failed");
    }
    return Roe<std::optional<std::string>>(std::optional<std::string>{});
  }

  if (*type == "accept_group_invite") {
    auto invite_nonce = payload.getString("invite_nonce");
    if (!invite_nonce) {
      return Error("Missing invite_nonce");
    }
    if (!groups_) {
      return Error("Messaging not initialized");
    }
    auto thread = groups_->AcceptInvite(*invite_nonce);
    if (!thread) {
      return thread.error();
    }
    (void)inbox_.OpenThread(thread->id);
    if (on_action_message_) {
      on_action_message_("Joined " + thread->title);
    }
    return Roe<std::optional<std::string>>(std::optional<std::string>{});
  }

  if (*type == "decline_group_invite") {
    auto invite_nonce = payload.getString("invite_nonce");
    if (!invite_nonce) {
      return Error("Missing invite_nonce");
    }
    if (!groups_) {
      return Error("Messaging not initialized");
    }
    if (auto declined = groups_->DeclineInvite(*invite_nonce); !declined) {
      return declined.error();
    }
    if (on_action_message_) {
      on_action_message_("Declined group invitation");
    }
    return Roe<std::optional<std::string>>(std::optional<std::string>{});
  }

  if (*type == "block_group_inviter") {
    auto inviter_identity = payload.getString("inviter_identity");
    if (!inviter_identity) {
      return Error("Missing inviter_identity");
    }
    auto contacts = contacts_.List();
    if (!contacts) {
      return contacts.error();
    }
    bool updated = false;
    for (Contact& contact : *contacts) {
      for (const mesh::ContactId& id : contact.ids) {
        if (id.value == *inviter_identity) {
          contact.trust = TrustLevel::Blocked;
          if (auto saved = contacts_.Upsert(contact); !saved) {
            return saved.error();
          }
          updated = true;
          break;
        }
      }
    }
    if (auto invite_nonce = payload.getString("invite_nonce"); invite_nonce && groups_) {
      (void)groups_->DeclineInvite(*invite_nonce);
      (void)groups_->ResolveInviteCard(*inviter_identity, *invite_nonce, InviteStatus::Blocked,
                                       "You blocked the inviter");
    }
    if (on_action_message_) {
      on_action_message_(updated ? "Blocked inviter" : "Blocked unknown inviter");
    }
    return Roe<std::optional<std::string>>(std::optional<std::string>{});
  }

  if (*type == "create_group") {
    auto title = payload.getString("title");
    if (!title) {
      return Error("Missing title");
    }
    std::vector<std::string> member_contact_ids;
    if (const Array* members = payload.getArray("member_contact_ids")) {
      for (const Value& entry : members->elements) {
        if (auto s = asString(entry)) {
          member_contact_ids.push_back(*s);
        }
      }
    }
    auto thread = inbox_.CreateGroup(*title, member_contact_ids);
    if (!thread) {
      return thread.error();
    }
    if (on_action_message_) {
      on_action_message_("Created group " + thread->title);
    }
    return Roe<std::optional<std::string>>(std::optional<std::string>{});
  }

  if (*type == "fork_group") {
    auto group_id = payload.getString("group_id");
    if (!group_id) {
      return Error("Missing group_id");
    }
    if (!groups_) {
      return Error("Messaging not initialized");
    }
    auto roster = groups_->ListRoster(*group_id);
    std::string old_title = "Group";
    if (auto thread = store_.FindGroupThread(*group_id); thread && *thread) {
      old_title = (*thread)->title.empty() ? old_title : (*thread)->title;
    }
    const std::string new_title = old_title + " (continued)";
    std::vector<std::string> member_contact_ids;
    if (roster) {
      auto local = identity_.Get();
      for (const GroupRosterMember& member : *roster) {
        if (local && member.member_identity == local->account_id) {
          continue;
        }
        if (groups_->IsMemberUnreachable(*group_id, member.member_identity)) {
          continue;
        }
        if (auto contact = contacts_.FindByIdentity(member.member_identity, mesh::ContactIdKind::Account)) {
          if (*contact) {
            member_contact_ids.push_back((*contact)->id);
          }
        }
      }
    }
    auto forked = groups_->ForkGroup(*group_id, new_title, member_contact_ids);
    if (!forked) {
      return forked.error();
    }
    (void)groups_->ResolveOwnerUnreachableAdvisory(*group_id);
    (void)inbox_.OpenThread(forked->id);
    if (on_action_message_) {
      on_action_message_("Started " + forked->title + " (fresh history)");
    }
    return Roe<std::optional<std::string>>(std::optional<std::string>{});
  }

  if (*type == "message_group_owner") {
    auto owner_identity = payload.getString("owner_identity");
    if (!owner_identity) {
      return Error("Missing owner_identity");
    }
    if (!groups_) {
      return Error("Messaging not initialized");
    }
    auto thread = groups_->OpenOwnerDirectMessage(*owner_identity);
    if (!thread) {
      return thread.error();
    }
    (void)inbox_.OpenThread(thread->id);
    if (on_action_message_) {
      on_action_message_("Opened chat with owner");
    }
    return Roe<std::optional<std::string>>(std::optional<std::string>{});
  }

  if (*type == "dismiss_owner_advisory") {
    auto group_id = payload.getString("group_id");
    if (!group_id) {
      return Error("Missing group_id");
    }
    if (!groups_) {
      return Error("Messaging not initialized");
    }
    if (auto resolved = groups_->ResolveOwnerUnreachableAdvisory(*group_id); !resolved) {
      return resolved.error();
    }
    inbox_.NotifyThreadChanged();
    return Roe<std::optional<std::string>>(std::optional<std::string>{});
  }

  return Roe<std::optional<std::string>>(std::optional<std::string>{});
}

} // namespace pbr
