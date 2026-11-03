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
  /** A group member who is not a contact: their page in its member state (account id, shown name, photo path). */
  std::function<void(const std::string& account_id, const std::string& name, const std::string& icon_src)> show_member;
  /** Phone Chats "compose": pushes the full-screen New chat page. */
  std::function<void()> open_new_chat;
};

} // namespace pbr
