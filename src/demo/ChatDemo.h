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

  bool Setup(Rml::Context* context, const AppConfig& config);
  void Update();
  void Shutdown();

private:
  struct ChatSuggestion {
    Rml::String label;
    Rml::String message;
  };

  struct ChatMessage {
    Rml::String role;
    Rml::String content_rml;
    std::vector<ChatSuggestion> suggestions;
  };

  struct ChatState {
    Rml::String draft;
    Rml::String status;
    std::vector<ChatMessage> messages;
    bool loading = false;
  };

  struct ShellState {
    std::vector<SessionRow> sessions;
    Rml::String preview_rml;
  };

  struct PendingReply {
    std::string output;
    bool from_llm = false;
  };

  ChatDemo();

  static void SendMessageCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void SendSuggestionCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);

  void OnSendMessage();
  void SendUserText(const std::string& text);
  void FinishAssistantReply(const std::string& raw_output, bool from_llm);
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
