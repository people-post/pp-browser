#pragma once

namespace pbr {

enum class PeoplePickerMode {
  Free,
  FromDm,
  /** Pick group members to invite when starting a call from a group chat. */
  GroupCall,
  /** Pick contacts to invite mid-call (guest / late join). */
  CallAddGuest,
};

enum class PeoplePickerCta {
  Disabled,
  Message,
  CreateGroup,
  StartCall,
};

/** free_selected_count excludes locked rows. */
inline PeoplePickerCta ComputePeoplePickerCta(PeoplePickerMode mode, int free_selected_count) {
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

} // namespace pbr
