#pragma once

#include "agent/AgentSession.h"
#include "agent/StructuredTextParser.h"
#include "agent/TurnPlan.h"
#include "app/Bootstrap.h"
#include "common/Module.h"
#include "demo/ChatWidgetTypes.h"
#include "ui/WorkingSetTypes.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Types.h>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Rml {
class Context;
}

namespace pbr {

class ChatDemo : public Module {
public:
  static ChatDemo& Instance();

  using SessionRow = SessionDisplayRow;

  bool Setup(Rml::Context* context, const BootstrapResult& bootstrap);
  void Update();
  void Shutdown();
  void ApplyConfig(const AppConfig& config);

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

  ChatDemo();

  static void SendMessageCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void SendSuggestionCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void SendChatActionCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void SubmitFormCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void CalendarPrevCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void CalendarNextCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void SelectCalendarDayCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void NewChatCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void SelectThreadCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void CloseThreadCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OpenWorkingSetCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);

  void OnSendMessage();
  void OnNewChat();
  void OnSelectThread(const std::string& thread_id);
  void OnCloseThread(const std::string& thread_id);
  void SendUserText(const std::string& text, std::optional<std::string> user_payload = std::nullopt);
  void SendChatAction(const std::string& entry_id, int action_index);
  void SubmitForm(const std::string& entry_id, const std::string& form_id);
  void CalendarPrev(const std::string& entry_id);
  void CalendarNext(const std::string& entry_id);
  void SelectCalendarDay(const std::string& entry_id, const std::string& iso_date);
  void SyncDisplayFromThread();
  void SyncShellSessions();
  void UpdateThreadChrome();
  void UpdateSidebarPreview(const std::string& preview_text);
  void FinishAssistantReply(const std::string& entry_id, const std::string& raw_output, bool from_llm,
                            const std::string& finish_reason = {}, const std::string& thread_id = {},
                            ResponseGoal response_goal = ResponseGoal::General,
                            RenderMode render_mode = RenderMode::Blocks);
  void HandleAgentEvent(const AgentEvent& event);
  void HandleLocalAction(const std::string& message, const std::optional<std::string>& payload);
  void RefreshFromMessaging();

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

  Rml::Context* context_ = nullptr;
  ChatState chat_;
  ShellState shell_;
  BootstrapResult bootstrap_{};
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
};

bool SetupChatDemo(Rml::Context* context, const BootstrapResult& bootstrap);
void UpdateChatDemo();
void ShutdownChatDemo();

} // namespace pbr
