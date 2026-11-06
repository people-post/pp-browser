#pragma once

#include "lib/messaging/GroupTypes.h"

namespace pbr {

/** What a group member may try to do (G017). */
enum class GroupAction : uint8_t {
  Send,
  Invite,
  MakeInviteLink,
  BringCallGuest,
  Rename,
  /** Remove someone from the group; who depends on their role (`target`). */
  Remove,
  /** Bring back someone removed (G018 R2, G020). */
  Readmit,
  /** Revoke an invite link someone else made (G021); a link's maker may always revoke its own. */
  RevokeInviteLink,
  /** Leave the group (G018): anyone but the owner, who transfers or dissolves instead (G009, G016). */
  Leave,
  RemoveCallGuest,
  MentionEveryone,
  SetRole,
  SetPreset,
  Transfer,
  Dissolve,
};

/**
 * Whether `actor` may do `action` in a group under `preset` (G017). `target`: the role of whom `Remove` (or
 * `SetRole`) acts on. The one table every check asks. Until the change log (o2, G018) ships, only the owner signs
 * what the wire carries, so the workflow still lets only the owner invite, remove and rename; this states the rule
 * those checks move to; the change rules (GroupRosterRules.h) already ask it.
 */
bool GroupAllows(GroupAction action, MemberRole actor, GroupPreset preset, MemberRole target = MemberRole::Member);

} // namespace pbr
