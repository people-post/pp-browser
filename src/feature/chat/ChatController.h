#pragma once

#include "feature/messaging/AgentUiPorts.h"
#include "feature/ui/ContactsNotifyPorts.h"
#include "feature/ui/PeoplePickerNotifyPorts.h"
#include "feature/ui/EmojiPickerNotifyPorts.h"
#include "feature/messaging/MessagingFacade.h"
#include "feature/chat/ChatThreadChrome.h"
#include "feature/chat/ChatTranscriptScroller.h"
#include "feature/chat/ChatWidgetHost.h"
#include "feature/chat/WorkingSetController.h"
#include "feature/messaging/MessagingUiPorts.h"
#include "feature/ui/BadgeNotifyPorts.h"
#include "feature/ui/CallActionsPorts.h"
#include "feature/ui/ChatSurfaceNotifyPorts.h"
#include "feature/ui/ShellFeedbackPorts.h"
#include "feature/ui/ShellNavigationPorts.h"
#include "feature/ui/ShellSetupPorts.h"
#include "feature/ui/UnlockEnsurePorts.h"
#include "base/messaging/AtAiParser.h"
#include "base/ai/StructuredTextParser.h"
#include "base/ai/TurnPlan.h"
#include "base/data/Config.h"
#include "base/data/SessionStore.h"
#include "common/Module.h"
#include "base/ui/ChatWidgetTypes.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Types.h>

#include <optional>
#include <string>
#include <functional>
#include <utility>
#include <vector>

namespace Rml {
class Context;
class Element;
}

namespace pbr {

class InputCoordinator;

class ChatController : public Module {
public:
  /** Hot-reloadable assistant slice projected from AppConfig (LLM / MCP / search). */
  struct AgentConfig {
    LlmConfig llm;
    std::string llm_api_key_env;
    McpConfig promoted_mcp;
    std::vector<McpConfig> mcp_servers;
    SearchConfig search;
    ContextBudget context = DefaultContextBudget();

    bool operator==(const AgentConfig& other) const {
      if (llm.base_url != other.llm.base_url || llm.model != other.llm.model ||
          llm.api_key != other.llm.api_key || llm.preset != other.llm.preset ||
          llm.require_api_key != other.llm.require_api_key ||
          llm_api_key_env != other.llm_api_key_env || promoted_mcp.url != other.promoted_mcp.url ||
          search.provider != other.search.provider || mcp_servers.size() != other.mcp_servers.size()) {
        return false;
      }
      for (size_t i = 0; i < mcp_servers.size(); ++i) {
        const McpConfig& a = mcp_servers[i];
        const McpConfig& b = other.mcp_servers[i];
        if (a.id != b.id || a.url != b.url || a.command != b.command || a.enabled != b.enabled ||
            a.args != b.args) {
          return false;
        }
      }
      return true;
    }
    bool operator!=(const AgentConfig& other) const { return !(*this == other); }
  };

  static AgentConfig ProjectAgent(const AppConfig& config);
  void Apply(const AgentConfig& config);

  ChatController();
  ~ChatController() override = default;

  /** App-owned instance; set via InstallInstance from Application. Static callbacks use Instance(). */
  static void InstallInstance(ChatController& controller);
  static void ClearInstance();
  static ChatController& Instance();

  using SessionRow = SessionDisplayRow;

