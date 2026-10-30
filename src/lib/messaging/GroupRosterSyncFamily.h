#pragma once

#include "lib/messaging/GroupRosterStore.h"
#include "lib/messaging/GroupRosterSyncCodec.h"

#include "common/chat/SystemControl.h"

#include <functional>
#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

inline constexpr const char* kGroupRosterSyncFamily = "group roster";

/**
 * A restored group's member check (G013) as a control-lane family: `group_roster_request` and
 * `group_roster_reply` are bookkeeping between two devices — never stored, applied once while fresh, and
 * an account copy that carries the sender's card (the asker may be a device restored from its recovery phrase,
 * which keeps no cards). A request from a member is handed to `answer` (it signs and sends; the
 * family only decides); a reply is applied to the roster (group_roster_sync::ApplyReply).
 * `local_account` is our account id.
 */
SystemControlFamily MakeGroupRosterSyncFamily(
    GroupRosterStore& roster, std::function<std::optional<std::string>()> local_account,
    std::function<void(group_roster_sync_codec::RosterRequest request, std::string asker)> answer);

} // namespace pbr
