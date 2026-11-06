#pragma once

#include "common/chat/MessagingLimits.h"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

namespace pbr {

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
  /**
   * G022: "Make a group…" in a call — everyone in it starts picked (each can be unticked); rows are accounts, since
   * people met in the call need not be contacts. Next names the group; Create makes it and invites them.
   */
  CallMakeGroup,
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
  if (mode == PeoplePickerMode::FromDm || mode == PeoplePickerMode::CallMakeGroup) {
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

/**
 * Whether to tell the person picking that invited people must have added them as a contact: the button
 * would create a group or invite into one. Not for a group made in a call: someone in the call with us may be
 * invited there whether or not they have us as a contact (G022).
 */
inline bool ShowInviteNeedsContactHint(PeoplePickerCta cta, PeoplePickerMode mode = PeoplePickerMode::Free) {
  if (mode == PeoplePickerMode::CallMakeGroup) {
    return false;
  }
  return cta == PeoplePickerCta::CreateGroup || cta == PeoplePickerCta::Invite || cta == PeoplePickerCta::Inviting;
}

/** The fewest people a new group's pick holds, the maker aside (a free pick of one is a direct chat). */
inline size_t MinGroupPick(PeoplePickerMode mode) {
  return mode == PeoplePickerMode::Free ? 2 : 1;
}

/**
 * G022: who the "Make a group…" picker lists, all picked to start with: everyone in the call (`call_members`, account
 * ids in the call's order) but this account, each once.
 */
inline std::vector<std::string> CallMakeGroupPick(const std::vector<std::string>& call_members,
                                                  const std::string& self_identity) {
  std::vector<std::string> picked;
  for (const std::string& identity : call_members) {
    if (identity.empty() || identity == self_identity) {
      continue;
    }
    if (std::find(picked.begin(), picked.end(), identity) == picked.end()) {
      picked.push_back(identity);
    }
  }
  return picked;
}

/** G022: names a default group title lists before it counts the rest. */
inline constexpr size_t kCallGroupTitleNames = 3;

/**
 * G022: the default name of a group made in a call, from the people in it (the maker's name first, then those
 * picked): the first kCallGroupTitleNames joined by `separator` ("Ann, Bo, Cy"), then " +N" for the rest. Empty names
 * are skipped; none at all gives `fallback`.
 */
inline std::string DefaultCallGroupTitle(const std::vector<std::string>& names, const std::string& separator,
                                         const std::string& fallback) {
  std::string title;
  size_t listed = 0;
  size_t rest = 0;
  for (const std::string& name : names) {
    if (name.empty()) {
      continue;
    }
    if (listed == kCallGroupTitleNames) {
      ++rest;
      continue;
    }
    title += (listed == 0 ? "" : separator) + name;
    ++listed;
  }
  if (listed == 0) {
    return fallback;
  }
  if (rest > 0) {
    title += " +" + std::to_string(rest);
  }
  return title;
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

/** Contacts that may be invited into the group: not on the roster, not this account, not already invited, invitable. */
inline std::vector<std::string> AddMemberCandidateIds(const std::vector<AddMemberCandidate>& contacts,
                                                      const std::unordered_set<std::string>& roster_identities,
                                                      const std::string& self_identity,
                                                      const std::unordered_set<std::string>& invited_identities = {}) {
  std::vector<std::string> ids;
  for (const AddMemberCandidate& candidate : contacts) {
    if (candidate.identity.empty() || candidate.identity == self_identity ||
        roster_identities.count(candidate.identity) > 0 || invited_identities.count(candidate.identity) > 0) {
      continue;
    }
    ids.push_back(candidate.contact_id);
  }
  return ids;
}

} // namespace pbr
