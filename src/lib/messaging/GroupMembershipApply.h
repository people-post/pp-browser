#pragma once

#include "lib/messaging/GroupMembershipCodec.h"
#include "lib/messaging/GroupRosterStatement.h"
#include "lib/messaging/GroupRosterStore.h"

#include "common/Error.h"

#include <string>
#include <vector>
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

/** What `ApplyGroupDissolvedToRoster` did. */
struct GroupDissolvedApplied {
  /** The group ended here now; false for a group we are not in or one already over (nothing changed). */
  bool applied = false;
  /** The pending invites to or from the group, now void. */
  std::vector<PendingGroupInvite> voided_invites;
};

/**
 * End a group in the roster (G016): empty its roster, drop the send target, void its pending invites and mark it
 * dissolved at `roster_epoch` (at least ours) under `owner_identity`. For the owner's own device, which needs no
 * statement to believe itself; a member's goes through `ApplyGroupDissolvedToRoster`. A group we do not hold, or
 * one already dissolved, is left alone (`applied` false).
 */
Roe<GroupDissolvedApplied> EndGroupInRoster(GroupRosterStore& roster, const std::string& group_id,
                                           const std::string& owner_identity, uint64_t roster_epoch);

/**
 * Apply a `group_dissolved` (G016) — the same rule on the owner's device and on every member's: `statement` is
 * the owner-signed roster with `dissolved`, sent by `actor_identity`, who must be the owner it names, and the
 * owner we know or one it handed the group on to. Empties the roster, drops the send target, voids pending
 * invites and marks the group dissolved (sync state). A group we are not in, or one already dissolved, is left
 * alone (`applied` false); a statement that does not hold is an error.
 */
Roe<GroupDissolvedApplied> ApplyGroupDissolvedToRoster(GroupRosterStore& roster, const GroupRosterStatement& statement,
                                                       const std::string& actor_identity,
                                                       const std::string& local_identity);

} // namespace pbr
