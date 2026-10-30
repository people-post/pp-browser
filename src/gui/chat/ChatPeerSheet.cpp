#include "gui/chat/ChatPeerSheet.h"

#include "common/thread/ThreadTypes.h"
#include "foundation/i18n/LocalizationService.h"
#include "gui/UserFeedback.h"
#include "lib/people/PeerDisplayLabel.h"

#include <ui/base/SystemInterface.h>

#include <optional>
#include <utility>

namespace pbr {

ChatPeerSheet::ChatPeerSheet(ChatMenusHost& host, ChatPresenter& presenter, const ChatScreenState& chat,
                             const ContactsNotifyPorts& contacts_notify,
                             const PeoplePickerNotifyPorts& people_picker_notify)
    : host_(host), presenter_(presenter), chat_(chat), contacts_notify_(contacts_notify),
      people_picker_notify_(people_picker_notify) {}

void ChatPeerSheet::Open(ui::Event& ev) {
  ConversationsFacade* facade = host_.Facade();
  if (!host_.MessagingIsReady() || !facade || !chat_.show_peer_sheet) {
    return;
  }
  auto thread = facade->GetActiveThread();
  if (!thread) {
    return;
  }
  std::vector<ContextMenuAction> actions;
  if (thread->kind == ThreadKind::Direct) {
    actions = DirectActions(*thread);
  } else if (thread->kind == ThreadKind::Group && thread->group_id) {
    actions = GroupActions(*thread);
  }
  if (actions.empty()) {
    return;
  }
  ContextMenuHost::Instance().ShowActions(MenuPositionBelowEvent(ev), std::move(actions));
}

void ChatPeerSheet::OpenGroupMembers() {
  ConversationsFacade* facade = host_.Facade();
  if (!host_.MessagingIsReady() || !facade || !people_picker_notify_.open_group_members) {
    return;
  }
  auto thread = facade->GetActiveThread();
  if (!thread || thread->kind != ThreadKind::Group || !thread->group_id) {
    return;
  }
  people_picker_notify_.open_group_members(thread->id);
}

std::vector<ContextMenuAction> ChatPeerSheet::DirectActions(const Thread& thread) {
  ConversationsFacade* facade = host_.Facade();
  std::vector<ContextMenuAction> actions;
  const std::string peer_id = thread.peer_identity_value;
  std::optional<std::string> contact_id = facade->ResolveThreadLabel(thread).contact_id;
  if (!contact_id && !thread.participant_contact_ids.empty()) {
    const std::string& candidate = thread.participant_contact_ids.front();
    if (!candidate.empty()) {
      if (auto contact = facade->GetContact(candidate); contact && *contact) {
        contact_id = candidate;
      }
    }
  }
  if (contact_id) {
    actions.push_back({
        "add_people",
        Tr("chat.menu.add_people"),
        nullptr,
        [this, id = *contact_id]() {
          if (people_picker_notify_.open_from_dm) {
            people_picker_notify_.open_from_dm(id);
          }
        },
        "../icons/group.svg",
    });
    actions.push_back({
        "view_contact",
        Tr("chat.menu.view_contact"),
        nullptr,
        [this, id = *contact_id]() {
          if (contacts_notify_.select_contact) {
            contacts_notify_.select_contact(id);
          }
        },
        "../icons/contacts.svg",
    });
  } else if (!peer_id.empty()) {
    actions.push_back({
        "add_contact",
        Tr("chat.menu.add_to_contacts"),
        nullptr,
        [this, peer_id]() { AddToContacts(peer_id); },
        "../icons/contacts.svg",
    });
  }
  if (!peer_id.empty()) {
    actions.push_back({
        "copy_id",
        Tr("contacts.copy_id"),
        nullptr,
        [this, peer_id]() {
          if (ui::SystemInterface* system = ui::GetSystemInterface()) {
            system->SetClipboardText(peer_id.c_str());
          }
          host_.ShowToast(Tr("chat.toast.id_copied"));
          host_.NotifySurfaceChanged();
        },
        "../icons/copy.svg",
    });
  }
  return actions;
}

void ChatPeerSheet::AddToContacts(const std::string& peer_id) {
  auto added = presenter_.AddPeerToContacts(peer_id);
  if (!added) {
    UserFeedback::Fail(Tr(added.error().message));
    host_.NotifySurfaceChanged();
    return;
  }
  if (contacts_notify_.select_contact) {
    contacts_notify_.select_contact(*added);
  }
}

std::vector<ContextMenuAction> ChatPeerSheet::GroupActions(const Thread& thread) {
  ConversationsFacade* facade = host_.Facade();
  std::vector<ContextMenuAction> actions;
  const std::string thread_id = thread.id;
  const std::string group_id = *thread.group_id;
  const std::string current_local = thread.local_title;
  const std::string shared_default = facade->ResolveThreadLabel(thread).shared_title.value_or(thread.title);

  actions.push_back({
      "group_members",
      Tr("chat.menu.group_members"),
      nullptr,
      [this]() { OpenGroupMembers(); },
      "../icons/group.svg",
  });
  actions.push_back({
      "rename_for_me",
      Tr("chat.group.rename_for_me"),
      nullptr,
      [this, thread_id, current_local, shared_default]() {
        host_.ShowPrompt(Tr("chat.group.rename_for_me"), Tr("chat.group.rename_for_me_prompt"),
                         current_local.empty() ? shared_default : current_local,
                         [this, thread_id](bool ok, std::string value) {
                           ConversationsFacade* facade = host_.Facade();
                           if (!ok || !facade) {
                             return;
                           }
                           if (auto saved = facade->SetThreadLocalTitle(thread_id, value); !saved) {
                             UserFeedback::Fail(saved.error().message);
                           }
                           host_.NotifySurfaceChanged();
                         });
      },
      "../icons/message.svg",
  });
  if (!current_local.empty()) {
    actions.push_back({
        "clear_my_name",
        Tr("chat.group.clear_my_name"),
        nullptr,
        [this, thread_id]() {
          if (ConversationsFacade* facade = host_.Facade()) {
            (void)facade->SetThreadLocalTitle(thread_id, "");
          }
          host_.NotifySurfaceChanged();
        },
        "../icons/trash.svg",
    });
  }

  const ChatGroupSheet sheet = presenter_.GroupSheetOf(group_id);
  if (sheet.is_owner) {
    actions.push_back({
        "rename_for_everyone",
        Tr("chat.group.rename_for_everyone"),
        nullptr,
        [this, group_id, shared_default]() {
          host_.ShowPrompt(Tr("chat.group.rename_for_everyone"), Tr("chat.group.rename_for_everyone_prompt"),
                           shared_default, [this, group_id](bool ok, std::string value) {
                             ConversationsFacade* facade = host_.Facade();
                             if (!ok || !facade) {
                               return;
                             }
                             if (value.empty()) {
                               UserFeedback::Fail(Tr("chat.group.title_required"));
                               host_.NotifySurfaceChanged();
                               return;
                             }
                             if (auto renamed = facade->RenameGroupShared(group_id, value); !renamed) {
                               UserFeedback::Fail(renamed.error().message);
                             } else {
                               facade->NotifyThreadChanged();
                             }
                             host_.NotifySurfaceChanged();
                           });
        },
        "../icons/group.svg",
    });
    for (const auto& [identity, name] : sheet.unreachable_members) {
      actions.push_back({
          "remove_unreachable_" + identity,
          Tr("chat.group.remove_unreachable", {{"name", name}}),
          nullptr,
          [this, group_id, identity = identity, name = name]() {
            host_.ShowConfirm(Tr("chat.group.remove_member_title"),
                              Tr("chat.group.remove_member_confirm", {{"name", name}}),
                              [this, group_id, identity](bool ok) {
                                ConversationsFacade* facade = host_.Facade();
                                if (!ok || !facade) {
                                  return;
                                }
                                if (auto removed = facade->RemoveMemberByIdentity(group_id, identity); !removed) {
                                  UserFeedback::Fail(removed.error().message);
                                } else {
                                  facade->NotifyThreadChanged();
                                }
                                host_.NotifySurfaceChanged();
                              });
          },
          "../icons/trash.svg",
          true,
      });
    }
  } else if (sheet.owner_unreachable) {
    actions.push_back({
        "rename_for_everyone",
        Tr("chat.group.rename_for_everyone"),
        nullptr,
        [this]() {
          host_.ShowToast(Tr("chat.group.owner_only"));
          host_.NotifySurfaceChanged();
        },
        "../icons/group.svg",
    });
  }
  return actions;
}

} // namespace pbr
