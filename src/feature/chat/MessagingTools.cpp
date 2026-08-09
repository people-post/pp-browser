#include "feature/chat/MessagingTools.h"

#include "feature/messaging/MessagingFacade.h"

#include "base/messaging/MessagingJson.h"

#include <nlohmann/json.hpp>

namespace pbr {

namespace {

nlohmann::json ContactsToJson(const std::vector<Contact>& contacts) {
  nlohmann::json out = nlohmann::json::array();
  for (const Contact& contact : contacts) {
    out.push_back(ContactToJson(contact));
  }
  return out;
}

nlohmann::json ThreadsToJson(const std::vector<Thread>& threads) {
  nlohmann::json out = nlohmann::json::array();
  for (const Thread& thread : threads) {
    out.push_back(ThreadToJson(thread));
  }
  return out;
}

} // namespace

void RegisterMessagingTools(ToolRegistry& registry, MessagingFacade& messaging) {
  registry.Register(
      {.definition = {.name = "search_people",
                      .description = "Search the public directory for people by name, nickname, or ID fragment.",
                      .parameters = {{"type", "object"},
                                     {"properties", {{"query", {{"type", "string"}, {"description", "Search query"}}}}},
                                     {"required", nlohmann::json::array({"query"})}}},
       .execute = [&messaging](const nlohmann::json& arguments) -> Roe<std::string> {
         const std::string query = arguments.value("query", "");
         auto hits = messaging.SearchPeople(query);
         if (!hits) {
           return hits.error();
         }
         nlohmann::json out = nlohmann::json::array();
         for (const DirectoryHit& hit : *hits) {
           out.push_back(DirectoryHitToJson(hit));
         }
         return out.dump();
       }});

  registry.Register({.definition = {.name = "list_contacts",
                                    .description = "List or search local contacts.",
                                    .parameters = {{"type", "object"},
                                                   {"properties",
                                                    {{"query", {{"type", "string"}, {"description", "Optional filter"}}}}},
                                                   {"required", nlohmann::json::array()}}},
                     .execute = [&messaging](const nlohmann::json& arguments) -> Roe<std::string> {
                       const std::string query = arguments.value("query", "");
                       auto contacts = messaging.SearchLocalContacts(query);
                       if (!contacts) {
                         return contacts.error();
                       }
                       return ContactsToJson(*contacts).dump();
                     }});

  registry.Register(
      {.definition = {.name = "add_contact",
                      .description = "Add a directory search hit to local contacts.",
                      .parameters = {{"type", "object"},
                                     {"properties", {{"directory_hit", {{"type", "object"}}}}},
                                     {"required", nlohmann::json::array({"directory_hit"})}}},
       .execute = [&messaging](const nlohmann::json& arguments) -> Roe<std::string> {
         if (!arguments.contains("directory_hit")) {
           return Error("directory_hit required");
         }
         const DirectoryHit hit = DirectoryHitFromJson(arguments["directory_hit"]);
         auto contact = messaging.AddContactFromDirectoryHit(hit);
         if (!contact) {
           return contact.error();
         }
         return ContactToJson(*contact).dump();
       }});

  registry.Register({.definition = {.name = "list_conversations",
                                    .description = "List inbox threads (AI and person-to-person).",
                                    .parameters = {{"type", "object"}, {"properties", nlohmann::json::object()}}},
                     .execute = [&messaging](const nlohmann::json& /*arguments*/) -> Roe<std::string> {
                       auto threads = messaging.ListThreads();
                       if (!threads) {
                         return threads.error();
                       }
                       return ThreadsToJson(*threads).dump();
                     }});

  registry.Register(
      {.definition = {.name = "open_conversation",
                      .description = "Switch the active inbox thread by thread id.",
                      .parameters = {{"type", "object"},
                                     {"properties", {{"thread_id", {{"type", "string"}}}}},
                                     {"required", nlohmann::json::array({"thread_id"})}}},
       .execute = [&messaging](const nlohmann::json& arguments) -> Roe<std::string> {
         const std::string thread_id = arguments.value("thread_id", "");
         auto thread = messaging.OpenThread(thread_id);
         if (!thread) {
           return thread.error();
         }
         return ThreadToJson(*thread).dump();
       }});

  registry.Register(
      {.definition = {.name = "start_conversation",
                      .description = "Open or create a direct conversation with a local contact id.",
                      .parameters = {{"type", "object"},
                                     {"properties", {{"contact_id", {{"type", "string"}}}}},
                                     {"required", nlohmann::json::array({"contact_id"})}}},
       .execute = [&messaging](const nlohmann::json& arguments) -> Roe<std::string> {
         const std::string contact_id = arguments.value("contact_id", "");
         auto thread = messaging.FindOrCreateDirectThread(contact_id, ThreadChannel::E2ePublic);
         if (!thread) {
           return thread.error();
         }
         return ThreadToJson(*thread).dump();
       }});

  registry.Register({.definition = {.name = "register_user",
                                    .description = "Register this device on the network with Ed25519 identity.",
                                    .parameters = {{"type", "object"},
                                                   {"properties",
                                                    {{"nickname",
                                                      {{"type", "string"}, {"description", "Optional nickname override"}}}}},
                                                   {"required", nlohmann::json::array()}}},
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

  registry.Register(
      {.definition = {.name = "update_profile_nickname",
                      .description = "Update the registered nickname on the network.",
                      .parameters = {{"type", "object"},
                                     {"properties", {{"nickname", {{"type", "string"}}}}},
                                     {"required", nlohmann::json::array({"nickname"})}}},
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
}

} // namespace pbr
