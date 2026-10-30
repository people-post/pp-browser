#pragma once

#include <string>
#include <unordered_set>
#include <vector>

namespace pbr {

/** A group holds at most this many members (the owner's device checks it; G014 may add a hard stop). */
inline constexpr int kGroupMemberLimit = 50;

/**
 * "Re-invite" on the member list. It only helps if the invitee, who lost the group, can accept it and then be
 * heard by the owner and the others. That holds when they only cleared the group, not when they came back on a
 * fresh profile (ReInvitedMemberTest.DISABLED_AMemberOnAFreshProfile…, a messaging gap): off until that passes.
 */
inline constexpr bool kGroupReinviteEnabled = false;

enum class PeoplePickerMode {
  Free,
  FromDm,
  /** Pick group members to invite when starting a call from a group chat. */
  GroupCall,
  /** Pick contacts to invite mid-call (guest / late join). */
  CallAddGuest,
  /** Read-only roster of a group; the owner can add, remove and re-invite. */
  GroupMembers,
  /** Pick contacts to invite into an existing group. */
  GroupAddMember,
};

enum class PeoplePickerCta {
  Disabled,
  Message,
  CreateGroup,
  StartCall,
  Invite,
  Inviting,
};

/** free_selected_count excludes locked rows. `sending`: invites to an existing group are going out. */
inline PeoplePickerCta ComputePeoplePickerCta(PeoplePickerMode mode, int free_selected_count,
                                              bool sending = false) {
  if (mode == PeoplePickerMode::GroupMembers) {
    return PeoplePickerCta::Disabled;
  }
  if (mode == PeoplePickerMode::GroupAddMember) {
    if (free_selected_count < 1) {
      return PeoplePickerCta::Disabled;
    }
    return sending ? PeoplePickerCta::Inviting : PeoplePickerCta::Invite;
  }
  if (mode == PeoplePickerMode::GroupCall || mode == PeoplePickerMode::CallAddGuest) {
    return free_selected_count >= 1 ? PeoplePickerCta::StartCall : PeoplePickerCta::Disabled;
  }
  if (mode == PeoplePickerMode::FromDm) {
    return free_selected_count >= 1 ? PeoplePickerCta::CreateGroup : PeoplePickerCta::Disabled;
  }
  if (free_selected_count <= 0) {
    return PeoplePickerCta::Disabled;
  }
  if (free_selected_count == 1) {
    return PeoplePickerCta::Message;
  }
  return PeoplePickerCta::CreateGroup;
}

/** Roster members plus the people picked so far have reached the limit. */
inline bool GroupIsFull(int roster_count, int selected_count) {
  return roster_count + selected_count >= kGroupMemberLimit;
}

/** When the group is full only the rows already picked stay usable (so they can be un-picked). */
inline bool GroupAddRowDisabled(int roster_count, int selected_count, bool row_selected) {
  return !row_selected && GroupIsFull(roster_count, selected_count);
}

struct AddMemberCandidate {
  std::string contact_id;
  /** Account id the contact would be invited as; empty when the contact has none. */
  std::string identity;
};

/** Contacts that may be invited into the group: not on the roster, not this account, invitable. */
inline std::vector<std::string> AddMemberCandidateIds(const std::vector<AddMemberCandidate>& contacts,
                                                      const std::unordered_set<std::string>& roster_identities,
                                                      const std::string& self_identity) {
  std::vector<std::string> ids;
  for (const AddMemberCandidate& candidate : contacts) {
    if (candidate.identity.empty() || candidate.identity == self_identity ||
        roster_identities.count(candidate.identity) > 0) {
      continue;
    }
    ids.push_back(candidate.contact_id);
  }
  return ids;
}

} // namespace pbr
