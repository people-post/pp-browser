#include "feature/conversations/GroupInviteBatch.h"

namespace pbr {

Roe<void> CheckGroupHasRoom(const size_t roster_size) {
  if (roster_size >= static_cast<size_t>(kGroupMemberLimit)) {
    return Error("Group is full");
  }
  return {};
}

GroupInviteBatchResult InviteEach(const std::vector<std::string>& contact_ids,
                                  const std::function<Roe<void>(const std::string& contact_id)>& invite_one) {
  GroupInviteBatchResult result;
  for (const std::string& contact_id : contact_ids) {
    if (auto invited = invite_one(contact_id); invited) {
      result.sent.push_back(contact_id);
    } else {
      result.failed.emplace_back(contact_id, invited.error().message);
    }
  }
  return result;
}

} // namespace pbr
