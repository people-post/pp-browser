#pragma once

#include "feature/ai/AgentUiPorts.h"
#include "gui/contacts/ContactsNotifyPorts.h"
#include "gui/contacts/PeoplePickerNotifyPorts.h"
#include "gui/EmojiPickerNotifyPorts.h"
#include "feature/conversations/ChatPresenter.h"
#include "feature/conversations/ConversationsFacade.h"
#include "gui/chat/AgentTurnController.h"
#include "gui/chat/ChatMenus.h"
#include "gui/chat/ChatScreenState.h"
#include "gui/chat/ChatThreadChrome.h"
#include "gui/chat/ChatTranscriptScroller.h"
#include "gui/chat/ChatWidgetHost.h"
#include "gui/chat/HeldAgentSend.h"
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

  /** App-owned instance; set via InstallInstance from Application. Static callbacks use Instance(). */
  static void InstallInstance(ChatController& controller);
  static void ClearInstance();
  static ChatController& Instance();

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

private:
  using ChatState = ChatScreenState;

  using ShellState = ChatShellState;

  struct PendingReply {
    std::string entry_id;
    std::string thread_id;
    std::string output;
    bool from_llm = false;
  };

  static void SendMessageCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void SendSuggestionCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void SendSuggestionActionCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void SendChatActionCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OpenChatLinkCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void StopTurnCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void ToggleReactionCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OpenEmojiInsertCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void AttachFileCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void RemoveImageCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OpenAttachmentCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void DownloadAttachmentCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void RetryAttachmentCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void SubmitFormCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void CalendarPrevCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void CalendarNextCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void SelectCalendarDayCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void NewChatCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void NewMessageCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OpenNewSessionMenuCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OpenThreadActionsMenuCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void StartCallCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OpenPeerSheetCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void SelectThreadCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void ClearHistoryCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void ForgetMemoryCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void SyncWithPeerCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void RetryGapSyncCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void StartNewSecureChatCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void PauseIntegrityCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void CopyPskKeyCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void TogglePskImportCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void ImportPskCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void VerifyPskCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void RotatePskExportCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OpenWorkingSetCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void LoadOlderHistoryCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void RetryPeerDialCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void MessagesScrollCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void JumpToLatestCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);

  void OnSendMessage();
  void OnNewChat() override;
  void OnNewMessage() override;
  void OnOpenPeerSheet(ui::Event& ev);
  void OnCloseThread(const std::string& thread_id) override;
  void OnClearHistory();
  /** Asks, then deletes the thread's messages on this device (the thread stays in the list). */
  void ClearHistoryOf(const std::string& thread_id) override;
  void AfterHistoryCleared(const std::string& thread_id);
  /** After a chat's settings changed (the presenter refreshed its list): badges and the surface follow. */
  void AfterThreadPrefsChanged() override;
  /** Run a close (`action`) and, when it went through, clear what the open chat held; tells the user
   *  when it failed (`show_error_text`: the action's own message, else "could not delete"). */
  void CloseChatWith(const std::string& thread_id, const std::function<Roe<void>()>& action,
                     bool show_error_text = false);
  /** Copy the presenter's chats list into the shell's data model. */
  void ApplySessionList();
  void OnToggleArchived();
  static void ToggleArchivedCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
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
  void OnAttachFile();
  void StartAttachmentUpload(const std::string& path);
  /** The composer sends to an AI thread: an open AI thread, or Home (its first send opens one). */
  bool InAiComposerContext() const override;
  /** Phones: "Photo / File / Cancel" before a picker (both composers). */
  void ShowAttachChoice(std::function<void()> photo, std::function<void()> file) override;
  void OnRemoveImage();
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
  /**
   * Reply / Ask AI: shows `text` as the quote bar above the composer; the next send carries it as a quote.
   * `draft_prefix` ("@ai " for Ask AI) is put into the input with the caret after it.
   */
  void StartQuoteReply(const std::string& text, const std::string& draft_prefix = {}) override;
  void CancelQuoteReply();
  /** A tap on the quote of reply `reply_message_id`: scrolls to the message it quotes, if it is on screen. */
  void JumpToQuotedMessage(const std::string& reply_message_id);
  static void JumpToQuoteCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  /** A tap on link `link_index` of a plain-text message: confirms the host, then opens it. */
  void OpenMessageLink(const std::string& message_id, int link_index);
  static void OpenMessageLinkCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void CancelQuoteReplyCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
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
  void UpdateSidebarPreview(const std::string& preview_text) override;
  void HandleLocalAction(const std::string& message, const std::optional<std::string>& payload) override;
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
                  std::function<void(bool, std::string)> on_result);



  ui::Context* context_ = nullptr;
  ConversationsFacade* facade_ = nullptr;
  /** The chats list's decisions and buttons (pp-app-probe drives the same presenter). */
  ChatPresenter presenter_;
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
  // AgentTurnHost, ChatMenusHost
  ConversationsFacade* Facade() const override { return facade_; }
  bool MessagingIsReady() const override { return messaging_ready_; }
  void RegisterMessagingTools(ToolRegistry& tools) override {
    if (register_messaging_tools_) {
      register_messaging_tools_(tools);
    }
  }
  bool skip_link_confirm_this_run_ = false; // set from the confirm dialog's checkbox; never persisted
  bool focus_draft_after_sync_ = false;

  static ChatController* installed_instance_;
};

} // namespace pbr
