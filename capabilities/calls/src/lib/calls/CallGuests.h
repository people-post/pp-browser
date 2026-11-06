#pragma once

#include "lib/calls/CallTypes.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * V062: where an account stands in a call's group, in the calls' words: what the group's roster and preset (group-chat
 * G017) let them do in its call. The product layer fills it from messaging's roster; calls never names roles.
 */
struct CallGroupStanding {
  /** In the group's roster (any role). */
  bool member = false;
  /** May bring a guest into the group's call (Open: any member; Managed: the owner and admins). */
  bool may_bring_guest = false;
  /** May take a guest out of the group's call (the owner and admins). */
  bool may_remove_guest = false;
  /** May invite someone to the group itself ("Add {name} to the group"). */
  bool may_invite_to_group = false;

  bool operator==(const CallGroupStanding&) const = default;
};

/**
 * `account`'s standing in `group_id`; none when this device holds no roster for the group (a guest's device: it is
 * not in the group). An account the held roster does not list stands as a non-member.
 */
using CallGroupStandingFn =
    std::function<std::optional<CallGroupStanding>(const std::string& group_id, const std::string& account)>;

/** What adding someone to a call from this device is (V062 2). */
enum class CallAddVerdict {
  /** An ordinary invite: no group, or the invitee is one of the group. */
  Plain,
  /** A guest invite (`guest: true`, `brought_by` this account). */
  Guest,
  /** This device may not: the bringer may not bring guests, or this device is a guest's (guests bring nobody). */
  Refused,
};

/**
 * V062 2: adding `invitee` to a call. `group_call`: the call has an `origin_group_id`. `self` / `invitee`: their
 * standing in that group as this device's roster says (none: this device holds no roster — it is a guest's, which
 * cannot tell a member from a stranger, so it adds nobody). Calls with no group: anyone may add anyone.
 */
CallAddVerdict DecideCallAdd(bool group_call, const std::optional<CallGroupStanding>& self,
                             const std::optional<CallGroupStanding>& invitee);

/**
 * V062 2: a roster (or invite roster) entry received for a group's call is taken. `entry`: the entry's standing (none:
 * this device cannot check — a guest's device takes it as given). A member's entry is always taken; a non-member's
 * only as a guest brought by a member allowed to bring guests (`brought_by`, `bringer`). Not for one's own entry.
 */
bool TakesCallRosterEntry(const std::optional<CallGroupStanding>& entry, const std::string& brought_by,
                          const std::optional<CallGroupStanding>& bringer);

/** What a received `call_remove` is taken as (V062 3). */
enum class CallRemoveVerdict {
  /** Someone else is out of the call: taken as their leave (the key rotates without them). */
  Remove,
  /** It names this device's account, a guest here: this device leaves the call. */
  Leave,
  /** Dropped (logged): no group, a member named, a sender who may not remove. */
  Refused,
};

struct CallRemoveCheck {
  /** The call has an `origin_group_id` (calls with no group: nobody removes anyone). */
  bool group_call = false;
  /** It names this device's account. */
  bool names_local = false;
  /** The sender's and the named one's standing as this device's roster says (none: no roster here). */
  std::optional<CallGroupStanding> sender;
  std::optional<CallGroupStanding> target;
  /** The named one's row here is a guest's (it has a `brought_by`). */
  bool target_labelled_guest = false;
};

/**
 * V062 3: a received `call_remove`. Members are never removed from a call (they could tap Join again; removal from the
 * group drops them). A device that holds the roster checks the sender may remove guests; a guest's device cannot
 * check and follows it — about itself (it leaves) and about someone labelled a guest (as the members do).
 */
CallRemoveVerdict DecideCallRemove(const CallRemoveCheck& check);

/** V062 3: this device's account (`self`) may take `target` out of the group's call: owner / admin, a non-member. */
bool MayRemoveFromCall(bool group_call, const std::optional<CallGroupStanding>& self,
                       const std::optional<CallGroupStanding>& target);

/**
 * V062 1: a participant shows as a guest ("Guest" on the tile): a group's call, their row names who brought them, and
 * this device's roster does not list them as a member (one who joined the group since is no guest any more).
 */
bool ShowsAsGuest(bool group_call, const std::string& brought_by, const std::optional<CallGroupStanding>& standing);

/**
 * V062 4: "Add {name} to the group" on a guest: this device's account may invite to the group, and the guest is not in
 * it yet.
 */
bool OffersGroupInvite(bool group_call, const std::optional<CallGroupStanding>& self,
                       const std::optional<CallGroupStanding>& guest);

} // namespace pbr
