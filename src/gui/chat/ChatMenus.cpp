#include "gui/chat/ChatMenus.h"

#include "common/Utilities.h"
#include "common/thread/ThreadTypes.h"
#include "feature/conversations/SessionMenu.h"
#include "foundation/i18n/LocalizationService.h"
#include "foundation/runtime/AppRuntime.h"
#include "gui/UserFeedback.h"
#include "lib/messaging/SendRelayOptions.h"

#include <ui/base/SystemInterface.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/SelectionController.h>

#include <algorithm>
#include <optional>
#include <utility>

namespace pbr {

namespace {

const char* kReactionPresets[] = {"👍", "❤️", "😂", "😮", "😢", "🙏"};

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

ChatMenus::ChatMenus(ChatMenusHost& host, ChatPresenter& presenter, const ChatScreenState& chat,
                     WorkingSetController& working_set, const ContactsNotifyPorts& contacts_notify,
                     const EmojiPickerNotifyPorts& emoji_picker_notify, const ShellFeedbackPorts& shell_feedback,
                     const CallActionsPorts& call_actions)
    : host_(host), presenter_(presenter), chat_(chat), working_set_(working_set), contacts_notify_(contacts_notify),
      emoji_picker_notify_(emoji_picker_notify), shell_feedback_(shell_feedback), call_actions_(call_actions) {}

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
    actions.push_back({"ask_ai_item", Tr("chat.menu.ask_ai"), nullptr, [this, shared, prefix]() {
                         AppRuntime::PostUI([this, shared, prefix]() {
                           working_set_.Clear();
                           host_.StartQuoteReply(shared, prefix);
                         });
                       }});
    actions.push_back({"copy_item", Tr("common.copy"), nullptr, [shared]() { CopyToClipboard(shared); }});
    const ui::Vector2i position = request.position;
    // Opened after this menu is gone: a menu cannot replace itself from inside its own click.
    actions.push_back({"share_item", Tr("chat.menu.share"), nullptr, [this, shared, position]() {
                         AppRuntime::PostUI([this, shared, position]() { OpenShareTargets(shared, position); });
                       }});
    return actions;
  }

  const std::string message_id = FindAttributeUp(request.target, "message-id");
  if (message_id.empty() || host_.IsDeletedMessage(message_id)) {
    return actions; // a deleted message takes no reaction, reply, copy or second delete
  }
  // Reactions sit in the row above the message, the rest in the list below it.
  for (const char* emoji : kReactionPresets) {
    ContextMenuAction react{std::string("react_") + emoji, emoji, nullptr,
                            [this, message_id, emoji]() { host_.ToggleReaction(message_id, emoji); }};
    react.quick = true;
    actions.push_back(std::move(react));
  }
  ContextMenuAction more{"react_more", "+", nullptr, [this, message_id]() { OpenReactionMore(message_id); }};
  more.quick = true;
  actions.push_back(std::move(more));

  // A contact card: its person can be added (the card is checked first).
  if (host_.IsContactCard(message_id)) {
    actions.push_back({"add_contact_from_card", Tr("chat.menu.add_to_contacts"), nullptr,
                       [this, message_id]() { host_.AddContactFromCard(message_id); }, "../icons/contacts.svg"});
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
    actions.push_back(
        {"reply_message", Tr("chat.menu.reply"), nullptr, [this, text]() { host_.StartQuoteReply(text); }});
    actions.push_back({"copy_message", Tr("common.copy"), nullptr, [text]() { CopyToClipboard(text); }});
    // Ask AI: the message goes into the quote bar and "@ai " into the input; the user adds the question.
    if (auto active = facade->GetActiveThread(); active && active->kind == ThreadKind::Direct) {
      actions.push_back({"ask_ai_message", Tr("chat.menu.ask_ai"), nullptr,
                         [this, text]() { host_.StartQuoteReply(text, "@ai "); }});
    }
  }
  ContextMenuAction remove{"delete_message", Tr("common.delete"), nullptr,
                           [this, message_id, position = request.position]() {
                             OpenDeleteChoice(message_id, position);
                           }};
  remove.danger = true;
  actions.push_back(std::move(remove));
  return actions;
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
  if (contact_id) {
    if (auto contact = facade->GetContact(*contact_id); contact && *contact) {
      state.blocked = (*contact)->local.trust == TrustLevel::Blocked;
    }
  }
  const ThreadLocalPrefs prefs = presenter_.PrefsOf(thread_id).value_or(ThreadLocalPrefs{});
  state.unread = SessionIsUnread(thread.unread_count, prefs.marked_unread);
  state.pinned = prefs.pinned_at != 0;
  state.muted = ThreadIsMuted(prefs.muted_until, util::NowUnixMs());
  state.archived = prefs.archived;

