#pragma once

#include "lib/messaging/GroupMembershipCodec.h"
#include "lib/messaging/GroupRosterStore.h"

#include "common/Error.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Owner-side: an invite this device issued (`outbound`), for `group_id`, which `local_identity`
 * owns, accepted by its invitee before it expired → active member, roster epoch +1. True when the
 * member was added now; false when the invite was already accepted (a re-delivered accept adds no
 * one — a member removed since, or one who left, needs a new invite).
 */
Roe<bool> ApplyInviteAcceptToRoster(GroupRosterStore& roster, const std::string& invite_nonce,
                                    const std::string& member_identity, const std::string& group_id,
                                    const std::string& local_identity, int64_t now_ms);
/** Owner-side: clear a pending invite this device issued (invitee was never an encrypt target). */
Roe<void> ApplyInviteDeclineToRoster(GroupRosterStore& roster, const std::string& invite_nonce,
                                     const std::string& member_identity, const std::string& group_id);

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

/**
 * Apply an owner-signed group_renamed: the actor must be the owner, and the rename must be newer
 * than the title we hold (`title_version`) — a late or re-delivered rename does not bring an old
 * title back. True when the title changed.
 */
Roe<bool> ApplyGroupRenamedToRoster(GroupRosterStore& roster, const GroupMembershipCodec::GroupRenamedPayload& payload,
                                    const std::string& actor_identity);

} // namespace pbr
