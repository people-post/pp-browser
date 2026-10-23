#pragma once

#include "feature/conversations/ChatPresenter.h"
#include "feature/conversations/ConversationsFacade.h"
#include "gui/CallActionsPorts.h"
#include "gui/EmojiPickerNotifyPorts.h"
#include "gui/chat/ChatScreenState.h"
#include "gui/chat/WorkingSetController.h"
#include "gui/contacts/ContactsNotifyPorts.h"
#include "gui/shell/ShellFeedbackPorts.h"
#include "lib/ui/ContextMenuHost.h"
#include "lib/ui/ShellTypes.h"

#include <ui/base/Types.h>
#include <ui/dom/Event.h>

#include <functional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** What ChatMenus needs from the chat screen it is part of (ChatController). UI thread. */
class ChatMenusHost {
public:
  virtual ~ChatMenusHost() = default;
  virtual ConversationsFacade* Facade() const = 0;
  /** Messaging is wired to this screen (vault unlocked, bindings in place). */
  virtual bool MessagingIsReady() const = 0;
  virtual const std::string& ActiveThreadId() const = 0;
  /** After a chat's settings changed (the presenter refreshed its list): badges and the surface follow. */
  virtual void AfterThreadPrefsChanged() = 0;
  virtual void NotifySurfaceChanged() = 0;
  virtual void ShowToast(const std::string& message, ToastDuration duration = ToastDuration::Short) = 0;
  virtual void ShowConfirm(const std::string& title, const std::string& message,
                           std::function<void(bool)> on_result) = 0;

  // What the menus' items do.
  virtual void OnNewChat() = 0;
  virtual void OnNewMessage() = 0;
  virtual void OnFindSomeone() = 0;
  virtual void OnCloseThread(const std::string& thread_id) = 0;
  virtual void ClearHistoryOf(const std::string& thread_id) = 0;
  virtual void OnLockPublicToThisDevice() = 0;
  virtual void OnSyncWithPeer() = 0;
  virtual void OnForgetMemory() = 0;
  virtual void ToggleReaction(const std::string& message_id, const std::string& emoji) = 0;
  virtual void InsertEmojiIntoDraft(const std::string& emoji, bool restore_composer_focus = true) = 0;
  virtual void StartQuoteReply(const std::string& text, const std::string& draft_prefix = {}) = 0;
  virtual void DeleteMessageForMe(const std::string& message_id) = 0;
  virtual void DeleteMessageForEveryone(const std::string& message_id) = 0;
  virtual bool IsDeletedMessage(const std::string& message_id) const = 0;
  /** What the user reads in a message of the active thread ("" when not found). */
  virtual std::string MessagePlainText(const std::string& message_id) const = 0;
};

/**
 * The chat screen's menus: the long-press / right-click menu (a chats-list row, a message, an item
 * the user can pass on), the header's "new", thread-actions and call menus, the emoji and reaction
 * pickers' fallbacks, mute durations, share targets and the delete choice. Each item calls back into
 * the screen (ChatMenusHost) or the chats list's presenter.
 *
 * UI thread.
 */
class ChatMenus {
public:
  ChatMenus(ChatMenusHost& host, ChatPresenter& presenter, const ChatScreenState& chat,
            WorkingSetController& working_set, const ContactsNotifyPorts& contacts_notify,
            const EmojiPickerNotifyPorts& emoji_picker_notify, const ShellFeedbackPorts& shell_feedback,
            const CallActionsPorts& call_actions);

  /** ContextMenuHost's provider: what a long press / right-click on `request.target` offers. */
  std::vector<ContextMenuAction> ForRequest(const ContextMenuRequest& request);
  /** A row in the chats list; `position` anchors a second step (mute durations). */
  std::vector<ContextMenuAction> SessionActions(const std::string& thread_id, ui::Vector2i position);

  /** "+": chat with AI / message a contact / find someone. */
  void OpenNewSession(ui::Event& ev);
  /** The open chat's "⋯": lock to this device, sync, clear history, forget AI memory. */
  void OpenThreadActions(ui::Event& ev);
  /** Call button: joins a call that goes on, else voice / video (straight to voice without video). */
  void StartCall(ui::Event& ev);
  /** Composer emoji button: the picker, else (tests / headless) a strip of presets. */
  void OpenEmojiInsert(ui::Event* ev);
  /** Reaction "+": the picker, else a prompt for any emoji. */
  void OpenReactionMore(const std::string& message_id);
  /** Share: lists the direct chats (most recent first); picking one sends `text` there. */
  void OpenShareTargets(const std::string& text, ui::Vector2i position);
  /** Delete: our own message in a chat offers "for everyone" too; anything else asks, then deletes for me. */
  void OpenDeleteChoice(const std::string& message_id, ui::Vector2i position);

private:
  /** Second step of "Mute": 8 hours / 1 week / Always. */
  void OpenMute(const std::string& thread_id, ui::Vector2i position);

  ChatMenusHost& host_;
  ChatPresenter& presenter_;
  const ChatScreenState& chat_;
  WorkingSetController& working_set_;
  const ContactsNotifyPorts& contacts_notify_;
  const EmojiPickerNotifyPorts& emoji_picker_notify_;
  const ShellFeedbackPorts& shell_feedback_;
  const CallActionsPorts& call_actions_;
};

} // namespace pbr
