#include "feature/conversations/MessagingPeoplePickerPorts.h"

#include "feature/conversations/ConversationsHub.h"
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

MessagingPeoplePickerPorts MakeMessagingPeoplePickerPorts(ConversationsHub& hub) {
  MessagingPeoplePickerPorts ports;
  ports.get_thread = [&hub](const std::string& thread_id) { return hub.Store().GetThread(thread_id); };
  ports.list_group_roster = [&hub](const std::string& group_id) { return hub.Groups().ListRoster(group_id); };
  ports.local_relay_identity = [&hub]() -> std::optional<std::string> {
    if (auto identity = hub.Identity().Get()) {
      if (!identity->account_id.empty()) {
        return identity->account_id;
      }
    }
    return std::nullopt;
  };
  ports.find_contact_by_identity = [&hub](const std::string& identity_value, const mesh::ContactIdKind kind) {
    return hub.Contacts().FindByIdentity(identity_value, kind);
  };
  ports.create_group = [&hub](const std::string& title, const std::vector<std::string>& member_contact_ids) {
    return hub.Inbox().CreateGroup(title, member_contact_ids);
  };
  ports.list_unreachable_members = [&hub](const std::string& group_id) { return hub.Groups().ListUnreachable(group_id); };
  ports.kept_card_nickname = [&hub](const std::string& account_id) { return hub.KeptCardNickname(account_id); };
  ports.remove_member_by_identity = [&hub](const std::string& group_id, const std::string& member_identity) {
    return hub.Groups().RemoveMemberByIdentity(group_id, member_identity);
  };
  ports.dispatch_action_async = [&hub](const std::string& payload_json,
                                       std::function<void(Roe<std::optional<std::string>>)> done) {
    hub.Actions().DispatchAsync(payload_json, std::move(done));
  };
  return ports;
}

} // namespace pbr
