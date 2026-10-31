#pragma once

#include "lib/messaging/GroupMembershipCodec.h"
#include "lib/messaging/GroupRosterStatement.h"
#include "lib/messaging/GroupRosterStore.h"
#include "common/thread/IThreadStore.h"

#include "common/Error.h"

#include <optional>
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

/** What ending a group did (G016). */
struct GroupDissolvedApplied {
  /** The group ended here now; false for a group we are not in or one already over (nothing changed). */
  bool applied = false;
  /** The pending invites to or from the group, now void. */
  std::vector<PendingGroupInvite> voided_invites;
};

/**
 * End a group in the roster in one transaction (G016): empty its roster, drop the send target, void its pending
 * invites, mark it dissolved at `roster_epoch` (at least ours) under `owner_identity`, hold `statement_json` (the
 * owner-signed dissolve) to answer member checks, and record `sends` still to make. For the owner's own device,
 * which needs no statement to believe itself. A group we do not hold, or one already dissolved, is left alone
 * (`applied` false).
 */
Roe<GroupDissolvedApplied> EndGroupInRoster(GroupRosterStore& roster, const std::string& group_id,
                                           const std::string& owner_identity, uint64_t roster_epoch,
                                           const std::string& statement_json = {},
                                           const std::vector<PendingGroupSend>& sends = {});

/**
 * Apply a dissolve statement (G016) however it reached us — the owner's DM, a member's answer to our member check,
 * the owner's other device. The same authority as any owner statement: it verifies for the owner we know (or one the
 * chain hands the group to), is not older than our roster, and its owner has not handed the group on by then
 * (against the transfers we hold and the statement's own: a former owner and a stale chain lose, A to B to A wins).
 * A group we are not in, or one already dissolved, is left alone (`applied` false); a statement that does not hold is
 * an error. The statement is held to answer member checks.
 */
Roe<GroupDissolvedApplied> ApplyDissolvedStatement(GroupRosterStore& roster, const GroupRosterStatement& statement,
                                                   const std::string& local_identity);

/** `ApplyDissolvedStatement` for the owner's DM: the sender, `actor_identity`, must also be the owner it names. */
Roe<GroupDissolvedApplied> ApplyGroupDissolvedToRoster(GroupRosterStore& roster, const GroupRosterStatement& statement,
                                                       const std::string& actor_identity,
                                                       const std::string& local_identity);

/**
 * The group's chat gets its one "dissolved" line (G016) unless it has it; true thread id when added, nullopt when
 * the chat is gone or already says so. A redelivery after a crash between ending the group and the line adds it.
 */
Roe<std::optional<std::string>> EnsureDissolvedLine(IThreadStore& store, const std::string& group_id,
                                                    const std::string& owner_identity,
                                                    const std::string& sender_identity);

} // namespace pbr
