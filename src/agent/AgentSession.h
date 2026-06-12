#pragma once

#include "agent/LlmClient.h"
#include "agent/conversation/ConversationTypes.h"
#include "app/Config.h"

#include <memory>
#include <string>
#include <vector>

namespace pbr {

class Conversation;

enum class AgentEventType {
  LoadingChanged,
  ToolActivity,
  AssistantReady,
  Error,
};

struct AgentEvent {
  AgentEventType type = AgentEventType::LoadingChanged;
  bool loading = false;
  std::string tool_name;
  std::string status;
  std::string text;
  std::string message;
  std::string entry_id;
};

class AgentSession {
public:
  AgentSession();
  ~AgentSession();

  AgentSession(const AgentSession&) = delete;
  AgentSession& operator=(const AgentSession&) = delete;

  void Configure(const AppConfig& config);
  void Submit(const std::string& user_text);
  void PollEvents(std::vector<AgentEvent>& out);
  void Cancel();
  void StartNewConversation();

  bool IsConfigured() const;
  const Conversation& conversation() const;

  TranscriptEntry& AppendUserMessage(const std::string& user_text);
  bool CompleteAssistantMessage(const std::string& entry_id, const std::string& assistant_raw);
  bool SetAssistantDisplay(const std::string& entry_id, const std::string& assistant_rml,
                           std::vector<TranscriptSuggestion> suggestions);

private:
  struct Impl;

  static void ConfigureOnIO(const std::shared_ptr<Impl>& state);
  static void StartTurn(const std::shared_ptr<Impl>& state);
  static void RunProactiveSearchAndLlm(const std::shared_ptr<Impl>& state);
  static void RunLlmStep(const std::shared_ptr<Impl>& state);
  static void DispatchToolCalls(const std::shared_ptr<Impl>& state, const std::vector<ToolCall>& tool_calls,
                                const std::string& assistant_content);
  static void HandleLlmResponse(const std::shared_ptr<Impl>& state, const ChatCompletionResponse& response);
  static void PushEvent(const std::shared_ptr<Impl>& state, AgentEvent event);
  static void PushLoading(const std::shared_ptr<Impl>& state, bool loading);
  static void PushToolActivity(const std::shared_ptr<Impl>& state, const std::string& tool_name,
                               const std::string& status);
  static void PushAssistantReady(const std::shared_ptr<Impl>& state, const std::string& entry_id,
                                 const std::string& text);
  static void PushError(const std::shared_ptr<Impl>& state, const std::string& message);
  static void FinishTurn(const std::shared_ptr<Impl>& state);

  std::shared_ptr<Impl> impl_;
};

} // namespace pbr