  bool Setup(Rml::Context* context);
  /** Non-owning; pass nullptr to clear. Rebinds sub-presenters (scroller/chrome). */
  void BindMessagingFacade(MessagingFacade* facade);
  /** App-wired hook so messaging tool registration stays in Application (no feature/chat→hub edge). */
  void BindRegisterMessagingTools(std::function<void(ToolRegistry&)> hook);
  void BindAgentPorts(AgentUiPorts ports);
  void BindContactsNotify(ContactsNotifyPorts ports);
  void BindPeoplePickerNotify(PeoplePickerNotifyPorts ports);
  void BindEmojiPickerNotify(EmojiPickerNotifyPorts ports);
  /** Append a normalized emoji glyph to the composer draft (used by emoji picker).
   *  @param restore_composer_focus When true (popover), focus the composer after insert.
   *         When false (keyboard panel), advance caret only so the OSK stays dismissed. */
  void InsertEmojiIntoDraft(const std::string& emoji, bool restore_composer_focus = true);
  /** Toggle a reaction from the emoji picker (public for Application wiring). */
  void ReactWithEmoji(const std::string& message_id, const std::string& emoji);
  void BindShellSetup(ShellSetupPorts ports);
  void BindSessionStore(SessionStore& store);
  void BindBadgeNotify(BadgeNotifyPorts ports);
  void BindInputCoordinator(InputCoordinator& input);
  void BindCallActions(CallActionsPorts ports);
  void BindUnlockEnsure(UnlockEnsurePorts ports);
  void BindShellNavigation(ShellNavigationPorts ports);
  void BindShellFeedback(ShellFeedbackPorts ports);
  /** Push surface snapshot to app ChatShellBridge. Clear via BindSurfaceNotify({}). */
  void BindSurfaceNotify(ChatSurfaceNotifyPorts ports);
  void BindMessagingUi(MessagingUiPorts ports);
  SessionStore& Store();
  const SessionStore& Store() const;
  void Update();
  /** Call after Rml::Context::Update so follow-tail uses fresh layout heights. */
  void AfterLayout();
  void Shutdown();
  void OnApplicationPause();
  void FinalizeThreadDisplay();
  void OnHomeTabActivated();
  void OnSessionsTabActivated();
  void OnFindSomeone();
  void OnSelectThread(const std::string& thread_id);
  /** Rebind messaging ports after a full profile data wipe/reinit. */
  void OnProfileDataReset();
  /** Called when messaging becomes ready (app-wired). */
  void OnMessagingReady();
  /** Re-read agent LLM config (e.g. after Brief API key register/rotate). */
  void ReloadAgentConfig();
  /** App-wired from ShellHost layout sync (see Application::WireShellPresentationEvents). */
  void OnShellLayoutSynced();

private:
  struct ChatState {
    Rml::String draft;
    Rml::String draft_placeholder;
    Rml::String status;
    Rml::String thread_title;
    Rml::String thread_subtitle;
    Rml::String peer_link_status;
    Rml::String peer_link_banner;
    bool show_peer_link = false;
    bool show_peer_link_banner = false;
    bool show_retry_peer_dial = false;
    bool thread_encrypted = false;
    bool thread_is_ai = false;
    bool thread_is_private = false;
    bool thread_is_public = false;
    bool thread_is_group = false;
    bool compose_disabled = false;
    bool show_thread_actions = false;
    bool show_peer_sheet = false;
    bool show_call_actions = false;
    bool show_forget_memory = false;
    bool show_sync_with_peer = false;
    bool show_thread_menu = false;
    bool show_gap_banner = false;
    bool show_compromised_banner = false;
    bool show_psk_setup_banner = false;
    bool show_psk_import = false;
    bool psk_has_key = false;
    bool psk_verified = false;
    Rml::String psk_fingerprint;
    Rml::String psk_export_b64;
    Rml::String psk_import_text;
    bool sync_in_progress = false;
    bool show_older_history_hint = false;
    bool show_jump_to_latest = false;
    Rml::String jump_to_latest_label;
    std::vector<TranscriptDisplayRow> turns;
    std::vector<MessageDisplayRow> messages;
    bool use_messages_layout = true;
    bool loading = false;
    bool has_turns = false;
  };

  struct ShellState {
    std::vector<SessionRow> sessions;
    bool working_set_active = false;
    Rml::String working_set_title;
    Rml::String working_set_subtitle;
    Rml::String working_set_rml;
    TurnWidgetState working_set;
  };

  struct PendingReply {
    std::string entry_id;
    std::string thread_id;
    std::string output;
    bool from_llm = false;
  };

  static void SendMessageCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void SendSuggestionCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void SendChatActionCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void ToggleReactionCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OpenEmojiInsertCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void SubmitFormCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void CalendarPrevCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void CalendarNextCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void SelectCalendarDayCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void NewChatCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void NewMessageCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OpenNewSessionMenuCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OpenThreadActionsMenuCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void StartVoiceCallCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void StartVideoCallCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OpenPeerSheetCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void SelectThreadCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void CloseThreadCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void ClearHistoryCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void ForgetMemoryCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void SyncWithPeerCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void RetryGapSyncCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void StartNewSecureChatCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void PauseIntegrityCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void CopyPskKeyCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void TogglePskImportCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void ImportPskCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void VerifyPskCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void RotatePskExportCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OpenWorkingSetCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void LoadOlderHistoryCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void RetryPeerDialCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void MessagesScrollCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void JumpToLatestCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);

