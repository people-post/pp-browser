#pragma once

#include "base/messaging/CallTypes.h"
#include "base/messaging/GroupTypes.h"
#include "base/messaging/ThreadTypes.h"
#include "base/people/ContactTypes.h"
#include "common/Error.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Group / call / thread ports for people-picker flows.
 * Contact CRUD + DM thread ops live on MessagingContactsPorts.
 * Application fills from MessagingHub. Clear via BindPickerPorts({}).
 */
struct MessagingPeoplePickerPorts {
  std::function<Roe<std::optional<Thread>>(const std::string& thread_id)> get_thread;
  std::function<Roe<std::vector<GroupRosterMember>>(const std::string& group_id)> list_group_roster;
  std::function<std::optional<std::string>()> local_relay_identity;
  std::function<Roe<std::optional<Contact>>(const std::string& identity_value, ContactIdKind kind)>
      find_contact_by_identity;
  std::function<Roe<Thread>(const std::string& title, const std::vector<std::string>& member_contact_ids)> create_group;
  std::function<Roe<std::vector<CallParticipant>>(const std::string& call_id)> list_call_participants;
};

class MessagingHub;

MessagingPeoplePickerPorts MakeMessagingPeoplePickerPorts(MessagingHub& hub);

} // namespace pbr
