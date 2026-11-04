#pragma once

#include <functional>
#include <string>

namespace pbr {

/**
 * People-picker open hooks for ChatController and CallController
 * (no PeoplePickerController::Instance()). Application fills from PeoplePickerController.
 * Clear via BindPeoplePickerNotify({}).
 */
struct PeoplePickerNotifyPorts {
  std::function<void()> open_free;
  std::function<void(const std::string& locked_contact_id)> open_from_dm;
  std::function<void(const std::string& thread_id, bool video_allowed)> open_for_group_call;
  std::function<void(const std::string& call_id)> open_for_call_add_guest;
  /** The member list of the group chat `thread_id`. */
  std::function<void(const std::string& thread_id)> open_group_members;
  /** Add members to the group chat `thread_id` (the group info sheet's "Add members"). */
  std::function<void(const std::string& thread_id)> open_group_add_members;
  /** A member tapped on the group info sheet: what the member list does for that tap. */
  std::function<void(const std::string& thread_id, const std::string& identity)> tap_group_member;
  /**
   * A member's profile was just opened from the group chat `thread_id` (a tap on their avatar or name; `sender` is a
   * contact id or an account id): the owner gets the "Group admin" card on it.
   */
  std::function<void(const std::string& thread_id, const std::string& sender)> offer_group_admin;
};

} // namespace pbr
