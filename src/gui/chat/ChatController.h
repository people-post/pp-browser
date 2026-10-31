#pragma once

#include "feature/ai/AgentUiPorts.h"
#include "gui/contacts/ContactsNotifyPorts.h"
#include "gui/contacts/PeoplePickerNotifyPorts.h"
#include "gui/EmojiPickerNotifyPorts.h"
#include "feature/conversations/ChatPresenter.h"
#include "feature/conversations/ConversationsFacade.h"
#include "gui/chat/AgentTurnController.h"
#include "gui/chat/OutgoingAttachmentPrep.h"
#include "gui/chat/VoiceComposer.h"
#include "gui/chat/VoicePlayback.h"
#include "gui/chat/ChatMentionPicker.h"
#include "gui/chat/ChatMenus.h"
#include "gui/chat/ChatPeerSheet.h"
#include "gui/chat/ChatScreenState.h"
#include "gui/chat/ChatThreadChrome.h"
#include "gui/chat/ChatTranscriptScroller.h"
#include "gui/chat/ChatWidgetHost.h"
#include "gui/chat/HeldAgentSend.h"
#include "gui/chat/ThreadDrafts.h"
#include "gui/chat/WorkingSetController.h"
#include "feature/conversations/MessagingUiPorts.h"
#include "gui/BadgeNotifyPorts.h"
#include "gui/CallActionsPorts.h"
#include "gui/ChatSurfaceNotifyPorts.h"
#include "gui/shell/ShellFeedbackPorts.h"
#include "gui/shell/ShellNavigationPorts.h"
#include "gui/shell/ShellSetupPorts.h"
#include "gui/UnlockEnsurePorts.h"
#include "lib/messaging/AtAiParser.h"
#include "lib/ai/StructuredTextParser.h"
#include "lib/ai/TurnPlan.h"
#include "foundation/data/Config.h"
#include "foundation/data/SessionStore.h"
#include "common/Module.h"
#include "lib/ui/ChatWidgetTypes.h"
#include "lib/ui/ContextMenuHost.h"

#include <source_location>
#include <ui/data/DataModelHandle.h>
#include <ui/dom/Event.h>
#include <ui/base/Input.h>
#include <ui/base/Types.h>

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <functional>
#include <utility>
#include <vector>
#include "common/PbrCompat.h"

namespace ui {
class Context;
class Element;
}

namespace pbr {

class InputCoordinator;

class ChatController : public Module, private AgentTurnHost, private ChatMenusHost {
public:
  using AgentConfig = AgentTurnController::AgentConfig;

  static AgentConfig ProjectAgent(const AppConfig& config) { return AgentTurnController::ProjectAgent(config); }
  void Apply(const AgentConfig& config) { agent_.ApplyConfig(config); }
  void BindAgentPorts(AgentUiPorts ports) { agent_.BindPorts(std::move(ports)); }
  void ReloadAgentConfig() { agent_.ReloadAgentConfig(); }

  ChatController();
  ~ChatController() override = default;

  using SessionRow = SessionDisplayRow;

