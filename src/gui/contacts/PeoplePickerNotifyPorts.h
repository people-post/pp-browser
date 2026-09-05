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
  std::function<void(const std::string& thread_id)> open_for_group_call;
  std::function<void(const std::string& call_id)> open_for_call_add_guest;
};

} // namespace pbr
