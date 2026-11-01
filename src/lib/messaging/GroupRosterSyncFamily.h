#pragma once

#include "lib/messaging/GroupRosterStore.h"
#include "lib/messaging/GroupRosterSyncCodec.h"

#include "common/chat/SystemControl.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

inline constexpr const char* kGroupRosterSyncFamily = "group roster";

/**
 * A group that ended here (G016) by way other than the owner's DM — a member's or the owner's answer to our member
 * check, or the owner's other device (`group_dissolved_sibling`): the owner, and what it voided. The caller adds
 * the group's line and notice and closes the invite cards.
 */
struct GroupDissolvedEnd {
  std::string group_id;
  std::string owner_identity;
  std::vector<PendingGroupInvite> voided_invites;
};

/**
 * A restored group's member check (G013) as a control-lane family: `group_roster_request` and
 * `group_roster_reply` are bookkeeping between two devices — never stored, applied once while fresh, and
 * an account copy that carries the sender's card (the asker may be a device restored from its recovery phrase,
 * which keeps no cards). A request from a member is handed to `answer` (it signs and sends; the
 * family only decides); a reply is applied to the roster (group_roster_sync::ApplyReply). The owner's other device
 * says a group was dissolved the same way (`group_dissolved_sibling`, G016); `on_dissolved` hears of every group a
 * reply or such a notice ended.
 * `local_account` is our account id.
 */
SystemControlFamily MakeGroupRosterSyncFamily(
    GroupRosterStore& roster, std::function<std::optional<std::string>()> local_account,
    std::function<void(group_roster_sync_codec::RosterRequest request, std::string asker)> answer,
    std::function<void(GroupDissolvedEnd end)> on_dissolved = {});

} // namespace pbr
