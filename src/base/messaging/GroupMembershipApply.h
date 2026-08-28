#pragma once

#include "base/messaging/GroupMembershipCodec.h"
#include "base/messaging/GroupRosterStore.h"

#include "common/Error.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** Owner-side: pending invite → active member (requires pending row + matching invitee). */
Roe<void> ApplyInviteAcceptToRoster(GroupRosterStore& roster, const std::string& invite_nonce,
                                    const std::string& member_identity);
/** Owner-side: clear pending invite (invitee was never an active encrypt target). */
Roe<void> ApplyInviteDeclineToRoster(GroupRosterStore& roster, const std::string& invite_nonce,
                                     const std::string& member_identity);

/**
 * Apply owner-signed member_joined. Actor must be current owner; roster_epoch must be > local.
 * This is the peer-facing membership commit (G006).
 */
Roe<void> ApplyMemberJoinedToRoster(GroupRosterStore& roster, const GroupMembershipCodec::MemberJoinedPayload& payload,
                                    const std::string& actor_identity);

/**
 * Apply owner_transferred (optionally leave_previous). Actor must be current owner;
 * roster_epoch must be strictly greater than local.
 */
Roe<void> ApplyOwnerTransferredToRoster(GroupRosterStore& roster,
                                        const GroupMembershipCodec::OwnerTransferredPayload& payload,
                                        const std::string& actor_identity);

/**
 * Apply member_left. Actor must match member_identity; reject if actor is still recorded owner;
 * roster_epoch must be strictly greater than local.
 */
Roe<void> ApplyMemberLeftToRoster(GroupRosterStore& roster, const GroupMembershipCodec::MemberLeftPayload& payload,
                                  const std::string& actor_identity);

/**
 * Apply member_removed. Actor must be current owner; roster_epoch must be strictly greater than local.
 */
Roe<void> ApplyMemberRemovedToRoster(GroupRosterStore& roster,
                                     const GroupMembershipCodec::MemberRemovedPayload& payload,
                                     const std::string& actor_identity);

} // namespace pbr
