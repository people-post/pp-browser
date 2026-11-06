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
 * those checks move to.
 */
bool GroupAllows(GroupAction action, MemberRole actor, GroupPreset preset, MemberRole target = MemberRole::Member);

} // namespace pbr
