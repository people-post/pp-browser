#include "feature/chat/MessagingTools.h"

#include "feature/messaging/MessagingFacade.h"

#include "base/messaging/MessagingJson.h"
#include "common/ValueJson.h"

#include <nlohmann/json.hpp>

namespace pbr {

namespace {

Object ObjectFromNlohmann(const nlohmann::json& json) {
  return TryParseObject(json.dump()).value_or(Object{});
}

nlohmann::json NlohmannFromObject(const Object& object) {
  return nlohmann::json::parse(DumpJson(object), nullptr, false);
}

nlohmann::json ContactsToJson(const std::vector<Contact>& contacts) {
  nlohmann::json out = nlohmann::json::array();
  for (const Contact& contact : contacts) {
    out.push_back(NlohmannFromObject(ContactToJson(contact)));
  }
  return out;
}

nlohmann::json ThreadsToJson(const std::vector<Thread>& threads) {
  nlohmann::json out = nlohmann::json::array();
  for (const Thread& thread : threads) {
    out.push_back(NlohmannFromObject(ThreadToJson(thread)));
  }
  return out;
}

ToolMeta Meta(std::string domain, std::string risk, const bool mutating) {
  return ToolMeta{.provider = "messaging",
                  .domain = std::move(domain),
                  .risk = std::move(risk),
                  .mutating = mutating};
}

} // namespace

MessagingToolProvider::MessagingToolProvider(MessagingFacade& messaging) : messaging_(messaging) {}

std::string MessagingToolProvider::Id() const {
  return "messaging";
}

std::vector<ToolDescriptor> MessagingToolProvider::ListTools() {
  // Capture the long-lived facade, not `this` — providers are often stack-temporaries
  // during RegisterProvider.
  MessagingFacade& messaging = messaging_;
  std::vector<ToolDescriptor> tools;

  tools.push_back(
      {.definition = {.name = "search_people",
                      .description = "Search the public directory for people by name, nickname, or ID fragment.",
                      .parameters = {{"type", "object"},
                                     {"properties", {{"query", {{"type", "string"}, {"description", "Search query"}}}}},
                                     {"required", nlohmann::json::array({"query"})}}},
       .meta = Meta("people", "read", false),
       .execute = [&messaging](const nlohmann::json& arguments) -> Roe<std::string> {
         const std::string query = arguments.value("query", "");
         auto hits = messaging.SearchPeople(query);
         if (!hits) {
           return hits.error();
         }
         nlohmann::json out = nlohmann::json::array();
         for (const DirectoryHit& hit : *hits) {
           out.push_back(NlohmannFromObject(DirectoryHitToJson(hit)));
         }
         return out.dump();
       }});

  tools.push_back({.definition = {.name = "list_contacts",
                                  .description = "List or search local contacts.",
                                  .parameters = {{"type", "object"},
                                                 {"properties",
                                                  {{"query", {{"type", "string"}, {"description", "Optional filter"}}}}},
                                                 {"required", nlohmann::json::array()}}},
                   .meta = Meta("people", "read", false),
                   .execute = [&messaging](const nlohmann::json& arguments) -> Roe<std::string> {
                     const std::string query = arguments.value("query", "");
                     auto contacts = messaging.SearchLocalContacts(query);
                     if (!contacts) {
                       return contacts.error();
                     }
                     return ContactsToJson(*contacts).dump();
                   }});

  tools.push_back(
      {.definition = {.name = "add_contact",
                      .description = "Add a directory search hit to local contacts.",
                      .parameters = {{"type", "object"},
                                     {"properties", {{"directory_hit", {{"type", "object"}}}}},
                                     {"required", nlohmann::json::array({"directory_hit"})}}},
       .meta = Meta("people", "write", true),
       .execute = [&messaging](const nlohmann::json& arguments) -> Roe<std::string> {
         if (!arguments.contains("directory_hit")) {
           return Error("directory_hit required");
         }
         const DirectoryHit hit = DirectoryHitFromJson(ObjectFromNlohmann(arguments["directory_hit"]));
         auto contact = messaging.AddContactFromDirectoryHit(hit);
         if (!contact) {
           return contact.error();
         }
         return DumpJson(ContactToJson(*contact));
       }});

  tools.push_back({.definition = {.name = "list_conversations",
                                  .description = "List inbox threads (AI and person-to-person).",
                                  .parameters = {{"type", "object"}, {"properties", nlohmann::json::object()}}},
                   .meta = Meta("communications", "read", false),
                   .execute = [&messaging](const nlohmann::json& /*arguments*/) -> Roe<std::string> {
                     auto threads = messaging.ListThreads();
                     if (!threads) {
                       return threads.error();
                     }
                     return ThreadsToJson(*threads).dump();
                   }});

  tools.push_back(
      {.definition = {.name = "open_conversation",
                      .description = "Switch the active inbox thread by thread id.",
                      .parameters = {{"type", "object"},
                                     {"properties", {{"thread_id", {{"type", "string"}}}}},
                                     {"required", nlohmann::json::array({"thread_id"})}}},
       .meta = Meta("communications", "read", false),
       .execute = [&messaging](const nlohmann::json& arguments) -> Roe<std::string> {
         const std::string thread_id = arguments.value("thread_id", "");
         auto thread = messaging.OpenThread(thread_id);
         if (!thread) {
           return thread.error();
         }
         return DumpJson(ThreadToJson(*thread));
       }});

  tools.push_back(
      {.definition = {.name = "start_conversation",
                      .description = "Open or create a direct conversation with a local contact id.",
                      .parameters = {{"type", "object"},
                                     {"properties", {{"contact_id", {{"type", "string"}}}}},
                                     {"required", nlohmann::json::array({"contact_id"})}}},
       .meta = Meta("communications", "write", true),
       .execute = [&messaging](const nlohmann::json& arguments) -> Roe<std::string> {
         const std::string contact_id = arguments.value("contact_id", "");
         auto thread = messaging.FindOrCreateDirectThread(contact_id, ThreadChannel::E2ePublic);
         if (!thread) {
           return thread.error();
         }
         return DumpJson(ThreadToJson(*thread));
       }});

  tools.push_back(
      {.definition = {.name = "register_user",
                      .description = "Register this device on the network with Ed25519 identity.",
                      .parameters = {{"type", "object"},
                                     {"properties",
                                      {{"nickname",
                                        {{"type", "string"}, {"description", "Optional nickname override"}}}}},
                                     {"required", nlohmann::json::array()}}},
       .meta = Meta("identity", "write", true),
       .execute = [&messaging](const nlohmann::json& arguments) -> Roe<std::string> {
         auto identity = messaging.GetLocalIdentity();
         if (!identity) {
           return identity.error();
         }
         if (arguments.contains("nickname") && arguments["nickname"].is_string()) {
           LocalIdentity updated = *identity;
           updated.nickname = arguments["nickname"].get<std::string>();
           (void)messaging.UpdateLocalIdentity(updated);
           identity = messaging.GetLocalIdentity();
         }

         auto result = messaging.FinishAndPersistRegistration(identity->nickname);
         if (!result) {
           return result.error();
         }
         return nlohmann::json{{"success", result->registered},
                               {"relay_user_id", result->relay_user_id},
                               {"message", "Registered"},
                               {"expires_at", result->registration_expires_at}}
             .dump();
       }});

  tools.push_back(
      {.definition = {.name = "update_profile_nickname",
                      .description = "Update the registered nickname on the network.",
                      .parameters = {{"type", "object"},
                                     {"properties", {{"nickname", {{"type", "string"}}}}},
                                     {"required", nlohmann::json::array({"nickname"})}}},
       .meta = Meta("identity", "write", true),
       .execute = [&messaging](const nlohmann::json& arguments) -> Roe<std::string> {
         const std::string nickname = arguments.value("nickname", "");
         if (nickname.empty()) {
           return Error("nickname required");
         }
         auto result = messaging.UpdateRegisteredNickname(nickname);
         if (!result) {
           return result.error();
         }
         auto identity = messaging.GetLocalIdentity();
         if (identity) {
           LocalIdentity updated = *identity;
           updated.nickname = nickname;
           (void)messaging.UpdateLocalIdentity(updated);
         }
         return nlohmann::json{{"success", result->success}, {"message", result->message}}.dump();
       }});

  return tools;
}

void RegisterMessagingTools(ToolRegistry& registry, MessagingFacade& messaging) {
  MessagingToolProvider provider(messaging);
  registry.RegisterProvider(provider);
}

} // namespace pbr
