#include "feature/messaging/MessagingPeoplePickerPorts.h"

#include "feature/messaging/MessagingHub.h"

namespace pbr {

MessagingPeoplePickerPorts MakeMessagingPeoplePickerPorts(MessagingHub& hub) {
  MessagingPeoplePickerPorts ports;
  ports.get_thread = [&hub](const std::string& thread_id) { return hub.Store().GetThread(thread_id); };
  ports.list_group_roster = [&hub](const std::string& group_id) { return hub.Groups().ListRoster(group_id); };
  ports.local_relay_identity = [&hub]() -> std::optional<std::string> {
    if (auto identity = hub.Identity().Get()) {
      return identity->relay_user_id;
    }
    return std::nullopt;
  };
  ports.find_contact_by_identity = [&hub](const std::string& identity_value, const ContactIdKind kind) {
    return hub.Contacts().FindByIdentity(identity_value, kind);
  };
  ports.create_group = [&hub](const std::string& title, const std::vector<std::string>& member_contact_ids) {
    return hub.Inbox().CreateGroup(title, member_contact_ids);
  };
  ports.list_call_participants = [&hub](const std::string& call_id) -> Roe<std::vector<CallParticipant>> {
    if (auto* calls = hub.Calls()) {
      if (auto participants = calls->ListJoinedParticipants(call_id)) {
        return *participants;
      }
      return Roe<std::vector<CallParticipant>>(Error("call participants unavailable"));
    }
    return Roe<std::vector<CallParticipant>>(Error("calls unavailable"));
  };
  return ports;
}

} // namespace pbr