  bool Setup(ui::Context* context);
  /** Non-owning; pass nullptr to clear. Rebinds sub-presenters (scroller/chrome). */
  void BindConversationsFacade(ConversationsFacade* facade);
  /** App-wired hook so messaging tool registration stays in Application (no ui→hub registration edge). */
  void BindRegisterMessagingTools(std::function<void(ToolRegistry&)> hook);
  void BindContactsNotify(ContactsNotifyPorts ports);
  void BindPeoplePickerNotify(PeoplePickerNotifyPorts ports);
  void BindEmojiPickerNotify(EmojiPickerNotifyPorts ports);
  /** Append a normalized emoji glyph to the composer draft (used by emoji picker).
   *  @param restore_composer_focus When true (popover), focus the composer after insert.
   *         When false (keyboard panel), advance caret only so the OSK stays dismissed. */
  void InsertEmojiIntoDraft(const std::string& emoji, bool restore_composer_focus = true) override;
  /** Toggle a reaction from the emoji picker (public for Application wiring). */
  void ReactWithEmoji(const std::string& message_id, const std::string& emoji);
  void BindShellSetup(ShellSetupPorts ports);
  void BindSessionStore(SessionStore& store);
  void BindBadgeNotify(BadgeNotifyPorts ports);
  void BindInputCoordinator(InputCoordinator& input);
  void BindCallActions(CallActionsPorts ports);
  /** Refresh peer-link / "In call" header (Application wires from CallController). */
  void UpdatePeerLinkChrome();
  void BindUnlockEnsure(UnlockEnsurePorts ports);
  void BindShellNavigation(ShellNavigationPorts ports);
  void BindShellFeedback(ShellFeedbackPorts ports);
  /** Push surface snapshot to app ChatShellBridge. Clear via BindSurfaceNotify({}). */
  void BindSurfaceNotify(ChatSurfaceNotifyPorts ports);
  void BindMessagingUi(MessagingUiPorts ports);
  SessionStore& Store() override;
  const SessionStore& Store() const;
  void Update();
  /** Call after ui::Context::Update so follow-tail uses fresh layout heights. */
  void AfterLayout();
  void Shutdown();
  void OnApplicationPause();
  void FinalizeThreadDisplay();
  /** Put `text` in the open chat's composer and focus it; the user sends it (Settings "?" → PP Support). */
  void PrefillDraft(const std::string& text);
  void OnHomeTabActivated();
  void OnSessionsTabActivated();
  void OnFindSomeone() override;
  void OnSelectThread(const std::string& thread_id);
  /** Rebind messaging ports after a full profile data wipe/reinit. */
  void OnProfileDataReset();
  /** Called when messaging becomes ready (app-wired). */
  void OnMessagingReady();
  void OnMeshReady();
  /** App-wired from ShellHost layout sync (see Application::WireShellPresentationEvents). */
  void OnShellLayoutSynced();
  /** Escape after the shell closed nothing (desktop). Not in the composer: NotInComposer. */
  enum class ComposerEscape { NotInComposer, Handled, Idle };
  ComposerEscape OnComposerEscape();
  /** The composer shows the unsent text of the conversation now open (Home has its own). App-wired to every
   * nav-tab change, and called after every display sync. */
  void SyncDraftOwner();

private:
  using ChatState = ChatScreenState;

  using ShellState = ChatShellState;

  struct PendingReply {
    std::string entry_id;
    std::string thread_id;
    std::string output;
    bool from_llm = false;
  };


