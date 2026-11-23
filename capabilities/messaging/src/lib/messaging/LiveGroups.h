#pragma once

#include "lib/messaging/IGroupRosterStore.h"

#include "common/Error.h"

#include <cstddef>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** A group this account is in now, with its chat on this device: one row of the Groups page. */
struct LiveGroup {
  std::string group_id;
  std::string thread_id;
  size_t member_count = 0;
};

/**
 * The groups `local_account` is in now: not dissolved, not removed, and still on its own roster (the rule the account
 * locker uses for "left", GroupMembershipWorkflow::StateGroups), with a chat on this device. Unordered.
 */
Roe<std::vector<LiveGroup>> ListLiveGroups(const IGroupRosterStore& roster, const std::string& local_account);

} // namespace pbr
