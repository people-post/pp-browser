#include "lib/messaging/LiveGroups.h"

#include <algorithm>
#include "common/PbrCompat.h"

namespace pbr {

Roe<std::vector<LiveGroup>> ListLiveGroups(const IGroupRosterStore& roster, const std::string& local_account) {
  std::vector<LiveGroup> live;
  if (local_account.empty()) {
    return live;
  }
  auto ids = roster.ListGroupIds();
  if (!ids) {
    return ids.error();
  }
  for (const std::string& group_id : *ids) {
    auto metadata = roster.LoadMetadata(group_id);
    auto members = roster.ListMembers(group_id);
    if (!metadata || !*metadata || !members) {
      continue;
    }
    const GroupSyncState state = (*metadata)->sync_state;
    if (state == GroupSyncState::Dissolved || state == GroupSyncState::Removed) {
      continue;
    }
    const bool member = std::any_of(members->begin(), members->end(),
                                    [&](const GroupRosterMember& m) { return m.member_identity == local_account; });
    if (!member) {
      continue; // left (or never joined: an invite not taken)
    }
    auto thread = roster.FindThreadIdForGroup(group_id);
    if (!thread || !*thread || (*thread)->empty()) {
      continue; // its chat was deleted on this device
    }
    live.push_back({group_id, **thread, members->size()});
  }
  return live;
}

} // namespace pbr