  /** Events both data models ("chat" and "shell") bind: the agent's widgets, chat actions, "new message". */
  void BindSharedEvents(ui::DataModelConstructor& ctor);
  void OnSendMessage();
  /** A Home suggestion that runs a function (`id`): its localized sentence is sent with the function's payload. */
  void SendSuggestionAction(const std::string& id);
  void OnNewChat() override;
  void OnNewMessage() override;
  void OnCloseThread(const std::string& thread_id) override;
  void OnClearHistory();
  /** Asks, then deletes the thread's messages on this device (the thread stays in the list). */
  void ClearHistoryOf(const std::string& thread_id) override;
  void AfterHistoryCleared(const std::string& thread_id);
  /** After a chat's settings changed (the presenter refreshed its list): badges and the surface follow. */
  void AfterThreadPrefsChanged() override;
  /** G016: asks once, then the owner dissolves the group for everyone (the chat stays, read-only). */
  void DissolveGroupAfterConfirm(const std::string& group_id);
  /** Run a close (`action`) and, when it went through, clear what the open chat held; tells the user
   *  when it failed (`show_error_text`: the action's own message, else "could not delete"). */
  void CloseChatWith(const std::string& thread_id, const std::function<Roe<void>()>& action,
                     bool show_error_text = false);
  /** Copy the presenter's chats list into the shell's data model. */
  void ApplySessionList();
  void OnToggleArchived();
  void OnForgetMemory() override;
  void OnSyncWithPeer() override;
  void OnRetryGapSync();
  void OnStartNewSecureChat();
  void OnPauseIntegrityOnly();
  void OnCopyPskKey();
  void OnTogglePskImport();
  void OnImportPsk();
  void OnVerifyPsk();
  void OnRotatePskExport();
  void OnLockPublicToThisDevice() override;
  /** Opens the AI thread a send from Home goes to: `chip_message`'s own earlier thread when there is one, else a new thread. */
  bool EnsureHomeOutboundSession(const std::string& chip_message = {}) override;
  /** `from_home_chip`: a Home chip (not typed text) is being sent, so it continues that chip's own thread. */
  void SendUserText(const std::string& text, std::optional<std::string> user_payload = std::nullopt,
                    bool from_home_chip = false) override;
  void ToggleReaction(const std::string& message_id, const std::string& emoji) override;
  void OnAttachFile(ui::Event* ev = nullptr);
  /** `remove_after_read`: the file is a temporary one of ours (a voice recording): deleted once its bytes are read. */
  void StartAttachmentUpload(const std::string& path, OutgoingPickSource source, bool remove_after_read = false);
  /** Retry / discard the failed send the chip shows; the staged ciphertext is what a retry sends again. */
  void OnRetryAttachmentSend();
  void OnDiscardAttachmentSend();
  /** Show the chip for a staged send of the open thread that did not finish (also after the app was killed). */
  void RefreshPendingAttachmentChip(bool force);
  void ShowFailedAttachment(const std::string& record_id, const std::string& name);
  /** Progress for the chip: the percent line, with a "please stay in PP" hint for a large file. */
  std::function<bool(uint64_t done, uint64_t total)> MakeSendProgress();
  /** The composer sends to an AI thread: an open AI thread, or Home (its first send opens one). */
  bool InAiComposerContext() const override;
  /** Phones: "Photo / File / Cancel" before a picker (both composers). */
  void ShowAttachChoice(std::function<void()> photo, std::function<void()> file) override;
  /** A thread's transcript window drawn as bound rows, AI question images decorated. */
  std::vector<MessageDisplayRow> BuildMessageRows(const std::string& thread_id, std::optional<int64_t> oldest_inclusive,
                                                  std::optional<int64_t> newest_inclusive) const;
  void OpenAttachment(const std::string& message_id);
  void DownloadAttachment(const std::string& message_id);
  void RetryAttachmentDownload(const std::string& message_id);
  void SyncComposerInputState() override;
  void DeleteMessageForMe(const std::string& message_id) override;
  void DeleteMessageForEveryone(const std::string& message_id) override;
  bool IsDeletedMessage(const std::string& message_id) const override;
  /** What the user reads in a message of the active thread ("" when not found). */
  std::string MessagePlainText(const std::string& message_id) const override;
  bool IsContactCard(const std::string& message_id) const override;
  void AddContactFromCard(const std::string& message_id) override;
  /**
   * Reply / Ask AI: shows `text` as the quote bar above the composer; the next send carries it as a quote.
   * `draft_prefix` ("@ai " for Ask AI) is put into the input with the caret after it.
   */
  void StartQuoteReply(const std::string& text, const std::string& draft_prefix = {}) override;
  void CancelQuoteReply();
  /** A tap on the quote of reply `reply_message_id`: scrolls to the message it quotes, if it is on screen. */
  void JumpToQuotedMessage(const std::string& reply_message_id);
  /** A tap on link `link_index` of a plain-text message: confirms the host, then opens it. */
  void OpenMessageLink(const std::string& message_id, int link_index);
  void SyncDisplayFromThread() override;
  /** Last time a display sync ran the whole-thread attachment backfill. */
  std::chrono::steady_clock::time_point last_attachment_backfill_{};
  std::chrono::steady_clock::time_point last_call_joinable_poll_{};
  /** Each second: whether the open group chat has a call this device may join (calls change it). */
  void RefreshCallJoinable(std::chrono::steady_clock::time_point now);
  void OpenChatLink(const std::string& entry_id, int link_index);
  /** Opens an https URL after the user confirms its host; the dialog offers to stop asking until the app restarts. */
  void ConfirmAndOpenUrl(const std::string& url);
  void RestoreWorkingSetsFromActiveThread();
  void SyncShellSessions() override;
  void UpdateThreadChrome();
  void ResetChatPanelState();
  std::string failed_record_id_;
  /** Where the attach menu ends: just above the attach button (B90), refreshed on each tap. */
  int attach_menu_x_ = 12;
  int attach_menu_y_ = 120;
  /** Attachments being opened right now (an Open decrypts on a worker; a second tap waits for the first). */
  std::unordered_set<std::string> opening_message_ids_;
  std::string pending_chip_thread_;
  void UpdateSidebarPreview(const std::string& preview_text) override;
  void HandleLocalAction(const std::string& message, const std::optional<std::string>& payload,
                         const std::string& entry_id) override;
  QueuedSendOutcome RunQueuedAgentSend(const QueuedAgentSend& item) override;
  void PutBackDraft(const std::string& thread_id, const std::string& text) override;
  /** Text that did not go out goes back to `key`'s composer (a thread id, or ThreadDrafts::kHome). */
  void PutTextBack(const std::string& key, const std::string& text, bool prepend);
  /** The draft key of what the composer shows now: Home, or the open thread ("" = none shown). */
  std::string CurrentDraftKey() const;
  /** An AI send while another thread's turn runs: confirm "@ai+" sharing first, then queue it (B112). */
  void QueueAgentSend(const std::string& thread_id, const std::string& text, std::optional<std::string> payload,
                      bool thread_created_for_this, bool already_confirmed = false);
  /** The "share with the AI?" dialog for an "@ai+" / "@ai++" send; "don't ask again" is remembered. */
  void ConfirmShareWithAi(const std::string& thread_id, const std::string& text, std::function<void(bool ok)> then);
  /** The dialog itself (also the router's confirm hook): the one copy of its wording. */
  void ShowShareWithAiDialog(const std::string& thread_id, AtAiMode mode,
                             std::function<void(bool ok, bool dont_ask)> done);
  /** A send that did not go out: its text back in the composer, if that thread is open and the composer empty. */
  void RestoreDraftIfEmpty(const std::string& thread_id, const std::string& text);
  /** The open thread's queued send is cancelled: its text goes back to the composer. */
  void CancelQueuedHere();
  /** Same decision SendUserText makes, before the composer is cleared: false = keep the draft. */
  bool ComposerSendAllowed(const std::string& text);
  /** The composer's send of a group text with mentions (`reply`: the typed text; `out`: with any quote). */
  void SendWithMentions(const std::string& thread_id, const std::string& reply, const std::string& out,
                        std::vector<TextSpan> spans);
  /** Enter / Tab choose the `@` picker's first line while it is open over the composer. */
  void RegisterMentionPickerKeys();
  void RefreshFromMessaging();
  void OnLoadOlderHistory();
  void OnRetryPeerDial();
  void OnMessagesScroll();
  void OnJumpToLatest();
  void WireMessagingBindings();
  void WithSecrets(std::function<void()> action);

