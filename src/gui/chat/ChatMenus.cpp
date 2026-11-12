#include "gui/chat/ChatMenus.h"

#include "common/Utilities.h"
#include "common/thread/ThreadTypes.h"
#include "feature/presenters/PendingUpload.h"
#include "feature/presenters/QuickReactions.h"
#include "feature/presenters/SessionMenu.h"
#include "foundation/i18n/LocalizationService.h"
#include "common/runtime/AppRuntime.h"
#include "common/PbrCompat.h"
#include "gui/UserFeedback.h"
#include "lib/delivery/SendRelayOptions.h"

#include <ui/base/SystemInterface.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/SelectionController.h>

#include <algorithm>
#include <optional>
#include <utility>

namespace pbr {

namespace {

/** The element's nearest ancestor carrying `attribute` (sidebar.rml sets `session-id`, rows `message-id`), or empty. */
std::string FindAttributeUp(ui::Element* element, const char* attribute) {
  for (ui::Element* cur = element; cur; cur = cur->GetParentNode()) {
    if (cur->HasAttribute(attribute)) {
      const ui::String value = cur->GetAttribute(attribute, ui::String());
      return std::string(value.c_str());
    }
  }
  return {};
}

void CopyToClipboard(const std::string& text) {
  if (ui::SystemInterface* system = ui::GetSystemInterface()) {
    system->SetClipboardText(text);
  }
}

} // namespace

void RunBlockFlow(ChatMenusHost& host, ChatPresenter& presenter, const ContactsNotifyPorts& contacts_notify,
                  const std::optional<std::string>& contact_id, const std::string& peer_id, const std::string& name,
                  const bool block) {
  const auto apply = [&host, &presenter, &contacts_notify, contact_id, peer_id, name, block]() {
    ConversationsFacade* facade = host.Facade();
    if (!facade) {
      return;
    }
    if (!contact_id) { // a stranger: saved as a blocked contact
      if (auto blocked = presenter.BlockStranger(peer_id); !blocked) {
        UserFeedback::Fail(Tr(blocked.error().message));
        host.NotifySurfaceChanged();
        return;
      }
    } else if (auto set = facade->SetContactTrust(*contact_id, block ? TrustLevel::Blocked : TrustLevel::Unknown);
               !set) {
      UserFeedback::Fail(Tr("contacts.err.trust"));
      host.NotifySurfaceChanged();
      return;
    }
    if (contacts_notify.refresh) {
      contacts_notify.refresh();
    }
    host.ShowToast(Tr(block ? "contacts.toast.blocked" : "contacts.toast.unblocked", {{"name", name}}));
    host.AfterThreadPrefsChanged(); // the row shows (or drops) its "Blocked" mark at once
  };
  if (!block) {
    apply();
    return;
  }
  host.ShowConfirm(
      Tr("chat.menu.block_confirm_title"), Tr("chat.menu.block_confirm_body"),
      [apply](const bool ok) {
        if (ok) {
          apply();
        }
      },
      true);
}

ChatMenus::ChatMenus(ChatMenusHost& host, ChatPresenter& presenter, const ChatScreenState& chat,
                     WorkingSetController& working_set, const ContactsNotifyPorts& contacts_notify,
                     const EmojiPickerNotifyPorts& emoji_picker_notify, const ShellFeedbackPorts& shell_feedback,
                     const CallActionsPorts& call_actions)
    : host_(host),
      presenter_(presenter),
      chat_(chat),
      working_set_(working_set),
      contacts_notify_(contacts_notify),
      emoji_picker_notify_(emoji_picker_notify),
      shell_feedback_(shell_feedback),
      call_actions_(call_actions) {}

void InsertEditAction(std::vector<ContextMenuAction>& actions, std::function<void()> start_edit) {
  ContextMenuAction edit{"edit_message", Tr("chat.menu.edit"), nullptr, std::move(start_edit),
                         "../icons/v2/pencil.svg"};
  const auto copy = std::find_if(actions.begin(), actions.end(),
                                 [](const ContextMenuAction& action) { return action.id == "copy_message"; });
  actions.insert(copy == actions.end() ? actions.end() : std::next(copy), std::move(edit));
}

void RemoveActionsWhileEditing(std::vector<ContextMenuAction>& actions) {
  std::erase_if(actions, [](const ContextMenuAction& action) {
    return action.id == "edit_message" || action.id == "reply_message" || action.id == "ask_ai_message" ||
           action.id == "ask_ai_item";
  });
}

std::vector<ContextMenuAction> ChatMenus::ForRequest(const ContextMenuRequest& request) {
  std::vector<ContextMenuAction> actions;
  ConversationsFacade* facade = host_.Facade();
  if (!host_.MessagingIsReady() || !facade) {
    return actions;
  }
  if (const std::string session_id = FindAttributeUp(request.target, "session-id"); !session_id.empty()) {
    return SessionActions(session_id, request.position);
  }
  if (chat_.compose_disabled) {
    return actions;
  }
  // An item the user can pass on (an article in a feed list): Ask AI / Copy / Share. The nearest of
  // such an item and a message row wins.
  for (ui::Element* cur = request.target; cur && !cur->HasAttribute("message-id"); cur = cur->GetParentNode()) {
    if (!cur->HasAttribute("share-text")) {
      continue;
    }
    std::string shared = cur->GetAttribute("share-text", ui::String()).c_str();
    const std::string url = cur->GetAttribute("share-url", ui::String()).c_str();
    if (!url.empty()) {
      // "<label><url>": a PP bubble shows the label as the link (PlainTextLinks.h); pasted elsewhere it
      // still reads as a label followed by the address.
      shared += "\n" + Tr("feed.result.open") + url;
    }
    // In a direct chat the question needs the "@ai " prefix; an AI thread takes it as it is.
    auto active = facade->GetActiveThread();
    const std::string prefix = active && active->kind == ThreadKind::Direct ? "@ai " : "";
    // The list panel closes so the composer with the quote bar is what the user sees next.
    actions.push_back({"ask_ai_item", Tr("chat.menu.ask_ai"), nullptr,
                       [this, shared, prefix]() {
                         AppRuntime::PostUI([this, shared, prefix]() {
                           working_set_.Clear();
                           host_.StartQuoteReply(shared, prefix);
                         });
                       },
                       "../icons/v2/sparkle.svg"});
    actions.push_back(
        {"copy_item", Tr("common.copy"), nullptr, [shared]() { CopyToClipboard(shared); }, "../icons/v2/copy.svg"});
    const ui::Vector2i position = request.position;
    // Opened after this menu is gone: a menu cannot replace itself from inside its own click.
    actions.push_back({"share_item", Tr("chat.menu.share"), nullptr,
                       [this, shared, position]() {
                         AppRuntime::PostUI([this, shared, position]() { OpenShareTargets(shared, position); });
                       },
                       "../icons/v2/share.svg"});
    if (chat_.editing_message) {
      RemoveActionsWhileEditing(actions);
    }
    return actions;
  }

  const std::string message_id = FindAttributeUp(request.target, "message-id");
  if (message_id.empty() || IsPendingUploadRowId(message_id) || host_.IsDeletedMessage(message_id)) {
    return actions; // a deleted message takes no reaction, reply, copy or second delete; a picture being sent is none
  }
  if (host_.IsCallRecord(message_id)) {
    return actions; // a call record is one tap (call back) and no menu (Kenneth 2026-11-12)
  }
  // Reactions sit in the row above the message, the rest in the list below it: the user's last-used first.
  for (const std::string& emoji : QuickReactions(host_.RecentEmojis())) {
    ContextMenuAction react{"react_" + emoji, emoji, nullptr, [this, message_id, emoji]() {
                              host_.ToggleReaction(message_id, emoji);
                              host_.RememberEmoji(emoji);
                            }};
    react.quick = true;
    actions.push_back(std::move(react));
  }
  ContextMenuAction more{"react_more", "+", nullptr, [this, message_id]() { OpenReactionMore(message_id); }};
  more.quick = true;
  more.pinned = true; // stays put while the recent emoji scroll (Kenneth 2026-11-05 原话 93)
  // Every emoji in a sheet of its own, about a third of the screen (Kenneth 2026-11-05 原话 81; EmojiPickerController).
  actions.push_back(std::move(more));

  // A contact card: its person can be added (the card is checked first).
  if (host_.IsContactCard(message_id)) {
    actions.push_back({"add_contact_from_card", Tr("chat.menu.add_to_contacts"), nullptr,
                       [this, message_id]() { host_.AddContactFromCard(message_id); }, "../icons/v2/user-plus.svg"});
  }

  // Copy takes the pointer selection when there is one (desktop), otherwise the whole message.
  std::string text;
  if (request.context) {
    if (ui::SelectionController* selection = request.context->GetSelectionController()) {
      text = selection->GetSelectedText();
    }
  }
  if (text.empty()) {
    text = host_.MessagePlainText(message_id);
  }
  if (!text.empty()) {
    // Reply: the message shows as a quote bar above the composer and is attached to the next send.
    actions.push_back({"reply_message", Tr("chat.menu.reply"), nullptr, [this, text]() { host_.StartQuoteReply(text); },
                       "../icons/v2/reply.svg"});
    actions.push_back(
        {"copy_message", Tr("common.copy"), nullptr, [text]() { CopyToClipboard(text); }, "../icons/v2/copy.svg"});
    // Ask AI: the message goes into the quote bar and "@ai " into the input; the user adds the question.
    if (auto active = facade->GetActiveThread(); active && active->kind == ThreadKind::Direct) {
      actions.push_back({"ask_ai_message", Tr("chat.menu.ask_ai"), nullptr,
                         [this, text]() { host_.StartQuoteReply(text, "@ai "); }, "../icons/v2/sparkle.svg"});
    }
  }
  // Edit: our own text, within its 15 minutes (DEV/PLAN-message-edit.md); right under Copy. While the chat is editing a
  // message, nothing that would start another edit or a reply (the words being edited stay as typed).
  if (chat_.editing_message) {
    RemoveActionsWhileEditing(actions);
  } else if (host_.CanEditMessage(message_id)) {
    InsertEditAction(actions, [this, message_id]() { host_.StartEditMessage(message_id); });
  }
  ContextMenuAction remove{
      "delete_message", Tr("common.delete"), nullptr,
      [this, message_id, position = request.position]() { OpenDeleteChoice(message_id, position); }};
  remove.danger = true;
  remove.icon = "../icons/v2/trash.svg";
  actions.push_back(std::move(remove));
  return actions;
}

std::string ChatMenus::SessionTitle(const std::string& thread_id) {
  ConversationsFacade* facade = host_.Facade();
  if (!facade) {
    return {};
  }
  auto found = facade->GetThread(thread_id);
  if (!found || !*found) {
    return {};
  }
  const std::string title = facade->ResolveThreadLabel(**found).title;
  // The stored default AI title is English; the row shows it in the UI language.
  return (*found)->kind == ThreadKind::Ai && title == "New chat" ? Tr("chat.new_chat") : title;
}

std::string ChatMenus::MenuTitleFor(const ContextMenuRequest& request) {
  if (const std::string session_id = FindAttributeUp(request.target, "session-id"); !session_id.empty()) {
    return SessionTitle(session_id);
  }
  if (const std::string message_id = FindAttributeUp(request.target, "message-id");
      !message_id.empty() && !IsPendingUploadRowId(message_id)) {
    return Tr("chat.menu.message_title");
  }
  return {};
}

void ChatMenus::RunSessionSwipeAction(const std::string& thread_id, const SessionSwipeAction action) {
  // The menu offers one of each pair (pin / unpin, ...) depending on the row's state; Delete is "leave" for a group.
  const char* ids[2] = {nullptr, nullptr};
  switch (action) {
    case SessionSwipeAction::More:
      // The long-press menu itself (a bottom action sheet on phones).
      ContextMenuHost::Instance().ShowActions(ui::Vector2i(0, 0), SessionActions(thread_id, ui::Vector2i(0, 0)), {},
                                              SessionTitle(thread_id));
      return;
    case SessionSwipeAction::Archive:
      ids[0] = "session_archive";
      ids[1] = "session_unarchive";
      break;
    case SessionSwipeAction::Delete:
      ids[0] = "session_delete";
      ids[1] = "session_leave_group";
      break;
    case SessionSwipeAction::Pin:
      ids[0] = "session_pin";
      ids[1] = "session_unpin";
      break;
    case SessionSwipeAction::Read:
      ids[0] = "session_mark_read";
      ids[1] = "session_mark_unread";
      break;
    case SessionSwipeAction::Rename:
      ids[0] = "session_rename"; // offered for AI conversations only
      break;
  }
  for (ContextMenuAction& item : SessionActions(thread_id, ui::Vector2i(0, 0))) {
    // An action with no second id (e.g. Rename) leaves ids[1] null; comparing a std::string to a null char* would
    // strlen(nullptr) and crash, so guard each id before the compare.
    const bool match = (ids[0] && item.id == ids[0]) || (ids[1] && item.id == ids[1]);
    if (match && item.run) {
      item.run();
      return;
    }
  }
}

void ChatMenus::RenameAiThread(const std::string& thread_id) {
  ConversationsFacade* facade = host_.Facade();
  if (!facade) {
    return;
  }
  auto thread = facade->GetThread(thread_id);
  if (!thread || !*thread || (*thread)->kind != ThreadKind::Ai) {
    return;
  }
  host_.ShowPrompt(Tr("chat.ai_rename.title"), Tr("chat.ai_rename.prompt"), SessionTitle(thread_id),
                   [this, thread_id](const bool ok, std::string value) {
                     if (!ok) {
                       return;
                     }
                     // An empty name goes back to the automatic title.
                     if (auto saved = presenter_.SetThreadLocalTitle(thread_id, value); !saved) {
                       UserFeedback::Fail(saved.error().message);
                     } else {
                       host_.AfterThreadPrefsChanged();
                     }
                     host_.NotifySurfaceChanged();
                   });
}

std::vector<ContextMenuAction> ChatMenus::SessionActions(const std::string& thread_id, const ui::Vector2i position) {
  std::vector<ContextMenuAction> actions;
  ConversationsFacade* facade = host_.Facade();
  if (!facade) {
    return actions;
  }
  auto found = facade->GetThread(thread_id);
  if (!found || !*found) {
    return actions;
  }
  const Thread& thread = **found;

  // Same rule as the chat header's "View contact": the resolved contact, else the first saved participant.
  std::optional<std::string> contact_id;
  if (thread.kind == ThreadKind::Direct) {
    contact_id = facade->ResolveThreadLabel(thread).contact_id;
    if (!contact_id && !thread.participant_contact_ids.empty() && !thread.participant_contact_ids.front().empty()) {
      const std::string& candidate = thread.participant_contact_ids.front();
      if (auto contact = facade->GetContact(candidate); contact && *contact) {
        contact_id = candidate;
      }
    }
  }

  SessionMenuState state;
  state.kind = thread.kind == ThreadKind::Group    ? SessionKind::Group
               : thread.kind == ThreadKind::Direct ? SessionKind::Direct
                                                   : SessionKind::Ai;
  state.has_contact = contact_id.has_value();
  state.has_peer_identity = !thread.peer_identity_value.empty();
  if (contact_id) {
    if (auto contact = facade->GetContact(*contact_id); contact && *contact) {
      state.blocked = (*contact)->local.trust == TrustLevel::Blocked;
    }
  }
  const ThreadLocalPrefs prefs = presenter_.PrefsOf(thread_id).value_or(ThreadLocalPrefs{});
  state.unread = SessionIsUnread(thread.unread_count, prefs.marked_unread);
  state.pinned = prefs.pinned_at != 0;
  state.muted = ThreadIsMuted(prefs.muted_until, util::NowUnixMs());
  state.mute_mentions = prefs.mute_mentions;
  state.share_ai_confirmed = prefs.share_ai_confirmed;
  state.archived = prefs.archived;

  for (const SessionMenuItem item : SessionMenuItems(state)) {
    ContextMenuAction action;
    action.danger = SessionMenuItemIsDestructive(item);
    action.icon = SessionMenuIcon(item);
    switch (item) {
      case SessionMenuItem::MarkUnread:
        action.id = "session_mark_unread";
        action.label = Tr("sidebar.menu.mark_unread");
        // A mark, not a message count: the row shows a dot and keeps its place in the list.
        action.run = [this, thread_id]() {
          presenter_.MarkUnread(thread_id);
          host_.AfterThreadPrefsChanged();
        };
        break;
      case SessionMenuItem::MarkRead:
        action.id = "session_mark_read";
        action.label = Tr("sidebar.menu.mark_read");
        action.run = [this, thread_id]() {
          presenter_.MarkRead(thread_id);
          host_.AfterThreadPrefsChanged();
        };
        break;
      case SessionMenuItem::Pin:
      case SessionMenuItem::Unpin:
        action.id = item == SessionMenuItem::Pin ? "session_pin" : "session_unpin";
        action.label = Tr(item == SessionMenuItem::Pin ? "sidebar.menu.pin" : "sidebar.menu.unpin");
        action.run = [this, thread_id, pin = item == SessionMenuItem::Pin]() {
          presenter_.SetPinned(thread_id, pin);
          host_.AfterThreadPrefsChanged();
        };
        break;
      case SessionMenuItem::Rename:
        action.id = "session_rename";
        action.label = Tr("sidebar.menu.rename");
        action.run = [this, thread_id]() { RenameAiThread(thread_id); };
        break;
      case SessionMenuItem::Mute:
        action.id = "session_mute";
        action.label = Tr("sidebar.menu.mute");
        action.run = [this, thread_id, position]() { OpenMute(thread_id, position); };
        break;
      case SessionMenuItem::Unmute:
        action.id = "session_unmute";
        action.label = Tr("sidebar.menu.unmute");
        action.run = [this, thread_id]() {
          presenter_.Unmute(thread_id);
          host_.AfterThreadPrefsChanged();
        };
        break;
      case SessionMenuItem::MuteMentions:
      case SessionMenuItem::AllowMentions:
        action.id = item == SessionMenuItem::MuteMentions ? "session_mute_mentions" : "session_allow_mentions";
        action.label =
            Tr(item == SessionMenuItem::MuteMentions ? "sidebar.menu.mute_mentions" : "sidebar.menu.allow_mentions");
        action.run = [this, thread_id, mute = item == SessionMenuItem::MuteMentions]() {
          presenter_.SetMuteMentions(thread_id, mute);
          host_.AfterThreadPrefsChanged();
        };
        break;
      case SessionMenuItem::AskBeforeSharingAi:
        action.id = "session_ask_before_sharing_ai";
        action.label = Tr("sidebar.menu.ask_before_sharing_ai");
        action.run = [this, thread_id]() {
          presenter_.AskBeforeSharingAi(thread_id);
          host_.AfterThreadPrefsChanged();
        };
        break;
      case SessionMenuItem::Archive:
      case SessionMenuItem::Unarchive:
        action.id = item == SessionMenuItem::Archive ? "session_archive" : "session_unarchive";
        action.label = Tr(item == SessionMenuItem::Archive ? "sidebar.menu.archive" : "sidebar.menu.unarchive");
        action.run = [this, thread_id, archive = item == SessionMenuItem::Archive]() {
          presenter_.SetArchived(thread_id, archive);
          host_.AfterThreadPrefsChanged();
        };
        break;
      case SessionMenuItem::ViewContact:
        action.id = "session_view_contact";
        action.label = Tr("chat.menu.view_contact");
        action.run = [this, id = *contact_id]() {
          if (contacts_notify_.select_contact) {
            contacts_notify_.select_contact(id);
          }
        };
        break;
      case SessionMenuItem::Block:
      case SessionMenuItem::Unblock: {
        const bool block = item == SessionMenuItem::Block;
        action.id = block ? "session_block" : "session_unblock";
        action.label = Tr(block ? "chat.menu.block" : "chat.menu.unblock");
        action.run = [this, id = contact_id, peer_id = thread.peer_identity_value,
                      name = facade->ResolveThreadLabel(thread).title,
                      block]() { RunBlockFlow(host_, presenter_, contacts_notify_, id, peer_id, name, block); };
        break;
      }
      case SessionMenuItem::ClearChat:
        action.id = "session_clear_chat";
        action.label = Tr("chat.menu.clear_history");
        action.run = [this, thread_id]() { host_.ClearHistoryOf(thread_id); };
        break;
      case SessionMenuItem::Delete:
        action.id = "session_delete";
        action.label = Tr("sidebar.menu.delete");
        // The close flow asks before it deletes.
        action.run = [this, thread_id]() { host_.OnCloseThread(thread_id); };
        break;
      case SessionMenuItem::LeaveGroup:
        action.id = "session_leave_group";
        action.label = Tr("chat.group.leave_title");
        // The close flow asks by itself and, for an owner, hands the group over first.
        action.run = [this, thread_id]() { host_.OnCloseThread(thread_id); };
        break;
    }
    actions.push_back(std::move(action));
  }
  return actions;
}

void ChatMenus::OpenMute(const std::string& thread_id, const ui::Vector2i position, const bool offer_unmute) {
  static constexpr std::pair<SessionMuteChoice, const char*> kChoices[] = {
      {SessionMuteChoice::EightHours, "sidebar.menu.mute_8h"},
      {SessionMuteChoice::OneWeek, "sidebar.menu.mute_1w"},
      {SessionMuteChoice::Always, "sidebar.menu.mute_always"},
  };
  std::vector<ContextMenuAction> actions;
  for (const auto& [choice, label_key] : kChoices) {
    actions.push_back({
        std::string("session_") + label_key,
        Tr(label_key),
        nullptr,
        [this, thread_id, choice = choice]() {
          presenter_.Mute(thread_id, choice);
          host_.AfterThreadPrefsChanged();
        },
        "../icons/v2/bell-off.svg",
    });
  }
  if (offer_unmute) {
    actions.push_back({
        "session_unmute",
        Tr("group_info.unmute"),
        nullptr,
        [this, thread_id]() {
          presenter_.Unmute(thread_id);
          host_.AfterThreadPrefsChanged();
        },
        "../icons/v2/bell.svg",
    });
  }
  ContextMenuHost::Instance().ShowActions(position, std::move(actions), {}, Tr("group_info.mute"));
}

void ChatMenus::OpenNewSession(ui::Event& ev) {
  const ui::Vector2i position = MenuPositionBelowEvent(ev);

  std::vector<ContextMenuAction> actions;
  actions.push_back({
      "chat_with_ai",
      Tr("chat.menu.chat_with_ai"),
      nullptr,
      [this]() { host_.OnNewChat(); },
      "../icons/v2/sparkle.svg",
  });
  actions.push_back({
      "message_contact",
      Tr("chat.menu.message_a_contact"),
      nullptr,
      [this]() { host_.OnNewMessage(); },
      "../icons/v2/compose.svg",
  });
  actions.push_back({
      "find_someone",
      Tr("chat.menu.find_someone"),
      nullptr,
      [this]() { host_.OnFindSomeone(); },
      "../icons/v2/search.svg",
  });
  ContextMenuHost::Instance().ShowActions(position, std::move(actions), {}, Tr("chat.new_chat"));
}

void ChatMenus::OpenThreadActions(ui::Event& ev) {
  // Anchor near the right edge of the trigger so the menu stays in the header corner.
  const ui::Vector2i position = MenuPositionBelowRightAlignedEvent(ev);

  std::vector<ContextMenuAction> actions;
  // Not while a voice message is being recorded (its bar holds the composer's place).
  if (chat_.show_search && !chat_.voice_recording) {
    actions.push_back({
        "search",
        Tr("chat.search.menu"),
        nullptr,
        [this]() { host_.OnOpenSearch(); },
        "../icons/search.svg",
    });
  }
  ConversationsFacade* facade = host_.Facade();
  if (chat_.thread_is_public && host_.MessagingIsReady() && facade) {
    auto can_lock = facade->CanLockPublicToThisDevice(host_.ActiveThreadId());
    if (can_lock && *can_lock) {
      actions.push_back({
          "lock_public_device",
          Tr("chat.device_lock.menu"),
          nullptr,
          [this]() { host_.OnLockPublicToThisDevice(); },
          "../icons/lock.svg",
      });
    }
  }
  if (chat_.show_sync_with_peer && !chat_.sync_in_progress) {
    actions.push_back({
        "sync_with_peer",
        Tr("chat.menu.sync_with_peer"),
        nullptr,
        [this]() { host_.OnSyncWithPeer(); },
        "../icons/sync.svg",
    });
  }
  // Phones, chats with people (design §8.15): the ask-AI panel over this chat. An AI chat is the AI already.
  if (host_.AskAiPanelAvailable() && !chat_.thread_is_ai) {
    actions.push_back({
        "ask_ai",
        Tr("ask_ai.title"),
        nullptr,
        [this]() { host_.OpenAskAiPanel(); },
        "../icons/v2/sparkle-fill.svg",
    });
  }
  if (chat_.show_thread_actions) {
    actions.push_back({
        "clear_history",
        Tr("chat.menu.clear_history"),
        nullptr,
        [this]() { host_.ClearHistoryOf(host_.ActiveThreadId()); },
        "../icons/trash.svg",
        true,
    });
  }
  if (chat_.show_forget_memory) {
    actions.push_back({
        "forget_memory",
        Tr("chat.menu.forget_ai_memory"),
        nullptr,
        [this]() { host_.OnForgetMemory(); },
        "../icons/sparkle.svg",
        true,
    });
  }
  if (actions.empty()) {
    return;
  }
  ContextMenuHost::Instance().ShowActions(position, std::move(actions), {}, chat_.thread_title.c_str());
}

void ChatMenus::CallBack(const std::string& video_arg) {
  // A tapped call record (#164): as the call button, the presenter joins a call that goes on or drops to voice.
  if (call_actions_.start_call) {
    (void)call_actions_.start_call(host_.ActiveThreadId(), video_arg == "1");
  }
}

void ChatMenus::StartCall(ui::Event& ev) {
  const std::string thread_id = host_.ActiveThreadId();
  if (thread_id.empty() || !call_actions_.start_call) {
    return;
  }
  auto start_call = call_actions_.start_call;
  const bool video_available = !call_actions_.video_call_available || call_actions_.video_call_available();
  if (chat_.call_joinable || !video_available) {
    (void)start_call(thread_id, false); // nothing to choose: a Join, or voice only
    return;
  }

  const ui::Vector2i position = MenuPositionBelowRightAlignedEvent(ev);
  std::vector<ContextMenuAction> actions;
  actions.push_back({
      "call_voice",
      Tr("call.start.voice"),
      nullptr,
      [thread_id, start_call]() { (void)start_call(thread_id, false); },
      "../icons/phone.svg",
  });
  actions.push_back({
      "call_video",
      Tr("call.start.video"),
      nullptr,
      [thread_id, start_call]() { (void)start_call(thread_id, true); },
      "../icons/video.svg",
  });
  ContextMenuHost::Instance().ShowActions(position, std::move(actions), {}, chat_.thread_title.c_str());
}

void ChatMenus::OpenEmojiInsert(ui::Event* ev) {
  if (chat_.compose_disabled) {
    return;
  }
  if (emoji_picker_notify_.open_insert) {
    emoji_picker_notify_.open_insert();
    return;
  }
  // Fallback when picker is not wired (tests / headless): keep preset strip.
  const ui::Vector2i position = ev ? MenuPositionBelowEvent(*ev) : ui::Vector2i(120, 120);
  std::vector<ContextMenuAction> actions;
  for (const std::string& emoji : DefaultQuickReactions()) {
    actions.push_back({
        "insert_" + emoji,
        emoji,
        nullptr,
        [this, emoji]() { host_.InsertEmojiIntoDraft(emoji); },
    });
  }
  ContextMenuHost::Instance().ShowActions(position, std::move(actions));
}

void ChatMenus::OpenReactionMore(const std::string& message_id) {
  if (emoji_picker_notify_.open_react) {
    emoji_picker_notify_.open_react(message_id);
    return;
  }
  if (!shell_feedback_.show_prompt) {
    return;
  }
  shell_feedback_.show_prompt(Tr("chat.react.title"), Tr("chat.react.prompt"), "",
                              [this, message_id](bool confirmed, std::string value) {
                                if (!confirmed) {
                                  return;
                                }
                                host_.ToggleReaction(message_id, value);
                              });
}

void ChatMenus::OpenShareTargets(const std::string& text, const ui::Vector2i position) {
  ConversationsFacade* facade = host_.Facade();
  if (!facade || text.empty()) {
    return;
  }
  // One friend per share, no caption: the direct chats, most recent first.
  constexpr size_t kMaxShareTargets = 30;
  std::vector<Thread> targets;
  if (auto threads = facade->ListThreads()) {
    for (const Thread& thread : *threads) {
      if (thread.kind == ThreadKind::Direct && !(thread.local_title.empty() && thread.title.empty())) {
        targets.push_back(thread);
      }
    }
  }
  std::sort(targets.begin(), targets.end(),
            [](const Thread& a, const Thread& b) { return a.updated_at > b.updated_at; });
  if (targets.size() > kMaxShareTargets) {
    targets.resize(kMaxShareTargets);
  }
  if (targets.empty()) {
    host_.ShowToast(Tr("chat.share.no_friends"));
    return;
  }
  std::vector<ContextMenuAction> actions;
  for (const Thread& thread : targets) {
    const std::string name = thread.local_title.empty() ? thread.title : thread.local_title;
    actions.push_back({"share_to_" + thread.id, name, nullptr, [this, thread_id = thread.id, name, text]() {
                         ConversationsFacade* facade = host_.Facade();
                         if (!facade) {
                           return;
                         }
                         SendRelayOptions opts;
                         opts.update_preview = true;
                         if (auto sent = facade->SendUserMessage(thread_id, text, opts); !sent) {
                           host_.ShowToast(sent.error().message);
                           return;
                         }
                         host_.ShowToast(Tr("chat.share.sent", {{"name", name}}));
                       }});
  }
  ContextMenuHost::Instance().ShowActions(position, std::move(actions), {}, Tr("chat.menu.share"));
}

void ChatMenus::OpenDeleteChoice(const std::string& message_id, const ui::Vector2i position) {
  if (!host_.Facade()) {
    return;
  }
  if (!presenter_.CanDeleteForEveryone(message_id)) {
    host_.ShowConfirm(
        Tr("chat.menu.delete_title"), Tr("chat.menu.delete_confirm"),
        [this, message_id](const bool ok) {
          if (ok) {
            host_.DeleteMessageForMe(message_id);
          }
        },
        true);
    return;
  }
  std::vector<ContextMenuAction> actions;
  ContextMenuAction everyone{"delete_for_everyone", Tr("chat.menu.delete_for_everyone"), nullptr,
                             [this, message_id]() { host_.DeleteMessageForEveryone(message_id); }};
  everyone.danger = true;
  everyone.icon = "../icons/v2/trash.svg";
  actions.push_back(std::move(everyone));
  ContextMenuAction mine{"delete_for_me", Tr("chat.menu.delete_for_me"), nullptr,
                         [this, message_id]() { host_.DeleteMessageForMe(message_id); }};
  mine.danger = true;
  mine.icon = "../icons/v2/trash.svg";
  actions.push_back(std::move(mine));
  actions.push_back({"delete_cancel", Tr("common.cancel"), nullptr, []() {}, "../icons/v2/close.svg"});
  actions.back().cancel = true; // the phone sheet's close disc stands for it
  // Opened after this menu is gone: a menu cannot replace itself from inside its own click.
  AppRuntime::PostUI([position, actions = std::move(actions)]() mutable {
    ContextMenuHost::Instance().ShowActions(position, std::move(actions), {}, Tr("chat.menu.delete_title"));
  });
}

} // namespace pbr
