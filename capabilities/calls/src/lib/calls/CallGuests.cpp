#include "lib/calls/CallGuests.h"

namespace pbr {

CallAddVerdict DecideCallAdd(const bool group_call, const std::optional<CallGroupStanding>& self,
                             const std::optional<CallGroupStanding>& invitee) {
  if (!group_call) {
    return CallAddVerdict::Plain;
  }
  if (!self || !self->member) {
    return CallAddVerdict::Refused; // a guest's device: guests do not bring guests
  }
  if (invitee && invitee->member) {
    return CallAddVerdict::Plain;
  }
  return self->may_bring_guest ? CallAddVerdict::Guest : CallAddVerdict::Refused;
}

bool TakesCallRosterEntry(const std::optional<CallGroupStanding>& entry, const std::string& brought_by,
                          const std::optional<CallGroupStanding>& bringer) {
  if (!entry || entry->member) {
    return true;
  }
  return !brought_by.empty() && bringer && bringer->member && bringer->may_bring_guest;
}

CallRemoveVerdict DecideCallRemove(const CallRemoveCheck& check) {
  if (!check.group_call) {
    return CallRemoveVerdict::Refused;
  }
  if (check.target && check.target->member) {
    return CallRemoveVerdict::Refused; // members are not removed from a call
  }
  if (!check.target) {
    // No roster here (a guest's device): nothing to check the sender by. About itself it leaves; about someone
    // labelled a guest it follows the members.
    if (check.names_local) {
      return CallRemoveVerdict::Leave;
    }
    return check.target_labelled_guest ? CallRemoveVerdict::Remove : CallRemoveVerdict::Refused;
  }
  const bool sender_may = check.sender && check.sender->member && check.sender->may_remove_guest;
  if (!sender_may) {
    return CallRemoveVerdict::Refused;
  }
  return check.names_local ? CallRemoveVerdict::Leave : CallRemoveVerdict::Remove;
}

bool MayRemoveFromCall(const bool group_call, const std::optional<CallGroupStanding>& self,
                       const std::optional<CallGroupStanding>& target) {
  return group_call && self && self->member && self->may_remove_guest && target && !target->member;
}

bool ShowsAsGuest(const bool group_call, const std::string& brought_by,
                  const std::optional<CallGroupStanding>& standing) {
  return group_call && !brought_by.empty() && !(standing && standing->member);
}

bool OffersGroupInvite(const bool group_call, const std::optional<CallGroupStanding>& self,
                       const std::optional<CallGroupStanding>& guest) {
  return group_call && self && self->member && self->may_invite_to_group && guest && !guest->member;
}

} // namespace pbr
