#pragma once

#include "feature/presenters/ChatPresenter.h"
#include "feature/conversations/ConversationsFacade.h"
#include "gui/CallActionsPorts.h"
#include "gui/EmojiPickerNotifyPorts.h"
#include "gui/chat/ChatScreenState.h"
#include "gui/chat/WorkingSetController.h"
#include "gui/contacts/ContactsNotifyPorts.h"
#include "gui/shell/SessionSwipeModel.h"
#include "gui/shell/ShellFeedbackPorts.h"
#include "lib/ui/ContextMenuHost.h"
#include "lib/ui/ShellTypes.h"

#include <ui/base/Types.h>
#include <ui/dom/Event.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** What ChatMenus and ChatPeerSheet need from the chat screen they are part of (ChatController). UI thread. */
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
  /** `destructive`: the confirm button deletes / removes / blocks something (danger colour on the phone). */
  virtual void ShowConfirm(const std::string& title, const std::string& message, std::function<void(bool)> on_result,
                           bool destructive = false) = 0;
  virtual void ShowPrompt(const std::string& title, const std::string& message, const std::string& default_value,
                          std::function<void(bool, std::string)> on_result) = 0;

  // What the menus' items do.
  virtual void OnNewChat() = 0;
  virtual void OnNewMessage() = 0;
  virtual void OnFindSomeone() = 0;
  virtual void OnCloseThread(const std::string& thread_id) = 0;
  virtual void ClearHistoryOf(const std::string& thread_id) = 0;
  virtual void OnLockPublicToThisDevice() = 0;
  virtual void OnSyncWithPeer() = 0;
  virtual void OnForgetMemory() = 0;
  /** "Search": search inside the open chat. */
  virtual void OnOpenSearch() = 0;
  /** G017, the owner: "Who can add people and rename" — Everyone / Admins only, as a menu at `position`. */
  virtual void ChooseGroupPreset(const std::string& group_id, GroupPreset current, ui::Vector2i position) = 0;
  virtual void ToggleReaction(const std::string& message_id, const std::string& emoji) = 0;
  /** The emoji the user used last, most recent first (the profile's list; reactions and the emoji picker share it). */
  virtual std::vector<std::string> RecentEmojis() const = 0;
  /** Puts `emoji` first in that list. */
  virtual void RememberEmoji(const std::string& emoji) = 0;
  virtual void InsertEmojiIntoDraft(const std::string& emoji, bool restore_composer_focus = true) = 0;
  virtual void StartQuoteReply(const std::string& text, const std::string& draft_prefix = {}) = 0;
  virtual void DeleteMessageForMe(const std::string& message_id) = 0;
  virtual void DeleteMessageForEveryone(const std::string& message_id) = 0;
  virtual bool IsDeletedMessage(const std::string& message_id) const = 0;
  /** What the user reads in a message of the active thread ("" when not found). */
  virtual std::string MessagePlainText(const std::string& message_id) const = 0;
  /** A contact card (someone's contact, shared in the chat). */
  virtual bool IsContactCard(const std::string& message_id) const = 0;
  /** *Add to contacts* on a contact card: checks it, adds the person, shows them. */
  virtual void AddContactFromCard(const std::string& message_id) = 0;
  /** The ask-AI panel exists here (phones): the chat's "⋯" offers "Ask AI". */
  virtual bool AskAiPanelAvailable() const = 0;
  /** Opens the ask-AI panel over the chat; nothing of the chat goes to the AI. */
  virtual void OpenAskAiPanel() = 0;
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
  /** The phone sheet's title for a long press: the chat's name on a chat row, "Message" on a message, else none. */
  std::string MenuTitleFor(const ContextMenuRequest& request);
  /** The chat's name as its row shows it. */
  std::string SessionTitle(const std::string& thread_id);
  /** A row in the chats list; `position` anchors a second step (mute durations). */
  std::vector<ContextMenuAction> SessionActions(const std::string& thread_id, ui::Vector2i position);
  /** A swipe button on a chats-list row: runs that row's matching long-press item (nothing if the menu lacks it). */
  void RunSessionSwipeAction(const std::string& thread_id, SessionSwipeAction action);
  /** Asks for a new name for an AI conversation and stores it as its local title (empty = the automatic title). */
  void RenameAiThread(const std::string& thread_id);

  /** "+": chat with AI / message a contact / find someone. */
  void OpenNewSession(ui::Event& ev);
  /** The open chat's "⋯": search, lock to this device, sync, clear history, forget AI memory. */
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

  /**
   * Second step of "Mute": 8 hours / 1 week / Always; `offer_unmute` (the group info sheet, already muted) adds
   * Unmute. On the phone an action sheet.
   */
  void OpenMute(const std::string& thread_id, ui::Vector2i position, bool offer_unmute = false);

private:

  ChatMenusHost& host_;
  ChatPresenter& presenter_;
  const ChatScreenState& chat_;
  WorkingSetController& working_set_;
  const ContactsNotifyPorts& contacts_notify_;
  const EmojiPickerNotifyPorts& emoji_picker_notify_;
  const ShellFeedbackPorts& shell_feedback_;
  const CallActionsPorts& call_actions_;
};

/**
 * Block (asks first) or unblock someone from a chat menu; no contact id means a stranger, who is saved as
 * a blocked contact. The pill names them. Shared by the session menu and the peer sheet.
 */
void RunBlockFlow(ChatMenusHost& host, ChatPresenter& presenter, const ContactsNotifyPorts& contacts_notify,
                  const std::optional<std::string>& contact_id, const std::string& peer_id, const std::string& name,
                  bool block);

} // namespace pbr