  bool MessagingInitialized() const override;
  bool MessagingReady() const override;
  const std::string& ActiveThreadId() const override;

  ShellChromeSnapshot ChromeSnapshot() const override;
  ChatSurfaceSnapshot BuildSurfaceSnapshot() const;
  void NotifySurfaceChanged() override;
  /** The call site is passed on as the request's reason (ShellHost::RequestSyncLayout). */
  void ShellSyncLayout(bool restore_focus_after = false, std::source_location where = std::source_location::current());
  void ShellSelectNavTab(NavTab tab);
  void ShellSetPrimaryPane(const std::string& key);
  void ShellOpenCompactChat();
  void ShellCloseCompactChat();
  void ShellSetActivity(bool visible, const ui::String& message = {}) override;
  void ShellRemountNavRail();
  void ShowToast(const std::string& message, ToastDuration duration = ToastDuration::Short) override;
  void ShowConfirm(const std::string& title, const std::string& message, std::function<void(bool)> on_result) override;
  void ShowConfirmWithCheckbox(const std::string& title, const std::string& message, const std::string& checkbox_label,
                               bool checkbox_default, std::function<void(bool, bool)> on_result);
  void ShowPrompt(const std::string& title, const std::string& message, const std::string& default_value,
                  std::function<void(bool, std::string)> on_result) override;



