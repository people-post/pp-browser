#pragma once

#include "feature/ai/AgentSession.h"
#include "base/messaging/AtAiParser.h"
#include "base/ai/StructuredTextParser.h"
#include "base/ai/TurnPlan.h"
#include "base/data/Config.h"
#include "common/Module.h"
#include "base/ui/ChatWidgetTypes.h"
#include "base/ui/WorkingSetTypes.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Types.h>

#include <chrono>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <functional>
#include <utility>
#include <vector>

namespace Rml {
class Context;
}

namespace pbr {

class ChatController : public Module {
public:
  static ChatController& Instance();

  using SessionRow = SessionDisplayRow;

  bool Setup(Rml::Context* context);
  void Update();
  void Shutdown();
  void OnApplicationPause();
  void FinalizeThreadDisplay();
  void OnHomeTabActivated();
  void OnSessionsTabActivated();
  void OnFindSomeone();
  void OnSelectThread(const std::string& thread_id);
  /** Rebind to MessagingHub after a full profile data wipe/reinit. */
  void OnProfileDataReset();
  /** Re-read agent LLM config (e.g. after Brief API key register/rotate). */
  void ReloadAgentConfig();

private:
  struct ActiveForm {
    std::string entry_id;
    std::string form_id;
  };

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

  ChatController();

  static void SendMessageCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void SendSuggestionCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void SendChatActionCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void SubmitFormCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void CalendarPrevCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void CalendarNextCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void SelectCalendarDayCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void NewChatCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void NewMessageCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OpenNewSessionMenuCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OpenThreadActionsMenuCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
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

  void OnSendMessage();
  void OnNewChat();
  void OnNewMessage();
  void OnOpenNewSessionMenu(Rml::Event& ev);
  void OnOpenThreadActionsMenu(Rml::Event& ev);
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
  void SendUserText(const std::string& text, std::optional<std::string> user_payload = std::nullopt);
  void SendChatAction(const std::string& entry_id, int action_index);
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
  void OnShellLayoutSynced();
  void OnLoadOlderHistory();
  void OnRetryPeerDial();
  void UpdatePeerLinkChrome();
  void SendSharedAssistantRelay(const std::string& thread_id, AtAiMode mode, const std::string& plain_text);
  void WireMessagingBindings();
  void WithSecrets(std::function<void()> action);

  std::string HydrateAssistantRml(const TranscriptEntry& entry) const;
  bool IsFormEditable(const std::string& entry_id, const std::string& form_id) const;
  void ExpireFormsExcept(const std::string& entry_id, const std::string& form_id);
  void InitializeWidgetState(const std::string& entry_id, const std::vector<WidgetInit>& inits);
  void MergeWidgetStateIntoRow(const std::string& entry_id, TranscriptDisplayRow& row) const;
  TurnWidgetState* FindWidgetState(const std::string& entry_id);
  const TurnWidgetState* FindWidgetState(const std::string& entry_id) const;
  void ClearFormState();
  void ClearWorkingSet();
  void ApplyWorkingSetFromParse(const std::string& entry_id, const std::vector<WorkingSetCandidate>& candidates);
  void OpenWorkingSet(const std::string& entry_id, int block_index);
  void SyncWorkingSetWidgetBindings(const std::string& entry_id);
  void DirtyWorkingSet();
  std::vector<WorkingSetCandidate> HydrateWorkingSetCandidates(const std::vector<WorkingSetCandidate>& candidates,
                                                                const std::string& entry_id) const;
  bool ShouldCloseWorkingSetForAction(const std::optional<std::string>& payload) const;

  void ApplyRuntimeConfig(const AppConfig& config);

  Rml::Context* context_ = nullptr;
  ChatState chat_;
  ShellState shell_;
  std::optional<AgentSession> agent_;
  bool use_llm_ = false;
  bool messaging_ready_ = false;
  std::optional<PendingReply> pending_reply_;
  std::optional<ActiveForm> active_form_;
  std::set<std::pair<std::string, std::string>> submitted_forms_;
  std::map<std::string, TurnWidgetState> widgets_by_entry_;
  std::map<std::string, std::vector<WorkingSetCandidate>> working_set_by_entry_;
  WorkingSetAffinity active_working_set_affinity_ = WorkingSetAffinity::None;
  std::string active_working_set_entry_id_;
  bool focus_draft_after_sync_ = false;
  std::chrono::steady_clock::time_point last_peer_link_poll_{};
};

bool SetupChatController(Rml::Context* context);
void UpdateChatController();
void ShutdownChatController();

} // namespace pbr
