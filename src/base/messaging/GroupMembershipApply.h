#pragma once

#include "base/messaging/GroupRosterStore.h"

#include "common/Error.h"

#include <string>

namespace pbr {

/** Owner-side roster mutations for invite accept/decline (no P2P). */
Roe<void> ApplyInviteAcceptToRoster(GroupRosterStore& roster, const std::string& invite_nonce,
                                    const std::string& member_identity);
Roe<void> ApplyInviteDeclineToRoster(GroupRosterStore& roster, const std::string& invite_nonce,
                                     const std::string& member_identity);

} // namespace pbr
