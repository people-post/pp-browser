#pragma once

#include "lib/calls/CallGuests.h"
#include "lib/messaging/GroupTypes.h"
#include "lib/messaging/IGroupRosterStore.h"

#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * V062: what a member's role and the group's preset let them do in the group's call, in the calls' words — the one
 * table (GroupAllows, group-chat G017): bring a guest per the preset, remove one as owner or admin, invite to the group.
 */
CallGroupStanding CallStandingOfMember(MemberRole role, GroupPreset preset);

/**
 * V062: `account`'s standing in `group_id` as this device's roster store says: none when it holds no roster for the
 * group (a guest's device: not in it), a non-member when the roster does not list them (a dissolved group lists
 * nobody).
 */
std::optional<CallGroupStanding> CallStandingFromRoster(const IGroupRosterStore& rosters, const std::string& group_id,
                                                        const std::string& account);

} // namespace pbr
