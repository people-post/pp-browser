#pragma once

namespace pbr {

enum class PeoplePickerMode {
  Free,
  FromDm,
};

enum class PeoplePickerCta {
  Disabled,
  Message,
  CreateGroup,
};

/** free_selected_count excludes locked rows. */
inline PeoplePickerCta ComputePeoplePickerCta(PeoplePickerMode mode, int free_selected_count) {
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