  void OnSendMessage();
  void OnNewChat();
  void OnNewMessage();
  void OnOpenNewSessionMenu(Rml::Event& ev);
  void OnOpenThreadActionsMenu(Rml::Event& ev);
  void OnOpenPeerSheet(Rml::Event& ev);
  void OnCloseThread(const std::string& thread_id);
  void OnClearHistory();
  void OnForgetMemory();
  void OnSyncWithPeer();
  void OnRetryGapSync();
  void OnStartNewSecureChat();
  void OnPauseIntegrityOnly();
  void OnCopyPskKey();
  void OnTogglePskImport();
  void OnImportPsk();
  void OnVerifyPsk();
  void OnRotatePskExport();
  /** From Home landing: mint AI thread, switch to Sessions, open chat. */
  bool EnsureHomeOutboundSession();
  void SendUserText(const std::string& text, std::optional<std::string> user_payload = std::nullopt);
  void SendChatAction(const std::string& entry_id, int action_index);
  void ToggleReaction(const std::string& message_id, const std::string& emoji);
  void OpenEmojiInsertMenu(Rml::Event* ev);
  void OpenReactPresetMenu(const std::string& message_id, Rml::Vector2i position);
  void ShowReactionMorePrompt(const std::string& message_id);
  void SubmitForm(const std::string& entry_id, const std::string& form_id);
  void CalendarPrev(const std::string& entry_id);
  void CalendarNext(const std::string& entry_id);
  void SelectCalendarDay(const std::string& entry_id, const std::string& iso_date);
  void SyncDisplayFromThread();
  void SyncShellSessions();
  void UpdateThreadChrome();
  void ResetChatPanelState();
  void UpdateSidebarPreview(const std::string& preview_text);
  void FinishAssistantReply(const std::string& entry_id, const std::string& raw_output, bool from_llm,
                            const std::string& finish_reason = {}, const std::string& thread_id = {},
                            ResponseGoal response_goal = ResponseGoal::General,
                            RenderMode render_mode = RenderMode::Blocks, AtAiMode shared_ai_mode = AtAiMode::None);
  void HandleAgentEvent(const AgentEvent& event);
  void HandleLocalAction(const std::string& message, const std::optional<std::string>& payload);
  void RefreshFromMessaging();
  void OnLoadOlderHistory();
  void OnRetryPeerDial();
  void OnMessagesScroll();
  void OnJumpToLatest();
  void UpdatePeerLinkChrome();
  void SendSharedAssistantRelay(const std::string& thread_id, AtAiMode mode, const std::string& plain_text);
  void WireMessagingBindings();
  /** Show/clear LLM setup banners once identity is readable (after unlock). */
  void RefreshLlmSetupBanner();
  void WithSecrets(std::function<void()> action);

  bool MessagingInitialized() const;
  bool MessagingReady() const;
  const std::string& ActiveThreadId() const;

  ShellChromeSnapshot ChromeSnapshot() const;
  ChatSurfaceSnapshot BuildSurfaceSnapshot() const;
  void NotifySurfaceChanged();
  void ShellSyncLayout(bool restore_focus_after = false);
  void ShellSelectNavTab(NavTab tab);
  void ShellSetPrimaryPane(const std::string& key);
  void ShellOpenCompactChat();
  void ShellCloseCompactChat();
  void ShellSetActivity(bool visible, const Rml::String& message = {});
  void ShellRemountNavRail();
  void ShowToast(const std::string& message, ToastDuration duration = ToastDuration::Short);
  void ShowConfirm(const std::string& title, const std::string& message, std::function<void(bool)> on_result);
  void ShowConfirmWithCheckbox(const std::string& title, const std::string& message, const std::string& checkbox_label,
                               bool checkbox_default, std::function<void(bool, bool)> on_result);
  void ShowPrompt(const std::string& title, const std::string& message, const std::string& default_value,
                  std::function<void(bool, std::string)> on_result);

  std::string HydrateAssistantRml(const TranscriptEntry& entry) const;
  bool IsFormEditable(const std::string& entry_id, const std::string& form_id) const;
  void InitializeWidgetState(const std::string& entry_id, const std::vector<WidgetInit>& inits);
  void MergeWidgetStateIntoRow(const std::string& entry_id, TranscriptDisplayRow& row) const;
  TurnWidgetState* FindWidgetState(const std::string& entry_id);
  const TurnWidgetState* FindWidgetState(const std::string& entry_id) const;
  void ClearFormState();

  bool AgentReady() const;
  bool AgentConfigured() const;

  Rml::Context* context_ = nullptr;
  MessagingFacade* facade_ = nullptr;
  std::function<void(ToolRegistry&)> register_messaging_tools_;
  AgentUiPorts agent_ports_;
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
  AgentConfig last_agent_runtime_;
  bool use_llm_ = false;
  bool messaging_ready_ = false;
  ChatTranscriptScroller scroller_;
  WorkingSetController working_set_;
  ChatThreadChrome chrome_;
  ChatWidgetHost widgets_;
  std::optional<PendingReply> pending_reply_;
  bool focus_draft_after_sync_ = false;

  static ChatController* installed_instance_;
};

} // namespace pbr
