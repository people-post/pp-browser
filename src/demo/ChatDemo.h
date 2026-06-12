#pragma once

#include "agent/AgentSession.h"
#include "app/Config.h"
#include "common/Module.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Types.h>

#include <optional>
#include <string>
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

  struct ChatSuggestion {
    Rml::String label;
    Rml::String message;
  };

  struct TranscriptDisplayRow {
    Rml::String user_content_rml;
    Rml::String assistant_content_rml;
    bool has_assistant = false;
    std::vector<ChatSuggestion> suggestions;
  };

  bool Setup(Rml::Context* context, const AppConfig& config);
  void Update();
  void Shutdown();

private:
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
  };

  ChatDemo();

  static void SendMessageCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void SendSuggestionCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void NewChatCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);

  void OnSendMessage();
  void OnNewChat();
  void SendUserText(const std::string& text);
  void SyncDisplayFromConversation();
  void UpdateSidebarPreview(const std::string& preview_text);
  void FinishAssistantReply(const std::string& entry_id, const std::string& raw_output, bool from_llm);
  void HandleAgentEvent(const AgentEvent& event);

  ChatState chat_;
  ShellState shell_;
  std::optional<AgentSession> agent_;
  bool use_llm_ = false;
  std::optional<PendingReply> pending_reply_;
};

bool SetupChatDemo(Rml::Context* context, const AppConfig& config);
void UpdateChatDemo();
void ShutdownChatDemo();

} // namespace pbr
