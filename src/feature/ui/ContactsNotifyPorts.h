#pragma once

#include <functional>
#include <string>

namespace pbr {

/**
 * Contacts list/detail notify ports for ChatController (no ContactsController::Instance()).
 * Application fills from ContactsController. Clear via BindContactsNotify({}).
 */
struct ContactsNotifyPorts {
  std::function<void()> refresh;
  std::function<void(const std::string& contact_id)> select_contact;
};

} // namespace pbr