  ui::Context* context_ = nullptr;
  ConversationsFacade* facade_ = nullptr;
  /** The chats list's decisions and buttons (pp-app-probe drives the same presenter). */
  ChatPresenter presenter_;
  /** When each muted group last alerted on a mention (G015: at most once a minute). */
  std::unordered_map<std::string, int64_t> muted_mention_alert_ms_;
  std::function<void(ToolRegistry&)> register_messaging_tools_;
  ContactsNotifyPorts contacts_notify_;
  PeoplePickerNotifyPorts people_picker_notify_;
  EmojiPickerNotifyPorts emoji_picker_notify_;
  ShellSetupPorts shell_setup_;
  SessionStore* session_store_ = nullptr;
  BadgeNotifyPorts badge_notify_;
  InputCoordinator* input_ = nullptr;
  CallActionsPorts call_actions_;
  UnlockEnsurePorts unlock_ensure_;
  ShellNavigationPorts shell_navigation_;
  ShellFeedbackPorts shell_feedback_;
  ChatSurfaceNotifyPorts surface_notify_;
  MessagingUiPorts messaging_ui_;
  ChatState chat_;
  ShellState shell_;
  bool messaging_ready_ = false;
  /** Its AppLifecycle background listener (removed at Shutdown). */
  uint64_t lifecycle_background_id_ = 0;
  bool mesh_ready_ = false;
  ChatTranscriptScroller scroller_;
  WorkingSetController working_set_;
  ChatThreadChrome chrome_;
  ChatWidgetHost widgets_;
  std::optional<PendingReply> pending_reply_;


  /** entry id -> https links its rendered bubble refers to (open_chat_link index). */
  std::map<std::string, std::vector<std::string>> chat_links_;
  /** The agent's side of the screen (turns, streaming, held sends, AI images, its widgets). */
  AgentTurnController agent_{*this, chat_, chat_links_, widgets_, working_set_, shell_feedback_};
  /** The screen's menus (long press / right-click, header buttons, pickers' fallbacks). */
  ChatMenus menus_{*this, presenter_, chat_, working_set_, contacts_notify_, emoji_picker_notify_, shell_feedback_,
                   call_actions_};
  ChatPeerSheet peer_sheet_{*this, presenter_, chat_, contacts_notify_, people_picker_notify_};
  /** The composer's `@` picker in a group chat, and the mentions it recorded per chat (G015). */
  ChatMentionPicker mentions_{*this, presenter_, chat_, context_};
  /** The composer's microphone, and the voice bubbles' player. Made in the constructor (they need `this`). */
  std::unique_ptr<VoiceComposer> voice_;
  std::unique_ptr<VoicePlayback> voice_playback_;
  /** The chat the voice parts last saw: another one cancels a recording and stops a clip. */
  std::string voice_thread_;
  // AgentTurnHost, ChatMenusHost
  ConversationsFacade* Facade() const override { return facade_; }
  bool MessagingIsReady() const override { return messaging_ready_; }
  void RegisterMessagingTools(ToolRegistry& tools) override {
    if (register_messaging_tools_) {
      register_messaging_tools_(tools);
    }
  }
  bool skip_link_confirm_this_run_ = false; // set from the confirm dialog's checkbox; never persisted
  bool local_action_in_flight_ = false; // a row action (Add / Message) is waiting on its directory lookup
  bool focus_draft_after_sync_ = false;
  std::optional<int64_t> last_escape_ms_; // a second Escape within kDoubleEscapeWindowMs stops the turn
  ThreadDrafts drafts_;                   // unsent text per conversation; the composer widget is shared
  std::map<std::string, std::string> quotes_; // a quote being replied to, per conversation, while another is shown
  void StashComposerExtras(const std::string& key);
  void RestoreComposerExtras(const std::string& key);
  void ForgetComposerExtras(const std::string& key);
};

} // namespace pbr
