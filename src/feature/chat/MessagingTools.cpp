#include "feature/chat/MessagingTools.h"

#include "feature/messaging/MessagingFacade.h"

#include "common/chat/MessagingJson.h"
#include "common/directory/DirectoryJson.h"
#include "domain/people/ContactJson.h"
#include "common/ValueJson.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

Value ContactsToJson(const std::vector<Contact>& contacts) {
  std::vector<Value> out;
  out.reserve(contacts.size());
  for (const Contact& contact : contacts) {
    out.push_back(ObjectValue(ContactToJson(contact)));
  }
  return ArrayValue(std::move(out));
}

Value ThreadsToJson(const std::vector<Thread>& threads) {
  std::vector<Value> out;
  out.reserve(threads.size());
  for (const Thread& thread : threads) {
    out.push_back(ObjectValue(ThreadToJson(thread)));
  }
  return ArrayValue(std::move(out));
}

Object StringProp(const char* description = nullptr) {
  Object prop;
  prop.set("type", "string");
  if (description) {
    prop.set("description", description);
  }
  return prop;
}

Object ObjectSchema(Object properties, std::vector<std::string> required = {}) {
  Object schema;
  schema.set("type", "object");
  schema.set("properties", std::move(properties));
  if (!required.empty()) {
    std::vector<Value> required_values;
    required_values.reserve(required.size());
    for (std::string& key : required) {
      required_values.push_back(Value(std::move(key)));
    }
    schema.set("required", ArrayValue(std::move(required_values)));
  }
  return schema;
}

ToolMeta Meta(std::string domain, std::string risk, const bool mutating) {
  return ToolMeta{"messaging", std::move(domain), std::move(risk), mutating};
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

  {
    Object properties;
    properties.set("query", StringProp("Search query"));
    tools.push_back(MakeTool(
      ToolDefinition{"search_people", "Search the public directory for people by name, nickname, or ID fragment.", ObjectSchema(std::move(properties), {"query"})},
      Meta("people", "read", false),
      [&messaging](const Object& arguments) -> Roe<std::string> {
           const std::string query = arguments.getString("query").value_or("");
           auto hits = messaging.SearchPeople(query);
           if (!hits) {
             return hits.error();
           }
           std::vector<Value> out;
           out.reserve(hits->size());
           for (const DirectoryHit& hit : *hits) {
             out.push_back(ObjectValue(DirectoryHitToJson(hit)));
           }
           return DumpJson(ArrayValue(std::move(out)));
         }));
  }

  {
    Object properties;
    properties.set("query", StringProp("Optional filter"));
    tools.push_back(MakeTool(
      ToolDefinition{"list_contacts", "List or search local contacts.", ObjectSchema(std::move(properties))},
      Meta("people", "read", false),
      [&messaging](const Object& arguments) -> Roe<std::string> {
                       const std::string query = arguments.getString("query").value_or("");
                       auto contacts = messaging.SearchLocalContacts(query);
                       if (!contacts) {
                         return contacts.error();
                       }
                       return DumpJson(ContactsToJson(*contacts));
                     }));
  }

  {
    Object properties;
    Object directory_hit;
    directory_hit.set("type", "object");
    properties.set("directory_hit", directory_hit);
    tools.push_back(MakeTool(
      ToolDefinition{"add_contact", "Add a directory search hit to local contacts.", ObjectSchema(std::move(properties), {"directory_hit"})},
      Meta("people", "write", true),
      [&messaging](const Object& arguments) -> Roe<std::string> {
           const Object* directory_hit = arguments.getObject("directory_hit");
           if (!directory_hit) {
             return Error("directory_hit required");
           }
           const DirectoryHit hit = DirectoryHitFromJson(*directory_hit);
           auto contact = messaging.AddContactFromDirectoryHit(hit);
           if (!contact) {
             return contact.error();
           }
           return DumpJson(ContactToJson(*contact));
         }));
  }

  tools.push_back(MakeTool(
      ToolDefinition{"list_conversations", "List inbox threads (AI and person-to-person).", ObjectSchema(Object{})},
      Meta("communications", "read", false),
      [&messaging](const Object& /*arguments*/) -> Roe<std::string> {
                     auto threads = messaging.ListThreads();
                     if (!threads) {
                       return threads.error();
                     }
                     return DumpJson(ThreadsToJson(*threads));
                   }));

  {
    Object properties;
    properties.set("thread_id", StringProp());
    tools.push_back(MakeTool(
      ToolDefinition{"open_conversation", "Switch the active inbox thread by thread id.", ObjectSchema(std::move(properties), {"thread_id"})},
      Meta("communications", "read", false),
      [&messaging](const Object& arguments) -> Roe<std::string> {
           const std::string thread_id = arguments.getString("thread_id").value_or("");
           auto thread = messaging.OpenThread(thread_id);
           if (!thread) {
             return thread.error();
           }
           return DumpJson(ThreadToJson(*thread));
         }));
  }

  {
    Object properties;
    properties.set("contact_id", StringProp());
    tools.push_back(MakeTool(
      ToolDefinition{"start_conversation", "Open or create a direct conversation with a local contact id.", ObjectSchema(std::move(properties), {"contact_id"})},
      Meta("communications", "write", true),
      [&messaging](const Object& arguments) -> Roe<std::string> {
           const std::string contact_id = arguments.getString("contact_id").value_or("");
           auto thread = messaging.FindOrCreateDirectThread(contact_id, ThreadChannel::E2ePublic);
           if (!thread) {
             return thread.error();
           }
           return DumpJson(ThreadToJson(*thread));
         }));
  }

  {
    Object properties;
    properties.set("nickname", StringProp("Optional nickname override"));
    tools.push_back(MakeTool(
      ToolDefinition{"register_user", "Register this device on the network with Ed25519 identity.", ObjectSchema(std::move(properties))},
      Meta("identity", "write", true),
      [&messaging](const Object& arguments) -> Roe<std::string> {
           auto identity = messaging.GetLocalIdentity();
           if (!identity) {
             return identity.error();
           }
           if (auto nickname = arguments.getString("nickname")) {
             LocalIdentity updated = *identity;
             updated.nickname = *nickname;
             (void)messaging.UpdateLocalIdentity(updated);
             identity = messaging.GetLocalIdentity();
           }

           auto result = messaging.FinishAndPersistRegistration(identity->nickname);
           if (!result) {
             return result.error();
           }
           Object out;
           out.set("success", result->registered);
           out.set("relay_user_id", result->relay_user_id);
           out.set("message", "Registered");
           out.set("expires_at", result->registration_expires_at);
           return DumpJson(out);
         }));
  }

  {
    Object properties;
    properties.set("nickname", StringProp());
    tools.push_back(MakeTool(
      ToolDefinition{"update_profile_nickname", "Update the registered nickname on the network.", ObjectSchema(std::move(properties), {"nickname"})},
      Meta("identity", "write", true),
      [&messaging](const Object& arguments) -> Roe<std::string> {
           const std::string nickname = arguments.getString("nickname").value_or("");
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
           Object out;
           out.set("success", result->success);
           out.set("message", result->message);
           return DumpJson(out);
         }));
  }

  return tools;
}

void RegisterMessagingTools(ToolRegistry& registry, MessagingFacade& messaging) {
  MessagingToolProvider provider(messaging);
  registry.RegisterProvider(provider);
}

} // namespace pbr
