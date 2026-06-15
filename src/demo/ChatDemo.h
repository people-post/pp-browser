#pragma once

#include "agent/AgentSession.h"
#include "app/Config.h"
#include "common/Module.h"

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

  struct SessionRow {
    Rml::String title;
    Rml::String preview;
  };

  struct TranscriptDisplayRow {
    Rml::String user_content_rml;
    Rml::String assistant_content_rml;
    bool has_assistant = false;
  };

  bool Setup(Rml::Context* context, const AppConfig& config);
  void Update();
  void Shutdown();

private:
  struct ActiveForm {
    std::string entry_id;
    std::string form_id;
  };

  struct ChatState {
    Rml::String draft;
    Rml::String status;
    std::vector<TranscriptDisplayRow> turns;
    bool loading = false;
  };

  struct ShellState {
    std::vector<SessionRow> sessions;
    Rml::String preview_rml;
  };

  struct PendingReply {
    std::string entry_id;
    std::string output;
    bool from_llm = false;
    bool append_mock_form = false;
  };

  ChatDemo();

  static void SendMessageCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void SendSuggestionCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void SendChatActionCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void SubmitFormCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void NewChatCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);

  void OnSendMessage();
  void OnNewChat();
  void SendUserText(const std::string& text, std::optional<std::string> user_payload = std::nullopt);
  void SendChatAction(const std::string& entry_id, int action_index);
  void SubmitForm(const std::string& entry_id, const std::string& form_id);
  void SyncDisplayFromConversation();
  void UpdateSidebarPreview(const std::string& preview_text);
  void FinishAssistantReply(const std::string& entry_id, const std::string& raw_output, bool from_llm,
                            const std::string& finish_reason = {}, bool append_mock_form = false);
  void HandleAgentEvent(const AgentEvent& event);

  std::string HydrateAssistantRml(const TranscriptEntry& entry) const;
  bool IsFormEditable(const std::string& entry_id, const std::string& form_id) const;
  void SnapshotActiveFormDraft();
  void RestoreActiveFormDraft();
  void UpdateActiveFormFromRml(const std::string& entry_id, const std::string& rml);
  void ClearFormState();

  Rml::Context* context_ = nullptr;
  ChatState chat_;
  ShellState shell_;
  std::optional<AgentSession> agent_;
  bool use_llm_ = false;
  std::optional<PendingReply> pending_reply_;
  std::optional<ActiveForm> active_form_;
  std::set<std::pair<std::string, std::string>> submitted_forms_;
  std::map<std::string, std::string> active_form_draft_;
  bool restore_form_on_next_update_ = false;
};

bool SetupChatDemo(Rml::Context* context, const AppConfig& config);
void UpdateChatDemo();
void ShutdownChatDemo();

} // namespace pbr
