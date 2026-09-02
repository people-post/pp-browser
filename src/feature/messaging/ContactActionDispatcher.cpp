#include "feature/messaging/ContactActionDispatcher.h"

#include "base/messaging/GroupTypes.h"
#include "common/chat/MessagingJson.h"
#include "base/net/RegistrationClientUtil.h"
#include "base/people/ContactIdentity.h"
#include "base/people/ContactTypes.h"
#include "feature/messaging/GroupMembershipService.h"
#include "feature/messaging/MeshMessagingService.h"

#include "common/ValueJson.h"
#include "common/PbrCompat.h"

namespace pbr {

ContactActionDispatcher::ContactActionDispatcher(InboxController& inbox, ContactsStore& contacts,
                                                 IdentityStore& identity, IThreadStore& store,
                                                 GroupMembershipService* groups, IRegistrationClient* registration,
                                                 MeshMessagingService* mesh_messaging)
    : inbox_(inbox), contacts_(contacts), identity_(identity), store_(store), groups_(groups),
      registration_(registration), mesh_messaging_(mesh_messaging) {
  redirectLogger("ContactActionDispatcher");
}

void ContactActionDispatcher::SetRegistrationClient(IRegistrationClient* registration) {
  registration_ = registration;
}

void ContactActionDispatcher::SetGroupMembership(GroupMembershipService* groups) {
  groups_ = groups;
}

void ContactActionDispatcher::SetOnActionMessage(std::function<void(const std::string& message)> callback) {
  on_action_message_ = std::move(callback);
}

namespace {

void RegisterKeysFromHit(MeshMessagingService* mesh_messaging, const DirectoryHit& hit) {
  if (!mesh_messaging) {
    return;
  }
  const auto account_id = PrimaryAccountIdFromHit(hit);
  if (!account_id) {
    return;
  }
  if (hit.signing_public_key_b64 && !hit.signing_public_key_b64->empty()) {
    mesh_messaging->RegisterPeerSigningKey(ContactIdKindToString(ContactIdKind::Account), *account_id,
                                *hit.signing_public_key_b64, "directory");
  }
  if (hit.kem_public_key_b64 && !hit.kem_public_key_b64->empty()) {
    mesh_messaging->RegisterPeerKemKey(ContactIdKindToString(ContactIdKind::Account), *account_id, *hit.kem_public_key_b64,
                            "directory");
  }
}

} // namespace

Roe<std::optional<std::string>> ContactActionDispatcher::Dispatch(const std::string& payload_json) {
  auto payload = TryParseObject(payload_json);
  auto type = payload ? payload->getString("type") : std::nullopt;
  if (!type) {
    return Roe<std::optional<std::string>>(std::optional<std::string>{});
  }

  if (*type == "start_conversation") {
    std::string contact_id;
    ThreadChannel channel = ThreadChannel::E2ePublic;
    if (auto cid = payload->getString("contact_id")) {
      contact_id = *cid;
    } else if (const Object* hit_obj = payload->getObject("directory_hit")) {
      const DirectoryHit hit = DirectoryHitFromJson(*hit_obj);
      RegisterKeysFromHit(mesh_messaging_, hit);
      auto contact = contacts_.AddFromDirectoryHit(hit);
      if (!contact) {
        return contact.error();
      }
      if (mesh_messaging_) {
        mesh_messaging_->RegisterContactDirectEndpoints(*contact);
      }
      contact_id = contact->id;
    } else {
      return Error("Missing contact_id or directory_hit");
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
    auto contact_id = payload->getString("contact_id");
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
    const Object* hit_obj = payload->getObject("directory_hit");
    if (!hit_obj) {
      return Error("Missing directory_hit");
    }
    const DirectoryHit hit = DirectoryHitFromJson(*hit_obj);
    RegisterKeysFromHit(mesh_messaging_, hit);
    auto contact = contacts_.AddFromDirectoryHit(hit);
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
    auto contact_id = payload->getString("contact_id");
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
    auto thread_id = payload->getString("thread_id");
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
    auto invite_nonce = payload->getString("invite_nonce");
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
    auto invite_nonce = payload->getString("invite_nonce");
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
    auto inviter_identity = payload->getString("inviter_identity");
    if (!inviter_identity) {
      return Error("Missing inviter_identity");
    }
    auto contacts = contacts_.List();
    if (!contacts) {
      return contacts.error();
    }
    bool updated = false;
    for (Contact& contact : *contacts) {
      for (const ContactId& id : contact.ids) {
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
    if (auto invite_nonce = payload->getString("invite_nonce"); invite_nonce && groups_) {
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
    auto title = payload->getString("title");
    if (!title) {
      return Error("Missing title");
    }
    std::vector<std::string> member_contact_ids;
    if (const Array* members = payload->getArray("member_contact_ids")) {
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
    auto group_id = payload->getString("group_id");
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
        if (auto contact = contacts_.FindByIdentity(member.member_identity, ContactIdKind::Account)) {
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
    auto owner_identity = payload->getString("owner_identity");
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
    auto group_id = payload->getString("group_id");
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
