#include "feature/messaging/ContactActionDispatcher.h"

#include "base/messaging/GroupTypes.h"
#include "base/messaging/MessagingJson.h"
#include "base/net/RegistrationClientUtil.h"
#include "base/people/ContactTypes.h"
#include "feature/messaging/MessagingHub.h"
#include "feature/messaging/P2pMessagingService.h"

#include <nlohmann/json.hpp>

namespace pbr {

ContactActionDispatcher::ContactActionDispatcher(InboxController& inbox, ContactsStore& contacts,
                                                 IdentityStore& identity, IRegistrationClient* registration,
                                                 P2pMessagingService* p2p)
    : inbox_(inbox), contacts_(contacts), identity_(identity), registration_(registration), p2p_(p2p) {
  redirectLogger("ContactActionDispatcher");
}

void ContactActionDispatcher::SetRegistrationClient(IRegistrationClient* registration) {
  registration_ = registration;
}

void ContactActionDispatcher::SetOnActionMessage(std::function<void(const std::string& message)> callback) {
  on_action_message_ = std::move(callback);
}

namespace {

std::optional<std::string> PrimaryRelayIdFromHit(const DirectoryHit& hit) {
  for (const ContactId& id : hit.ids) {
    if (id.kind == ContactIdKind::RelayUser && id.primary) {
      return id.value;
    }
  }
  for (const ContactId& id : hit.ids) {
    if (id.kind == ContactIdKind::RelayUser) {
      return id.value;
    }
  }
  return std::nullopt;
}

} // namespace

Roe<std::optional<std::string>> ContactActionDispatcher::Dispatch(const std::string& payload_json) {
  const nlohmann::json payload = nlohmann::json::parse(payload_json, nullptr, false);
  if (payload.is_discarded() || !payload.contains("type") || !payload["type"].is_string()) {
    return Roe<std::optional<std::string>>(std::optional<std::string>{});
  }

  const std::string type = payload["type"].get<std::string>();

  if (type == "start_conversation") {
    std::string contact_id;
    ThreadChannel channel = ThreadChannel::E2ePublic;
    if (payload.contains("contact_id") && payload["contact_id"].is_string()) {
      contact_id = payload["contact_id"].get<std::string>();
    } else     if (payload.contains("directory_hit") && payload["directory_hit"].is_object()) {
      const DirectoryHit hit = DirectoryHitFromJson(payload["directory_hit"]);
      if (p2p_ && hit.signing_public_key_b64 && !hit.signing_public_key_b64->empty()) {
        if (auto relay_id = PrimaryRelayIdFromHit(hit)) {
          p2p_->RegisterPeerSigningKey(ContactIdKindToString(ContactIdKind::RelayUser), *relay_id,
                                       *hit.signing_public_key_b64, "directory");
        }
      }
      if (p2p_ && hit.kem_public_key_b64 && !hit.kem_public_key_b64->empty()) {
        if (auto relay_id = PrimaryRelayIdFromHit(hit)) {
          p2p_->RegisterPeerKemKey(ContactIdKindToString(ContactIdKind::RelayUser), *relay_id,
                                  *hit.kem_public_key_b64, "directory");
        }
      }
      auto contact = contacts_.AddFromDirectoryHit(hit);
      if (!contact) {
        return contact.error();
      }
      if (p2p_) {
        p2p_->RegisterContactDirectEndpoints(*contact);
      }
      contact_id = contact->id;
    } else {
      return Error("Missing contact_id or directory_hit");
    }
    auto thread = inbox_.FindOrCreateDirectThread(contact_id, channel);
    if (!thread) {
      return thread.error();
    }
    if (p2p_) {
      auto contact = contacts_.Get(contact_id);
      if (contact && *contact) {
        p2p_->RegisterContactDirectEndpoints(**contact);
      }
      p2p_->WarmPeerForThread(thread->id);
    }
    if (on_action_message_) {
      on_action_message_("Opened conversation with " + thread->title);
    }
    return Roe<std::optional<std::string>>(std::optional<std::string>{});
  }

  if (type == "secure_message") {
    if (!payload.contains("contact_id") || !payload["contact_id"].is_string()) {
      return Error("Missing contact_id");
    }
    auto thread = inbox_.FindOrCreateDirectThread(payload["contact_id"].get<std::string>(), ThreadChannel::E2e);
    if (!thread) {
      return thread.error();
    }
    if (p2p_) {
      auto contact = contacts_.Get(payload["contact_id"].get<std::string>());
      if (contact && *contact) {
        p2p_->RegisterContactDirectEndpoints(**contact);
      }
      (void)p2p_->EnsurePskGenerated(thread->id);
      p2p_->WarmPeerForThread(thread->id);
    }
    if (on_action_message_) {
      on_action_message_("Opened secure conversation with " + thread->title);
    }
    return Roe<std::optional<std::string>>(std::optional<std::string>{});
  }

  if (type == "add_contact") {
    if (!payload.contains("directory_hit") || !payload["directory_hit"].is_object()) {
      return Error("Missing directory_hit");
    }
    const DirectoryHit hit = DirectoryHitFromJson(payload["directory_hit"]);
    if (p2p_ && hit.signing_public_key_b64 && !hit.signing_public_key_b64->empty()) {
      if (auto relay_id = PrimaryRelayIdFromHit(hit)) {
        p2p_->RegisterPeerSigningKey(ContactIdKindToString(ContactIdKind::RelayUser), *relay_id,
                                     *hit.signing_public_key_b64, "directory");
      }
    }
    if (p2p_ && hit.kem_public_key_b64 && !hit.kem_public_key_b64->empty()) {
      if (auto relay_id = PrimaryRelayIdFromHit(hit)) {
        p2p_->RegisterPeerKemKey(ContactIdKindToString(ContactIdKind::RelayUser), *relay_id,
                                *hit.kem_public_key_b64, "directory");
      }
    }
    auto contact = contacts_.AddFromDirectoryHit(hit);
    if (!contact) {
      return contact.error();
    }
    if (p2p_) {
      p2p_->RegisterContactDirectEndpoints(*contact);
    }
    if (on_action_message_) {
      on_action_message_("Added " + contact->display_name + " to contacts");
    }
    return Roe<std::optional<std::string>>(std::optional<std::string>{});
  }

  if (type == "show_contact") {
    if (!payload.contains("contact_id") || !payload["contact_id"].is_string()) {
      return Error("Missing contact_id");
    }
    auto contact = contacts_.Get(payload["contact_id"].get<std::string>());
    if (!contact || !*contact) {
      return Error("Contact not found");
    }
    return Roe<std::optional<std::string>>(ContactToJson(**contact).dump());
  }

  if (type == "open_conversation") {
    if (!payload.contains("thread_id") || !payload["thread_id"].is_string()) {
      return Error("Missing thread_id");
    }
    auto thread = inbox_.OpenThread(payload["thread_id"].get<std::string>());
    if (!thread) {
      return thread.error();
    }
    if (on_action_message_) {
      on_action_message_("Opened " + thread->title);
    }
    return Roe<std::optional<std::string>>(std::optional<std::string>{});
  }

  if (type == "register_user") {
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

  if (type == "accept_group_invite") {
    if (!payload.contains("invite_nonce") || !payload["invite_nonce"].is_string()) {
      return Error("Missing invite_nonce");
    }
    if (!MessagingHub::Instance().IsInitialized()) {
      return Error("Messaging not initialized");
    }
    auto thread = MessagingHub::Instance().Groups().AcceptInvite(payload["invite_nonce"].get<std::string>());
    if (!thread) {
      return thread.error();
    }
    (void)inbox_.OpenThread(thread->id);
    if (on_action_message_) {
      on_action_message_("Joined " + thread->title);
    }
    return Roe<std::optional<std::string>>(std::optional<std::string>{});
  }

  if (type == "decline_group_invite") {
    if (!payload.contains("invite_nonce") || !payload["invite_nonce"].is_string()) {
      return Error("Missing invite_nonce");
    }
    if (!MessagingHub::Instance().IsInitialized()) {
      return Error("Messaging not initialized");
    }
    if (auto declined = MessagingHub::Instance().Groups().DeclineInvite(payload["invite_nonce"].get<std::string>());
        !declined) {
      return declined.error();
    }
    if (on_action_message_) {
      on_action_message_("Declined group invitation");
    }
    return Roe<std::optional<std::string>>(std::optional<std::string>{});
  }

  if (type == "block_group_inviter") {
    if (!payload.contains("inviter_identity") || !payload["inviter_identity"].is_string()) {
      return Error("Missing inviter_identity");
    }
    const std::string inviter_identity = payload["inviter_identity"].get<std::string>();
    auto contacts = contacts_.List();
    if (!contacts) {
      return contacts.error();
    }
    bool updated = false;
    for (Contact& contact : *contacts) {
      for (const ContactId& id : contact.ids) {
        if (id.value == inviter_identity) {
          contact.trust = TrustLevel::Blocked;
          if (auto saved = contacts_.Upsert(contact); !saved) {
            return saved.error();
          }
          updated = true;
          break;
        }
      }
    }
    if (payload.contains("invite_nonce") && payload["invite_nonce"].is_string() &&
        MessagingHub::Instance().IsInitialized()) {
      const std::string invite_nonce = payload["invite_nonce"].get<std::string>();
      (void)MessagingHub::Instance().Groups().DeclineInvite(invite_nonce);
      (void)MessagingHub::Instance().Groups().ResolveInviteCard(inviter_identity, invite_nonce,
                                                                InviteStatus::Blocked, "You blocked the inviter");
    }
    if (on_action_message_) {
      on_action_message_(updated ? "Blocked inviter" : "Blocked unknown inviter");
    }
    return Roe<std::optional<std::string>>(std::optional<std::string>{});
  }

  if (type == "create_group") {
    if (!payload.contains("title") || !payload["title"].is_string()) {
      return Error("Missing title");
    }
    std::vector<std::string> member_contact_ids;
    if (payload.contains("member_contact_ids") && payload["member_contact_ids"].is_array()) {
      for (const nlohmann::json& entry : payload["member_contact_ids"]) {
        if (entry.is_string()) {
          member_contact_ids.push_back(entry.get<std::string>());
        }
      }
    }
    auto thread = inbox_.CreateGroup(payload["title"].get<std::string>(), member_contact_ids);
    if (!thread) {
      return thread.error();
    }
    if (on_action_message_) {
      on_action_message_("Created group " + thread->title);
    }
    return Roe<std::optional<std::string>>(std::optional<std::string>{});
  }

  if (type == "fork_group") {
    if (!payload.contains("group_id") || !payload["group_id"].is_string()) {
      return Error("Missing group_id");
    }
    if (!MessagingHub::Instance().IsInitialized()) {
      return Error("Messaging not initialized");
    }
    const std::string group_id = payload["group_id"].get<std::string>();
    auto roster = MessagingHub::Instance().Groups().ListRoster(group_id);
    std::string old_title = "Group";
    if (auto thread = MessagingHub::Instance().Store().FindGroupThread(group_id); thread && *thread) {
      old_title = (*thread)->title.empty() ? old_title : (*thread)->title;
    }
    const std::string new_title = old_title + " (continued)";
    std::vector<std::string> member_contact_ids;
    if (roster) {
      auto local = MessagingHub::Instance().Identity().Get();
      for (const GroupRosterMember& member : *roster) {
        if (local && member.member_identity == local->relay_user_id) {
          continue;
        }
        if (MessagingHub::Instance().Groups().IsMemberUnreachable(group_id, member.member_identity)) {
          continue;
        }
        if (auto contact = MessagingHub::Instance().Contacts().FindByIdentity(member.member_identity,
                                                                             ContactIdKind::RelayUser)) {
          if (*contact) {
            member_contact_ids.push_back((*contact)->id);
          }
        }
      }
    }
    auto forked = MessagingHub::Instance().Groups().ForkGroup(group_id, new_title, member_contact_ids);
    if (!forked) {
      return forked.error();
    }
    (void)MessagingHub::Instance().Groups().ResolveOwnerUnreachableAdvisory(group_id);
    (void)inbox_.OpenThread(forked->id);
    if (on_action_message_) {
      on_action_message_("Started " + forked->title + " (fresh history)");
    }
    return Roe<std::optional<std::string>>(std::optional<std::string>{});
  }

  if (type == "message_group_owner") {
    if (!payload.contains("owner_identity") || !payload["owner_identity"].is_string()) {
      return Error("Missing owner_identity");
    }
    if (!MessagingHub::Instance().IsInitialized()) {
      return Error("Messaging not initialized");
    }
    auto thread =
        MessagingHub::Instance().Groups().OpenOwnerDirectMessage(payload["owner_identity"].get<std::string>());
    if (!thread) {
      return thread.error();
    }
    (void)inbox_.OpenThread(thread->id);
    if (on_action_message_) {
      on_action_message_("Opened chat with owner");
    }
    return Roe<std::optional<std::string>>(std::optional<std::string>{});
  }

  if (type == "dismiss_owner_advisory") {
    if (!payload.contains("group_id") || !payload["group_id"].is_string()) {
      return Error("Missing group_id");
    }
    if (!MessagingHub::Instance().IsInitialized()) {
      return Error("Messaging not initialized");
    }
    if (auto resolved =
            MessagingHub::Instance().Groups().ResolveOwnerUnreachableAdvisory(payload["group_id"].get<std::string>());
        !resolved) {
      return resolved.error();
    }
    inbox_.NotifyThreadChanged();
    return Roe<std::optional<std::string>>(std::optional<std::string>{});
  }

  return Roe<std::optional<std::string>>(std::optional<std::string>{});
}

} // namespace pbr
