#include "feature/messaging/ContactActionDispatcher.h"

#include "common/Utilities.h"
#include "base/messaging/MessagingJson.h"

#include <nlohmann/json.hpp>

namespace pbr {

ContactActionDispatcher::ContactActionDispatcher(InboxController& inbox, ContactsStore& contacts,
                                                 IdentityStore& identity, IRegistrationClient* registration)
    : inbox_(inbox), contacts_(contacts), identity_(identity), registration_(registration) {
  redirectLogger("ContactActionDispatcher");
}

void ContactActionDispatcher::SetRegistrationClient(IRegistrationClient* registration) {
  registration_ = registration;
}

void ContactActionDispatcher::SetOnActionMessage(std::function<void(const std::string& message)> callback) {
  on_action_message_ = std::move(callback);
}

Roe<std::optional<std::string>> ContactActionDispatcher::Dispatch(const std::string& payload_json) {
  const nlohmann::json payload = nlohmann::json::parse(payload_json, nullptr, false);
  if (payload.is_discarded() || !payload.contains("type") || !payload["type"].is_string()) {
    return Roe<std::optional<std::string>>(std::optional<std::string>{});
  }

  const std::string type = payload["type"].get<std::string>();

  if (type == "start_conversation") {
    std::string contact_id;
    if (payload.contains("contact_id") && payload["contact_id"].is_string()) {
      contact_id = payload["contact_id"].get<std::string>();
    } else if (payload.contains("directory_hit") && payload["directory_hit"].is_object()) {
      const DirectoryHit hit = DirectoryHitFromJson(payload["directory_hit"]);
      auto contact = contacts_.AddFromDirectoryHit(hit);
      if (!contact) {
        return contact.error();
      }
      contact_id = contact->id;
    } else {
      return Error("Missing contact_id or directory_hit");
    }
    auto thread = inbox_.FindOrCreateDirectThread(contact_id);
    if (!thread) {
      return thread.error();
    }
    if (on_action_message_) {
      on_action_message_("Opened conversation with " + thread->title);
    }
    return Roe<std::optional<std::string>>(std::optional<std::string>{});
  }

  if (type == "add_contact") {
    if (!payload.contains("directory_hit") || !payload["directory_hit"].is_object()) {
      return Error("Missing directory_hit");
    }
    const DirectoryHit hit = DirectoryHitFromJson(payload["directory_hit"]);
    auto contact = contacts_.AddFromDirectoryHit(hit);
    if (!contact) {
      return contact.error();
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
    const int64_t timestamp = util::NowUnixMs();
    nlohmann::json body = {{"public_key", identity->public_key_b64},
                           {"nickname", identity->nickname},
                           {"timestamp", timestamp}};
    auto signature = identity_.SignPayload(body.dump());
    if (!signature) {
      return signature.error();
    }
    if (!registration_) {
      return Error("Registration client not configured");
    }
    auto result = registration_->Register(identity->public_key_b64, identity->nickname, *signature, timestamp);
    if (!result) {
      return result.error();
    }
    LocalIdentity updated = *identity;
    updated.registered = result->success;
    if (!result->relay_user_id.empty()) {
      updated.relay_user_id = result->relay_user_id;
    }
    (void)identity_.Update(updated);
    if (on_action_message_) {
      on_action_message_(result->message);
    }
    return Roe<std::optional<std::string>>(std::optional<std::string>{});
  }

  return Roe<std::optional<std::string>>(std::optional<std::string>{});
}

} // namespace pbr
