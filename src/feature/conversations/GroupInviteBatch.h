#pragma once

#include "common/Error.h"
#include "lib/ui/PeoplePickerLogic.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** Refuses an invite when the group's roster already holds kGroupMemberLimit members. */
Roe<void> CheckGroupHasRoom(size_t roster_size);

/** What a batch of invites did, by contact id. `failed` carries the reason for each. */
struct GroupInviteBatchResult {
  std::vector<std::string> sent;
  std::vector<std::pair<std::string, std::string>> failed;
};

/** One call per person: a failure does not stop the others. */
GroupInviteBatchResult InviteEach(const std::vector<std::string>& contact_ids,
                                  const std::function<Roe<void>(const std::string& contact_id)>& invite_one);

} // namespace pbr
