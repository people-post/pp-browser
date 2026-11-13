#pragma once

#include "lib/messaging/IGroupRosterStore.h"
#include "lib/people/ContactsStore.h"

#include <functional>
#include <string>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {

/**
 * Who this account gives a relay grant (multi-relay MR005): every contact it has not blocked, and every member of a
 * group it is in (group members are not always contacts, and group mail must not be throttled as a stranger's). A
 * blocked contact gets none, in a group or not. Any thread.
 */
class RelayGrantPeers {
public:
  /** `self`: our Account ID now (never granted). */
  RelayGrantPeers(ContactsStore& contacts, IGroupRosterStore& rosters, std::function<std::string()> self);

  bool Granted(const std::string& account_id) const;
  /** Every account granted now (contacts and co-members, blocked ones left out), each once. */
  std::vector<std::string> All() const;
  /** The blocked contacts' accounts: their grants are not renewed. */
  std::vector<std::string> Blocked() const;

private:
  /** A group we are in that has not ended or dropped us (its roster is live). */
  bool InLiveGroup(const std::string& group_id) const;

  ContactsStore& contacts_;
  IGroupRosterStore& rosters_;
  std::function<std::string()> self_;
};

} // namespace pbr
