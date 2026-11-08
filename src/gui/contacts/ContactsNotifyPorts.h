#pragma once

#include <functional>
#include <string>

namespace pbr {

/**
 * The group owner's "Group admin" card at the bottom of a member's profile sheet (phones, Kenneth 2026-11-04): shown
 * only when the owner opened the sheet from that group. Re-invite carries a grey note (why it is offered); Remove asks
 * first and calls `done` once the person is out, so the sheet can close.
 */
struct GroupAdminCard {
  /** "In this group" row: their role and who added them ("Member · Added by Ann"); the card shows whenever it is set. */
  std::string role_text;
  bool can_reinvite = false;
  std::string reinvite_note;
  /** G017: Make admin, or Remove as admin when `is_admin`. */
  bool can_set_admin = false;
  bool is_admin = false;
  bool can_remove = false;
  std::function<void()> reinvite;
  std::function<void()> set_admin;
  std::function<void(std::function<void()> done)> remove;
  /** G020 6: "Remove everyone {name} added…" (owner, admins) and "Re-invite everyone {name} removed…" (owner). */
  std::string remove_added_label;
  std::function<void()> remove_added;
  std::string reinvite_removed_label;
  std::function<void()> reinvite_removed;
};

/**
 * Contacts list/detail notify ports for ChatController (no ContactsController::Instance()).
 * Application fills from ContactsController. Clear via BindContactsNotify({}).
 */
struct ContactsNotifyPorts {
  std::function<void()> refresh;
  std::function<void(const std::string& contact_id)> select_contact;
  /**
   * From a group (a sender, the group info sheet, the member list): the contact's page as a page sheet on phones
   * (nothing under it changes; swiped down the user is back); the desktop as select_contact.
   */
  std::function<void(const std::string& contact_id)> select_contact_sheet;
  /** The same for a group member who is not a contact: their page in its member state (account id, name, photo). */
  std::function<void(const std::string& account_id, const std::string& name, const std::string& icon_src)>
      show_member_sheet;
  /**
   * Right after select_contact_sheet / show_member_sheet from a group: the owner's admin card for that person on the
   * sheet. Opening any person's page clears it first, so only the owner's tap from that group shows one.
   */
  std::function<void(GroupAdminCard card)> set_group_admin;
  /** Phone Chats "compose": pushes the full-screen New chat page. */
  std::function<void()> open_new_chat;
  /** Home's Favorites, a starred contact with no chat yet: opens (or starts) the chat their page's Message would. */
  std::function<void(const std::string& contact_id)> message_contact;
};

} // namespace pbr
