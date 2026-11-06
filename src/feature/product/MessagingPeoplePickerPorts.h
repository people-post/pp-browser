#pragma once

#include "feature/conversations/GroupInviteBatch.h"
#include "lib/calls/CallTypes.h"
#include "lib/messaging/GroupTypes.h"
#include "common/thread/ThreadTypes.h"
#include "lib/people/ContactTypes.h"
#include "common/Error.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/**
 * Group / call / thread ports for people-picker flows.
 * Contact CRUD + DM thread ops live on MessagingContactsPorts.
 * Application fills from ConversationsHub. Clear via BindPickerPorts({}).
 */
struct MessagingPeoplePickerPorts {
  std::function<Roe<std::optional<Thread>>(const std::string& thread_id)> get_thread;
  std::function<Roe<std::vector<GroupRosterMember>>(const std::string& group_id)> list_group_roster;
  std::function<std::optional<std::string>()> local_relay_identity;
  /** Whether this account owns the group, by the engine's own check (the group's recorded owner). */
  std::function<Roe<bool>(const std::string& group_id)> is_local_owner;
  std::function<Roe<std::optional<Contact>>(const std::string& identity_value, mesh::ContactIdKind kind)>
      find_contact_by_identity;
  std::function<Roe<Thread>(const std::string& title, const std::vector<std::string>& member_contact_ids)> create_group;
  /**
   * G022: a group made during the call `call_id`, inviting these accounts (people met in the call need not be
   * contacts); the chat opens.
   */
  std::function<Roe<Thread>(const std::string& title, const std::string& call_id,
                            const std::vector<std::string>& member_identities)>
      create_group_from_call;
  /** This account's public name (its profile nickname; empty if none). */
  std::function<std::string()> local_nickname;
  std::function<Roe<std::vector<CallParticipant>>(const std::string& call_id)> list_call_participants;
  std::function<std::vector<std::string>(const std::string& group_id)> list_unreachable_members;
  /** Owner: invites this device sent that are still waiting (not expired); latest per invitee. */
  std::function<std::vector<PendingGroupInvite>(const std::string& group_id)> list_invited_waiting;
  /** A member who is not a contact: the nickname on their kept account card (empty if none). */
  std::function<std::string(const std::string& account_id)> kept_card_nickname;
  std::function<Roe<void>(const std::string& group_id, const std::string& member_identity)> remove_member_by_identity;
  /** Owner (G017): make a member an admin, or a member again. */
  std::function<Roe<void>(const std::string& group_id, const std::string& member_identity, MemberRole role)>
      set_member_role;
  /** Owner: one invite per contact on a worker; `done` on the UI thread. Application fills it from the facade. */
  std::function<void(const std::string& group_id, std::vector<std::string> contact_ids,
                     std::function<void(GroupInviteBatchResult)> done)>
      invite_members_async;
  /** Contact actions (`add_contact` with an `account_id` looks the person up in the directory); `done` on the UI thread. */
  std::function<void(const std::string& payload_json, std::function<void(Roe<std::optional<std::string>>)> done)>
      dispatch_action_async;
};

class ConversationsHub;

MessagingPeoplePickerPorts MakeMessagingPeoplePickerPorts(ConversationsHub& hub);

} // namespace pbr