  for (const SessionMenuItem item : SessionMenuItems(state)) {
    ContextMenuAction action;
    action.danger = SessionMenuItemIsDestructive(item);
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
      action.icon = "../icons/pin.svg";
      action.run = [this, thread_id, pin = item == SessionMenuItem::Pin]() {
        presenter_.SetPinned(thread_id, pin);
        host_.AfterThreadPrefsChanged();
      };
      break;
    case SessionMenuItem::Mute:
      action.id = "session_mute";
      action.label = Tr("sidebar.menu.mute");
      action.icon = "../icons/bell-off.svg";
      action.run = [this, thread_id, position]() { OpenMute(thread_id, position); };
      break;
    case SessionMenuItem::Unmute:
      action.id = "session_unmute";
      action.label = Tr("sidebar.menu.unmute");
      action.icon = "../icons/bell-off.svg";
      action.run = [this, thread_id]() {
        presenter_.Unmute(thread_id);
        host_.AfterThreadPrefsChanged();
      };
      break;
    case SessionMenuItem::Archive:
    case SessionMenuItem::Unarchive:
      action.id = item == SessionMenuItem::Archive ? "session_archive" : "session_unarchive";
      action.label = Tr(item == SessionMenuItem::Archive ? "sidebar.menu.archive" : "sidebar.menu.unarchive");
      action.icon = "../icons/archive.svg";
      action.run = [this, thread_id, archive = item == SessionMenuItem::Archive]() {
        presenter_.SetArchived(thread_id, archive);
        host_.AfterThreadPrefsChanged();
      };
      break;
    case SessionMenuItem::ViewContact:
      action.id = "session_view_contact";
      action.label = Tr("chat.menu.view_contact");
      action.icon = "../icons/contacts.svg";
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
      action.run = [this, id = *contact_id, block]() {
        const auto apply = [this, id, block]() {
          ConversationsFacade* facade = host_.Facade();
          if (!facade) {
            return;
          }
          if (auto set = facade->SetContactTrust(id, block ? TrustLevel::Blocked : TrustLevel::Unknown); !set) {
            UserFeedback::Fail(Tr("contacts.err.trust"));
            return;
          }
          if (contacts_notify_.refresh) {
            contacts_notify_.refresh();
          }
          host_.ShowToast(Tr(block ? "chat.menu.blocked_toast" : "chat.menu.unblocked_toast"));
          host_.NotifySurfaceChanged();
        };
        if (!block) {
          apply();
          return;
        }
        host_.ShowConfirm(Tr("chat.menu.block_confirm_title"), Tr("chat.menu.block_confirm_body"),
                          [apply](const bool ok) {
                            if (ok) {
                              apply();
                            }
                          });
      };
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
      action.icon = "../icons/trash.svg";
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

void ChatMenus::OpenMute(const std::string& thread_id, const ui::Vector2i position) {
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
    });
  }
  ContextMenuHost::Instance().ShowActions(position, std::move(actions));
}

void ChatMenus::OpenNewSession(ui::Event& ev) {
  const ui::Vector2i position = MenuPositionBelowEvent(ev);

  std::vector<ContextMenuAction> actions;
  actions.push_back({
      "chat_with_ai",
      Tr("chat.menu.chat_with_ai"),
      nullptr,
      [this]() { host_.OnNewChat(); },
      "../icons/sparkle.svg",
  });
  actions.push_back({
      "message_contact",
      Tr("chat.menu.message_a_contact"),
      nullptr,
      [this]() { host_.OnNewMessage(); },
      "../icons/contacts.svg",
  });
  actions.push_back({
      "find_someone",
      Tr("chat.menu.find_someone"),
      nullptr,
      [this]() { host_.OnFindSomeone(); },
      "../icons/message.svg",
  });
  ContextMenuHost::Instance().ShowActions(position, std::move(actions));
}

void ChatMenus::OpenThreadActions(ui::Event& ev) {
  // Anchor near the right edge of the trigger so the menu stays in the header corner.
  const ui::Vector2i position = MenuPositionBelowRightAlignedEvent(ev);

  std::vector<ContextMenuAction> actions;
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
  ContextMenuHost::Instance().ShowActions(position, std::move(actions));
}

void ChatMenus::StartCall(ui::Event& ev) {
  const std::string thread_id = host_.ActiveThreadId();
  if (thread_id.empty() || !call_actions_.start_call) {
    return;
  }
  auto start_call = call_actions_.start_call;
  if (chat_.call_joinable) {
    (void)start_call(thread_id, false);  // Join: into the call that goes on, no voice / video choice
    return;
  }
  const bool video_available = !call_actions_.video_call_available || call_actions_.video_call_available();
  if (!video_available) {
    (void)start_call(thread_id, false);
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
  ContextMenuHost::Instance().ShowActions(position, std::move(actions));
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
  for (const char* emoji : kReactionPresets) {
    actions.push_back({
        std::string("insert_") + emoji,
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
  std::sort(targets.begin(), targets.end(), [](const Thread& a, const Thread& b) { return a.updated_at > b.updated_at; });
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
  ContextMenuHost::Instance().ShowActions(position, std::move(actions));
}

void ChatMenus::OpenDeleteChoice(const std::string& message_id, const ui::Vector2i position) {
  if (!host_.Facade()) {
    return;
  }
  if (!presenter_.CanDeleteForEveryone(message_id)) {
    host_.ShowConfirm(Tr("chat.menu.delete_title"), Tr("chat.menu.delete_confirm"), [this, message_id](const bool ok) {
      if (ok) {
        host_.DeleteMessageForMe(message_id);
      }
    });
    return;
  }
  std::vector<ContextMenuAction> actions;
  ContextMenuAction everyone{"delete_for_everyone", Tr("chat.menu.delete_for_everyone"), nullptr,
                             [this, message_id]() { host_.DeleteMessageForEveryone(message_id); }};
  everyone.danger = true;
  actions.push_back(std::move(everyone));
  ContextMenuAction mine{"delete_for_me", Tr("chat.menu.delete_for_me"), nullptr,
                         [this, message_id]() { host_.DeleteMessageForMe(message_id); }};
  mine.danger = true;
  actions.push_back(std::move(mine));
  actions.push_back({"delete_cancel", Tr("common.cancel"), nullptr, []() {}});
  // Opened after this menu is gone: a menu cannot replace itself from inside its own click.
  AppRuntime::PostUI([position, actions = std::move(actions)]() mutable {
    ContextMenuHost::Instance().ShowActions(position, std::move(actions));
  });
}

} // namespace pbr
