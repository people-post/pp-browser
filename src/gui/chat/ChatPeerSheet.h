#pragma once

#include "feature/conversations/ChatPresenter.h"
#include "gui/chat/ChatMenus.h"
#include "gui/chat/ChatScreenState.h"
#include "gui/contacts/ContactsNotifyPorts.h"
#include "gui/contacts/PeoplePickerNotifyPorts.h"

#include <ui/dom/Event.h>

#include "common/PbrCompat.h"

namespace pbr {

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
