#pragma once

#include "feature/presenters/ChatPresenter.h"
#include "gui/chat/ChatMenus.h"
#include "gui/chat/ChatScreenState.h"
#include "gui/contacts/ContactsNotifyPorts.h"
#include "gui/contacts/PeoplePickerNotifyPorts.h"

#include <ui/dom/Event.h>

#include <functional>
#include <optional>
#include <string>

#include "common/PbrCompat.h"

namespace pbr {

/** What tapping the chat header's person opens (DEV/PLAN-fewer-taps-1.md 2): the small menu, or their page. */
enum class ChatPeerTarget { Menu, ContactPage, MemberPage };

/**
 * Phones (`compact`), a 1:1 chat: a contact opens their page, a person who is not a contact (and whose chat id is an
 * account id, which is what the member page looks up) opens the member page. A group chat, an AI chat, the desktop
 * and any other kind of id keep the menu.
 */
ChatPeerTarget ChooseChatPeerTarget(bool compact, ThreadKind kind, bool is_contact, bool peer_is_account);

/** The contact a 1:1 thread is with (as DirectActions found it for the menu), if the person is one. */
std::optional<std::string> DirectPeerContactId(ConversationsFacade& facade, const Thread& thread);

/**
 * The open chat's peer sheet (the header's person / group button), a menu. A direct chat: add people,
 * view or add the contact, copy the id. A group: rename it for me / for everyone (the owner), and the
 * owner removes members it can no longer reach. The decisions are ChatPresenter's.
 *
 * UI thread.
 */
class ChatPeerSheet {
public:
  ChatPeerSheet(ChatMenusHost& host, ChatPresenter& presenter, const ChatScreenState& chat,
                const ContactsNotifyPorts& contacts_notify, const PeoplePickerNotifyPorts& people_picker_notify);

  void Open(ui::Event& ev);
  /** Phones, a 1:1 chat: opens the person's page instead of the menu. False when the menu should open instead. */
  bool OpenPeerPage(bool compact);
  /** The group's member list (the header's "N members", and a peer sheet item). */
  void OpenGroupMembers();

  // The group's naming, here and in the phone's group info sheet. `after` runs once the name was saved.
  /** A name for the group on this device only (asks for it). */
  void RenameForMe(const std::string& thread_id, std::function<void()> after = {});
  /** The owner: the group's name for everyone (asks for it). */
  void RenameForEveryone(const std::string& thread_id, std::function<void()> after = {});
  /** Drops this device's own name for the group. */
  void ClearMyName(const std::string& thread_id);

private:
  std::vector<ContextMenuAction> DirectActions(const Thread& thread);
  std::vector<ContextMenuAction> GroupActions(const Thread& thread);
  void AddToContacts(const std::string& peer_id);

  ChatMenusHost& host_;
  ChatPresenter& presenter_;
  const ChatScreenState& chat_;
  const ContactsNotifyPorts& contacts_notify_;
  const PeoplePickerNotifyPorts& people_picker_notify_;
};

} // namespace pbr
