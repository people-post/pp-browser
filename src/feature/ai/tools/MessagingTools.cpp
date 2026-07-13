#include "feature/ai/tools/MessagingTools.h"

#include "base/messaging/MessagingJson.h"
#include "base/net/RegistrationClientUtil.h"

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

void RegisterMessagingTools(ToolRegistry& registry, MessagingHub& hub) {
  registry.Register(
      {.definition = {.name = "search_people",
                      .description = "Search the public directory for people by name, nickname, or ID fragment.",
                      .parameters = {{"type", "object"},
                                     {"properties", {{"query", {{"type", "string"}, {"description", "Search query"}}}}},
                                     {"required", nlohmann::json::array({"query"})}}},
       .execute = [&hub](const nlohmann::json& arguments) -> Roe<std::string> {
         const std::string query = arguments.value("query", "");
         auto hits = hub.Directory().SearchPeople(query);
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
                     .execute = [&hub](const nlohmann::json& arguments) -> Roe<std::string> {
                       const std::string query = arguments.value("query", "");
                       auto contacts = hub.Contacts().SearchLocal(query);
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
       .execute = [&hub](const nlohmann::json& arguments) -> Roe<std::string> {
         if (!arguments.contains("directory_hit")) {
           return Error("directory_hit required");
         }
         const DirectoryHit hit = DirectoryHitFromJson(arguments["directory_hit"]);
         auto contact = hub.Contacts().AddFromDirectoryHit(hit);
         if (!contact) {
           return contact.error();
         }
         return ContactToJson(*contact).dump();
       }});

  registry.Register({.definition = {.name = "list_conversations",
                                    .description = "List inbox threads (AI and person-to-person).",
                                    .parameters = {{"type", "object"}, {"properties", nlohmann::json::object()}}},
                     .execute = [&hub](const nlohmann::json& /*arguments*/) -> Roe<std::string> {
                       auto threads = hub.Inbox().ListThreads();
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
       .execute = [&hub](const nlohmann::json& arguments) -> Roe<std::string> {
         const std::string thread_id = arguments.value("thread_id", "");
         auto thread = hub.Inbox().OpenThread(thread_id);
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
       .execute = [&hub](const nlohmann::json& arguments) -> Roe<std::string> {
         const std::string contact_id = arguments.value("contact_id", "");
         auto thread = hub.Inbox().FindOrCreateDirectThread(contact_id);
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
                     .execute = [&hub](const nlohmann::json& arguments) -> Roe<std::string> {
                       auto identity = hub.Identity().Get();
                       if (!identity) {
                         return identity.error();
                       }
                       if (arguments.contains("nickname") && arguments["nickname"].is_string()) {
                         LocalIdentity updated = *identity;
                         updated.nickname = arguments["nickname"].get<std::string>();
                         (void)hub.Identity().Update(updated);
                         identity = hub.Identity().Get();
                       }

                       auto result = FinishAndPersistRegistration(hub.Registration(), hub.Identity(), identity->nickname);
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
       .execute = [&hub](const nlohmann::json& arguments) -> Roe<std::string> {
         const std::string nickname = arguments.value("nickname", "");
         if (nickname.empty()) {
           return Error("nickname required");
         }
         auto result = UpdateRegisteredNickname(hub.Registration(), hub.Identity(), nickname);
         if (!result) {
           return result.error();
         }
         auto identity = hub.Identity().Get();
         if (identity) {
           LocalIdentity updated = *identity;
           updated.nickname = nickname;
           (void)hub.Identity().Update(updated);
         }
         return nlohmann::json{{"success", result->success}, {"message", result->message}}.dump();
       }});
}

} // namespace pbr
